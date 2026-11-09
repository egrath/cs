/* command.c - the ';' command set: Auto Copy Delete Edge Format Goto Help
               Insert Load Merge Order Print Quit Recalc Save What ? = *   */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <curses.h>
#include "cs.h"

/* helpers defined in other modules */
extern void cs_help_cmd(void);      /* help.c */
extern void cs_print_cmd(void);     /* print.c */
extern int  cs_save_dialog(void);   /* fileio.c */
extern int  cs_load_dialog(int merge); /* fileio.c */
extern int  cs_file_password(const char *path, char *pw, int max);
extern int  cs_prompt_hidden(const char *pr, char *out, int max);

static void redraw(void);
void cs_command_menu(void);   /* forward: auto entry loop needs it */

static const char *collab(const char *w, int col, const char *rest);
int cs_cell_x(int col);   /* exported geometry helper (screen.c) */

void cs_goto_cmd(void)
{
    char buf[32];
    int c, r;
    if (cs_prompt_str("Goto>", "A1", buf, (int) sizeof(buf)) < 0) return;
    if (!cs_parse_coord(buf, &c, &r)) { cs_msg("Bad coord"); return; }
    curCol = c; curRow = r;
    if (curCol >= lockCols && curCol < winCol) winCol = curCol;
    if (curRow >= lockRows && curRow < winRow) winRow = curRow;
    while (cs_cell_x(curCol) + colw[curCol] > cs_scrw() - 1 && winCol < curCol)
        winCol++;
    cs_stay_visible();   /* keep the target row visible in the window */
    redraw();
}

/* ============================ helpers ============================== */
static Cell *clone_cells(int c1, int r1, int c2, int r2, int *nret)
{
    int sh = c2 - c1 + 1, sv = r2 - r1 + 1, i, n = sh * sv;
    Cell *tmp;
    tmp = (Cell *) malloc((size_t) n * sizeof(Cell));
    if (!tmp) { *nret = 0; return NULL; }
    for (i = 0; i < n; i++) {
        tmp[i] = cells[r1 + i / sh][c1 + i % sh];
        if (tmp[i].cont)   tmp[i].cont   = cs_strdup(tmp[i].cont);
        if (tmp[i].rep)    tmp[i].rep    = cs_strdup(tmp[i].rep);
        if (tmp[i].strres) tmp[i].strres = cs_strdup(tmp[i].strres);
    }
    *nret = n;
    return tmp;
}

static void free_clones(Cell *t, int n)
{
    int i;
    if (!t) return;
    for (i = 0; i < n; i++) {
        free(t[i].cont); free(t[i].rep); free(t[i].strres);
    }
    free(t);
}

static void copy_one(Cell *dst, const Cell *src, int rel, int dc, int dr)
{
    if (src->type == CE_NUM && src->cont) {
        Csn val;
        char *str = NULL;
        char *use;
        int err;
        if (rel && !cs_is_inputf(src))
            use = cs_adjust_refs(src->cont, dc, dr);
        else
            use = cs_strdup(src->cont);
        if (!use) return;
        err = cs_eval_formula(use, &val, &str);
        cs_free_cell(dst);
        dst->type = CE_NUM;
        dst->just = 'R';
        dst->cont = use;
        csn_copy(&dst->val, &val);       /* zeroed by eval on error */
        dst->iserr = err ? 1 : 0;
        dst->prec = src->prec;
        dst->strres = str;
    } else if (src->type == CE_TEXT) {
        cs_free_cell(dst);
        dst->type = CE_TEXT;
        dst->just = src->just;
        dst->cont = cs_strdup(src->cont ? src->cont : "");
        dst->rep = (src->just == 'W' && src->rep) ? cs_strdup(src->rep) : NULL;
        csn_zero(&dst->val); dst->iserr = 0;
    } else {
        cs_free_cell(dst);
        dst->type = CE_ALLOC;
        dst->cont = NULL;
    }
}

/* ============================= Copy ================================ */
static void copy_cmd(void)
{
    char from[40], to[40];
    int c1, r1, c2, r2, d1, e1, d2, e2, sw, sh, dw, dh, rel, rep, nfill;
    Cell *tmp;
    int n, i;

    if (cs_prompt_str("from coord (>coord)", "", from, (int) sizeof(from)) < 0) return;
    if (cs_prompt_str("to coord (>coord)", "", to, (int) sizeof(to)) < 0) return;

    /* rep: 1x1 source repeated across a destination range (manual 5.9
     * "from one cell into a range of cells") */
    rep = 0; nfill = 0;
    if (cs_parse_range(from, &c1, &r1, &c2, &r2)) {
        sw = c2 - c1 + 1; sh = r2 - r1 + 1;
        if (strchr(to, '>')) {
            if (!cs_parse_range(to, &d1, &e1, &d2, &e2)) { cs_msg("bad range coord"); return; }
            dw = d2 - d1 + 1; dh = e2 - e1 + 1;
            if (sw == 1 && sh == 1) {
                rep = 1;
                nfill = dw * dh;
            } else if (dw == sw && dh == sh) {
                nfill = dw * dh;
            } else if (dw * dh == sw * sh) {
                /* equal counts but different geometry: the copy is not
                 * in a straight vertical or straight horizontal line */
                cs_msg("not along row/column"); return;
            } else {
                cs_msg("sizes don't agree");
                return;
            }
        } else {
            if (!cs_parse_coord(to, &d1, &e1)) { cs_msg("bad coord"); return; }
            d2 = d1 + sw - 1; e2 = e1 + sh - 1;
            nfill = sw * sh;
        }
    } else if (cs_parse_coord(from, &c1, &r1)) {
        c2 = c1; r2 = r1; sw = 1; sh = 1;
        if (strchr(to, '>')) {
            if (!cs_parse_range(to, &d1, &e1, &d2, &e2)) { cs_msg("bad range coord"); return; }
            rep = 1;
            nfill = (d2 - d1 + 1) * (e2 - e1 + 1);
        } else {
            if (!cs_parse_coord(to, &d1, &e1)) { cs_msg("bad coord"); return; }
            d2 = d1; e2 = e1; nfill = 1;
        }
    } else { cs_msg("bad coord"); return; }

    if (rep) { dw = d2 - d1 + 1; dh = e2 - e1 + 1; }
    else     { dw = d2 - d1 + 1; dh = e2 - e1 + 1; }
    if (d2 >= CS_MAXCOL || e2 >= CS_MAXROW || d1 < 0 || e1 < 0) {
        cs_msg("bad coord"); return;
    }
    if (!rep && nfill <= 0) nfill = 1;
    if (d2 >= CS_MAXCOL || e2 >= CS_MAXROW || d1 < 0 || e1 < 0) {
        cs_msg("bad coord"); return;
    }
    rel = 0;
    for (i = 0; i < (rep ? 1 : sw * sh); i++) {
        Cell *s = cs_cell(c1 + i % sw, r1 + i / sh);
        if (s && s->type == CE_NUM) {
            int ch = cs_prompt_char("R)elative or N)o adjustment");
            redraw();
            if (ch == 27) return;
            rel = (ch == 'R' || ch == 'r');
            break;
        }
    }
    tmp = clone_cells(c1, r1, c2, r2, &n);
    if (!tmp || !n) { free_clones(tmp, n); cs_msg("MEMORY IS TOO LOW"); return; }
    for (i = 0; i < (rep ? nfill : n); i++) {
        int sc = rep ? c1 : (c1 + i % sw);
        int sr = rep ? r1 : (r1 + i / sw);
        int dc = d1 + i % dw, dr = e1 + i / dw;
        copy_one(cs_cell(dc, dr), &tmp[rep ? 0 : i], rel, dc - sc, dr - sr);
    }
    free_clones(tmp, n);
    cs_recount_entries();
    redraw();
}

/* ====================== Insert row/column ========================== */
static void shift_all_refs(int iscol, int at, int insert)
{
    int r, c;
    for (r = 0; r < CS_MAXROW; r++)
        for (c = 0; c < CS_MAXCOL; c++) {
            Cell *cl = &cells[r][c];
            char *nu;
            if (!cl->cont) continue;
            nu = cs_shift_refs(cl->cont, iscol, at, insert);
            if (nu) {
                free(cl->cont);
                cl->cont = nu;
            }
        }
}

static void blank_cell_rowcol(int r, int c)
{
    cells[r][c].type = CE_EMPTY;
    cells[r][c].cont = NULL;
    cells[r][c].rep = NULL;
    cells[r][c].strres = NULL;
    cells[r][c].just = 'L';
    csn_zero(&cells[r][c].val);
    cells[r][c].iserr = 0;
    cells[r][c].formf = 0;
    cells[r][c].prec = -1;
}

static void insert_cmd(void)
{
    int ch, at, r, c;
    ch = cs_prompt_char("Insert: R)ow C)olumn");
    redraw();
    if (ch == 27) return;
    if (ch == 'R' || ch == 'r') {
        at = curRow;
        shift_all_refs(0, at, 1);
        for (r = CS_MAXROW - 1; r > at; r--)
            for (c = 0; c < CS_MAXCOL; c++)
                cells[r][c] = cells[r - 1][c];
        for (c = 0; c < CS_MAXCOL; c++) blank_cell_rowcol(at, c);
    } else if (ch == 'C' || ch == 'c') {
        at = curCol;
        shift_all_refs(1, at, 1);
        for (r = 0; r < CS_MAXROW; r++)
            for (c = CS_MAXCOL - 1; c > at; c--)
                cells[r][c] = cells[r][c - 1];
        for (r = 0; r < CS_MAXROW; r++) blank_cell_rowcol(r, at);
    } else return;
    redraw();
}

/* ====================== Delete row/column/all ====================== */
static int dep_check(int iscol, int at, char *list, int listmax)
{
    int r, c, found = 0;
    list[0] = '\0';
    for (r = 0; r < CS_MAXROW; r++)
        for (c = 0; c < CS_MAXCOL; c++) {
            Cell *cl = &cells[r][c];
            char buf[10];
            if (iscol ? c == at : r == at) continue;
            if (!cl->cont || cl->type != CE_NUM) continue;
            {
                const char *s = cl->cont;
                int hit = 0, cc, rr, inq = 0;
                while (*s && !hit) {
                    if (*s == '"') { inq = !inq; s++; continue; }
                    if (*s == '\\' && !inq) break;   /* comment: no refs */
                    if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) {
                        const char *st = s;
                        int nl = 0;
                        while ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) { s++; nl++; }
                        if (nl <= 2 && *s >= '0' && *s <= '9') {
                            while (*s >= '0' && *s <= '9') s++;
                            if (*s == '!' || *s == '>' || *s == ',' || *s == ')' ||
                                *s == ' ' || *s == ':' || *s == '+' || *s == '-' ||
                                *s == '*' || *s == '/' || *s == '%' || *s == '\0' ||
                                *s == '<' || *s == '=') {
                                char tmp[8];
                                int i;
                                for (i = 0; i < (s - st); i++)
                                    tmp[i] = (char) ((st[i] >= 'a' && st[i] <= 'z') ? st[i] - 32 : st[i]);
                                tmp[i] = '\0';
                                if (cs_parse_coord(tmp, &cc, &rr) &&
                                    (iscol ? cc == at : rr == at))
                                    hit = 1;
                            }
                            continue;
                        }
                        s = st + 1;
                        continue;
                    }
                    s++;
                }
                if (hit) {
                    char cn[4];
                    if (list[0] && (int) strlen(list) + 2 < listmax)
                        strcat(list, ", ");
                    cs_col_name(c, cn);
                    sprintf(buf, "%s%d", cn, r + 1);
                    if ((int) strlen(list) + (int) strlen(buf) < listmax - 2)
                        strcat(list, buf);
                    found = 1;
                }
            }
        }
    return found;
}

static void delete_cmd(void)
{
    char list[128];
    int ch, r, c;
    ch = cs_prompt_char("Delete: A)ll R)ow C)olumn E)ntry");
    redraw();
    if (ch == 27) return;
    if (ch == 'E' || ch == 'e') {
        cs_free_cell(cs_cell(curCol, curRow));
        redraw();
        return;
    }
    if (ch == 'R' || ch == 'r') {
        if (dep_check(0, curRow, list, (int) sizeof(list))) {
            cs_msg("ERROR: would delete ref(s) at %s", list);
            return;
        }
        ch = cs_prompt_yn("verify Y/N - ");
        redraw();
        if (ch != 1) return;
        shift_all_refs(0, curRow, 0);
        for (r = curRow; r < CS_MAXROW - 1; r++)
            for (c = 0; c < CS_MAXCOL; c++)
                cells[r][c] = cells[r + 1][c];
        for (c = 0; c < CS_MAXCOL; c++)
            cs_free_cell(&cells[CS_MAXROW - 1][c]);
    } else if (ch == 'C' || ch == 'c') {
        if (dep_check(1, curCol, list, (int) sizeof(list))) {
            cs_msg("ERROR: would delete ref(s) at %s", list);
            return;
        }
        ch = cs_prompt_yn("verify Y/N - ");
        redraw();
        if (ch != 1) return;
        shift_all_refs(1, curCol, 0);
        for (r = 0; r < CS_MAXROW; r++)
            for (c = curCol; c < CS_MAXCOL - 1; c++)
                cells[r][c] = cells[r][c + 1];
        for (r = 0; r < CS_MAXROW; r++)
            cs_free_cell(&cells[r][CS_MAXCOL - 1]);
    } else if (ch == 'A' || ch == 'a') {
        ch = cs_prompt_yn("verify Y/N - ");
        redraw();
        if (ch != 1) return;
        cs_clear_all();
        fname[0] = '\0';
        curCol = curRow = winCol = winRow = 0;
        lockCols = lockRows = 0;
    } else return;
    redraw();
}

/* ============================ Format =============================== */
static void format_cmd(void)
{
    int ch;
    char pw[24], tmpb[24];
    char label[72];
    Cell *cur = cs_cell(curCol, curRow);

    sprintf(pw, "%d", colw[curCol]);
    sprintf(label, "P)recision (%d) or W)idth (%d) or F)orm mode (%s)",
            colp[curCol], colw[curCol], cur->formf ? "set" : "clear");
    ch = cs_prompt_char(label);
    redraw();
    if (ch == 27) return;
    if (ch == 'W' || ch == 'w') {
        int w;
        if (cs_prompt_str(collab("Column", curCol, "width(3 - 63)"), pw, tmpb, 20) < 0) return;
        w = atoi(tmpb);
        if (w < 3 || w > 63) { cs_msg("Width must be 3 - 63"); return; }
        colw[curCol] = w;
    } else if (ch == 'P' || ch == 'p') {
        int p;
        sprintf(pw, "%d", colp[curCol]);
        if (cs_prompt_str(collab("Column", curCol, "precision(0 - 12)"), pw, tmpb, 20) < 0) return;
        if (tmpb[0] == 'E' || tmpb[0] == 'e') {
            p = atoi(tmpb + 1);
            if (p < 0 || p > CS_MAX_P) { cs_msg("Prec. must be 0 - 12"); return; }
            if (cur->type == CE_EMPTY) {
                /* per-cell format needs the cell: manual "can't create -
                 * not enough memory to set format precision" */
                if (!cs_ensure_room()) { cs_msg("can't create"); return; }
                cur->type = CE_ALLOC;
                cs_recount_entries();
            }
            cur->prec = p;
        } else {
            p = atoi(tmpb);
            if (p < 0 || p > CS_MAX_P) { cs_msg("Prec. must be 0 - 12"); return; }
            colp[curCol] = p;
        }
    } else if (ch == 'F' || ch == 'f') {
        cur->formf = !cur->formf;
        if (cur->formf) {
            if (cur->type == CE_EMPTY) {
                /* mark the cell so the type line reads
                 * "(empty, but allocated)" — manual 11.3 form mode */
                cur->type = CE_ALLOC;
                cs_recount_entries();
            }
            cs_msg("Form mode (set)");
        } else {
            if (cur->type == CE_ALLOC) {
                cs_free_cell(cur);      /* releases it back to CE_EMPTY */
                cs_recount_entries();
            }
            cs_msg("Form mode (clear)");
        }
        return;
    } else return;
    redraw();
}

static const char *collab(const char *w, int col, const char *rest)
{
    static char buf[40];
    char cn[4];
    cs_col_name(col, cn);
    sprintf(buf, "%s %s %s", w, cn, rest);
    return buf;
}

/* ========================= Recalculate ============================= */
static void recalc_cmd(void)
{
    int ch = cs_prompt_char("Recalculate - A)ll or E)ntry");
    redraw();
    if (ch == 27) return;
    if (ch == 'A' || ch == 'a')
        cs_recalc_all();
    else if (ch == 'E' || ch == 'e') {
        cs_nerrlist = 0;              /* single-cell run: fresh queue */
        cs_recalc_entry(curCol, curRow);
    }
    else return;
    redraw();
    if (cs_nerrlist) {
        /* the manual's error replay: the ?n? cells report one after
         * another, confirmed with Space (see main.c walker) */
        cs_sticky_err(cs_errlist[0].code,
                      cs_cell(cs_errlist[0].col, cs_errlist[0].row)->cont,
                      cs_errlist[0].col, cs_errlist[0].row);
        cs_set_errpend(1);
    }
}

/* ======================= What / Lock / misc ======================== */
static void what_cmd(void)
{
    const char *rt = "-", *ct = "-";
    Cell *c = cs_cell(curCol, 0);
    if (c && c->type == CE_TEXT && c->cont && c->cont[0]) rt = c->cont;
    c = cs_cell(0, curRow);
    if (c && c->type == CE_TEXT && c->cont && c->cont[0]) ct = c->cont;
    cs_msg("row, column  = %s,%s", rt, ct);
}

void cs_lock_cmd(void)
{
    int ch = cs_prompt_char("Lock: R)ow C)ol B)oth");
    redraw();
    if (ch == 27) return;
    if (ch == 'B' || ch == 'b') {
        if (lockRows || lockCols) { lockRows = 0; lockCols = 0; }
        else { lockRows = curRow + 1; lockCols = curCol + 1; }
    } else if (ch == 'R' || ch == 'r')
        lockRows = lockRows ? 0 : curRow + 1;   /* lock rows 1..cursor row */
    else if (ch == 'C' || ch == 'c')
        lockCols = lockCols ? 0 : curCol + 1;   /* cols A..cursor */
    else return;
    redraw();
}

static void storage_cmd(void)
{
    cs_msg("Room for %d entries", CS_BUDGET - entriesUsed);
}

/* ============================= Auto ================================ */
static void auto_cmd(void)
{
    int seq[CS_MAXROW * CS_MAXCOL / 2 + 2];
    int nseq = 0;
    int r, c, i;
    char buf[CS_EDITMAX + 1];   /* was buf[2]! second char typed went out
                                 * of bounds and smashed loop state */
    int k;

    for (r = 0; r < CS_MAXROW; r++)
        for (c = 0; c < CS_MAXCOL; c++)
            if (cells[r][c].formf && nseq < (int)(sizeof(seq) / sizeof(int)) - 1) {
                seq[nseq++] = r * CS_MAXCOL + c;
            }
    if (nseq == 0) { cs_msg("no form flags"); return; }
    g_edit[0] = '\0';
    g_editlen = 0;
    g_entry_num = 1;
    g_editstate = EDIT_EDITING;   /* show "edit: <text>" during Auto */
    for (i = 0; i < nseq; i++) {
        int cr = seq[i] / CS_MAXCOL, cc = seq[i] % CS_MAXCOL;
        curCol = cc; curRow = cr;
        g_entryrow = cr;          /* "current:" must show the stop cell */
        g_entrycol = cc;
        redraw();
        cs_msg("Form Control Mode");   /* bottom-left, manual 11.3 */
        /* entry loop: existing cell values are NOT cleared - a bare
         * RETURN keeps the stored value and jumps to the next stop */
        buf[0] = '\0';
        k = 0;
        for (;;) {
            int ch = getch();
            if (cs_was_resize(ch)) continue;
            if (ch == 27) {
                g_editstate = EDIT_IDLE;
                g_edit[0] = '\0';
                g_editlen = 0;
                redraw();
                cs_msg("Auto aborted");   /* paint after the redraw */
                return;
            }
            if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) break;
            if (ch == ';') {
                /* ';' is the command lead-in, never cell data
                 * (normal entry: main.c case ';' opens the menu) */
                cs_command_menu();
                continue;
            }
            if (ch == KEY_BACKSPACE || ch == 8 || ch == 127) {
                if (k > 0) buf[--k] = '\0';
            } else if (ch == 94) {
                /* '^' TEXT/NUMERIC toggle (manual 6.4), as in normal entry */
                g_entry_num = !g_entry_num;
            } else if (ch >= 32 && ch < 127 && k < CS_EDITMAX - 1) {
                /* '"' is a literal character, not a toggle */
                if (k == 0 && g_entry_num != ((ch >= '0' && ch <= '9') ||
                    ch == '+' || ch == '-' || ch == '(' || ch == '.'))
                    g_entry_num = ((ch >= '0' && ch <= '9') || ch == '+' ||
                                   ch == '-' || ch == '(' || ch == '.');
                buf[k++] = (char) ch;
                buf[k] = '\0';
            }
            g_editlen = k;
            strncpy(g_edit, buf, CS_EDITMAX);
            g_edit[CS_EDITMAX - 1] = '\0';
            redraw();
        }
        if (k > 0) {
            int ec = cs_commit_entry(cc, cr, buf);
            /* numeric eval error -> message (space is data inside Auto:
             * no "hit space" confirm here) */
            if (cells[cr][cc].iserr)
                cs_msg("Error %d-> %s", ec <= 0 ? 0 : ec, buf);
        }
        g_edit[0] = '\0';
        g_editlen = 0;
    }
    g_editstate = EDIT_IDLE;
    cs_recalc_all();
    redraw();
    cs_msg("Auto complete");   /* paint after the final redraw */
}

/* ========================== Quit / menu ============================ */
static void quit_cmd(void)
{
    int ch = cs_prompt_yn("verify Y/N - ");
    if (ch == 1) {
        endwin();
        exit(0);
    }
    redraw();
}

static void redraw(void) { cs_draw(); }

void cs_command_menu(void)
{
    int ch = cs_prompt_char("Command > ");
    redraw();
    if (ch == 27 || ch == '\n' || ch == '\r') return;
    switch (toupper(ch)) {
    case 'A': auto_cmd(); break;
    case 'C': copy_cmd(); break;
    case 'D': delete_cmd(); break;
    case 'E': winCol = curCol; winRow = curRow; redraw(); break;
    case 'F': format_cmd(); break;
    case 'G': cs_goto_cmd(); break;
    case 'H': cs_help_cmd(); break;
    case 'I': insert_cmd(); break;
    case 'L': cs_load_dialog(0); break;
    case 'M': cs_load_dialog(1); break;
    case 'O': dirTB = !dirTB; redraw(); break;
    case 'P': cs_print_cmd(); break;
    case 'Q': quit_cmd(); break;
    case 'R': recalc_cmd(); break;
    case 'S': cs_save_dialog(); break;
    case 'W': what_cmd(); break;
    case '?': storage_cmd(); break;
    case '=': cs_lock_cmd(); break;
    case '*':
        extended = !extended;
        cs_stay_visible();   /* keep the cursor inside the new row count */
        redraw();
        break;
    default: break;
    }
}
