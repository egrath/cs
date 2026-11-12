/* cells.c - cell storage, coordinates, recalculation */
#include <stdlib.h>
#include <string.h>
#include "cs.h"

Cell cells[CS_MAXROW][CS_MAXCOL];
int  colw[CS_MAXCOL];
int  colp[CS_MAXCOL];
int  curCol = 0, curRow = 0;
int  winCol = 0, winRow = 0;
int  dirTB = 0;
int  extended = 0;
int  lockCols = 0, lockRows = 0;
int  showForm = 0;
char fname[40] = "";
char cs_cmdfile[40] = "";
int  entriesUsed = 0;
int  orderTB = 0;
int  cs_headless = 0;

/* regression state, updated whenever a REGR() is evaluated */
int    g_reg_ok = 0;
Csn    g_reg_slope, g_reg_icept, g_reg_meany;

void cs_reg_reset(void)
{
    g_reg_ok = 0;
    csn_zero(&g_reg_slope);
    csn_zero(&g_reg_icept);
    csn_zero(&g_reg_meany);
}

char g_edit[CS_EDITMAX];
int  g_editlen = 0;
int  g_editstate = EDIT_IDLE;
int  g_entry_num = 1;
int  g_entryrow = 0, g_entrycol = 0;

Cell *cs_cell(int col, int rw)
{
    if (col < 0 || col >= CS_MAXCOL || rw < 0 || rw >= CS_MAXROW)
        return NULL;
    return &cells[rw][col];
}

char *cs_strdup(const char *s)
{
    char *p = (char *) malloc(strlen(s) + 1);
    if (p) strcpy(p, s);
    return p;
}

void cs_col_name(int col, char *buf)
{
    if (col < 26) {
        buf[0] = (char) ('A' + col);
        buf[1] = '\0';
    } else {
        col -= 26;
        buf[0] = (char) ('A' + col / 26);
        buf[1] = (char) ('A' + col % 26);
        buf[2] = '\0';
    }
}

int cs_name_to_col(const char *s)
{
    int c = -1;
    if (s[0] >= 'A' && s[0] <= 'Z') {
        c = s[0] - 'A';
        if (s[1] >= 'A' && s[1] <= 'Z')
            c = 26 + c * 26 + (s[1] - 'A');
    }
    return c;
}

/* parse "B7" -> col 1 row 6.  returns 1 ok, 0 bad */
/* the column letter 'A'..'Z' / 'a'..'z' as 0..25, or -1 */
static int col_letter(char ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a';
    return -1;
}

int cs_parse_coord(const char *s, int *col, int *row)
{
    int c = 0, r = 0, n = 0, v;
    if (!s || !*s) return 0;
    /* letters accept either case - coordinate fields (Goto, TAB, copy
     * ranges, save/load corners) fold like the formula parser and the
     * reference scanners already do (user request 2026-11) */
    v = col_letter(s[0]);
    if (v < 0) return 0;
    c = v;
    s++;
    v = col_letter(s[0]);
    if (v >= 0) {
        c = 26 + c * 26 + v;
        s++;
    }
    if (col_letter(s[0]) >= 0) return 0;   /* three or more letters */
    while (*s >= '0' && *s <= '9') { r = r * 10 + (*s - '0'); s++; n++; }
    if (!n || *s) return 0;
    if (r < 1 || r > CS_MAXROW || c < 0 || c >= CS_MAXCOL) return 0;
    *col = c; *row = r - 1;
    return 1;
}

int cs_parse_range(const char *s, int *c1, int *r1, int *c2, int *r2)
{
    char buf[32];
    const char *p;
    int i;
    p = s;
    i = 0;
    while (*p && *p != '>' && i < 15) buf[i++] = *p++;
    buf[i] = '\0';
    if (*p != '>') return 0;
    p++;
    if (!cs_parse_coord(buf, c1, r1)) return 0;
    if (!cs_parse_coord(p, c2, r2)) return 0;
    cs_range_norm(c1, r1, c2, r2);
    return 1;
}

void cs_range_norm(int *c1, int *r1, int *c2, int *r2)
{
    int t;
    if (*c1 > *c2) { t = *c1; *c1 = *c2; *c2 = t; }
    if (*r1 > *r2) { t = *r1; *r1 = *r2; *r2 = t; }
}

void cs_set_text(Cell *c, const char *cont, char just)
{
    cs_free_cell(c);
    c->type = CE_TEXT;
    c->just = just;
    if (just == 'W') c->rep = cs_strdup(cont);
    c->cont = cs_strdup(cont);
    csn_zero(&c->val);
    c->iserr = 0;
    c->strres = NULL;
    entriesUsed++;
}

void cs_set_num(Cell *c, const char *cont, const Csn *val, int iserr, const char *strres)
{
    cs_free_cell(c);
    c->type = CE_NUM;
    c->just = 'R';
    c->cont = cs_strdup(cont);
    csn_copy(&c->val, val);
    c->iserr = iserr;
    if (strres) c->strres = cs_strdup(strres);
    entriesUsed++;
}

void cs_free_cell(Cell *c)
{
    if (!c) return;
    if (c->type != CE_EMPTY) entriesUsed--;
    if (c->cont) { free(c->cont); c->cont = NULL; }
    if (c->rep) { free(c->rep); c->rep = NULL; }
    if (c->strres) { free(c->strres); c->strres = NULL; }
    c->type = CE_EMPTY;
    c->just = 'L';
    csn_zero(&c->val);
    c->iserr = 0;
    /* NOTE: formf deliberately survives - the user's form course must
     * keep its stop-cells across data re-entry (manual: ";A ... is very
     * useful when certain entries are subject to frequent changes").
     * cs_clear_all() resets the flags explicitly. */
    c->prec = -1;
}

void cs_clear_all(void)
{
    int r, c;
    for (r = 0; r < CS_MAXROW; r++)
        for (c = 0; c < CS_MAXCOL; c++)
            cs_free_cell(&cells[r][c]);
    for (r = 0; r < CS_MAXROW; r++)
        for (c = 0; c < CS_MAXCOL; c++)
            cells[r][c].formf = 0;
    for (c = 0; c < CS_MAXCOL; c++) { colw[c] = CS_DEF_W; colp[c] = CS_DEF_P; }
    for (r = 0; r < CS_MAXROW; r++)
        for (c = 0; c < CS_MAXCOL; c++) cells[r][c].prec = -1;
}

int cs_ensure_room(void)
{
    if (entriesUsed >= CS_BUDGET) return 0;
    return 1;
}

/* re-count real entries (copy writes directly into cells and can drift
 * from the count; the auto/memory warning and ";? Room for" rely on it) */
void cs_recount_entries(void)
{
    int r, c, n = 0;
    for (r = 0; r < CS_MAXROW; r++)
        for (c = 0; c < CS_MAXCOL; c++)
            if (cells[r][c].type != CE_EMPTY) n++;
    entriesUsed = n;
}

/* ---------------------------------------------------------------+
 | reference scanner: callback for every non-locked coordinate     |
 | reference (cell or range end) found in a formula string         |
 +--------------------------------------------------------------- */
int cs_scan_refs(const char *s, void (*cb)(int, int, void *), void *ctx)
{
    int col, row, inq = 0;
    const char *p = s;
    while (*p) {
        if (*p == '"') { inq = !inq; p++; continue; }
        if (*p == '\\' && !inq) return 0;   /* '\' comment: no refs follow */
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
            const char *st = p;
            int nl = 0;
            while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) { p++; nl++; }
            if (nl <= 2 && (*p >= '0' && *p <= '9')) {
                /* could be coordinate or unknown function name */
                while (*p >= '0' && *p <= '9') p++;
                if (*p == '!' || *p == '>' || *p == ',' || *p == ')' ||
                    *p == ' ' || *p == ':' || *p == '+' || *p == '-' ||
                    *p == '*' || *p == '/' || *p == '%' || *p == '\0' ||
                    *p == '<' || *p == '=') {
                    char tmp[8];
                    int i, ok;
                    for (i = 0; st + i < p; i++) tmp[i] = (char) ((st[i] >= 'a' && st[i] <= 'z') ? st[i] - 32 : st[i]);
                    tmp[i] = '\0';
                    ok = cs_parse_coord(tmp, &col, &row);
                    if (ok && cb) cb(col, row, ctx);
                    if (*p == '!') p++;
                    continue;
                }
            }
            /* not a coordinate: skip the whole word (function name) */
            p = st + 1;
            continue;
        }
        p++;
    }
    return 0;
}

/* shift every (non-bang) reference by (dc,dr); caller owns result */
char *cs_adjust_refs(const char *s, int dc, int dr)
{
    char *out = (char *) malloc(strlen(s) * 2 + 16);
    char *o = out;
    const char *p = s;
    int inq = 0;
    if (!out) return NULL;
    while (*p) {
        if (*p == '"') { inq = !inq; *o++ = *p++; continue; }
        if (*p == '\\' && !inq) {
            /* '\' comment: the rest is copied verbatim */
            size_t rest = strlen(p);
            memcpy(o, p, rest);
            o += rest;
            break;
        }
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
            const char *st = p;
            int nl = 0;
            while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) { p++; nl++; }
            if (nl <= 2 && *p >= '0' && *p <= '9') {
                while (*p >= '0' && *p <= '9') p++;
                if (*p == '!' || *p == '>' || *p == ',' || *p == ')' ||
                    *p == ' ' || *p == ':' || *p == '+' || *p == '-' ||
                    *p == '*' || *p == '/' || *p == '%' || *p == '\0' ||
                    *p == '<' || *p == '=') {
                    char tmp[8], tmp2[8];
                    int i, newc, newr, ok;
                    for (i = 0; st + i < p; i++) tmp[i] = (char) ((st[i] >= 'a' && st[i] <= 'z') ? st[i] - 32 : st[i]);
                    tmp[i] = '\0';
                    ok = cs_parse_coord(tmp, &newc, &newr);
                    if (ok && *p != '!') {
                        newc += dc; newr += dr;
                    }
                    if (*p == '!') p++;
                    if (ok) {
                        cs_col_name(newc, tmp2);
                        o += sprintf(o, "%s%d", tmp2, newr + 1);
                        if (*(p - 1) == '!') *o++ = '!';
                    } else {
                        memcpy(o, st, (size_t) (p - st));
                        o += p - st;
                    }
                    continue;
                }
            }
            /* not a coordinate: a word (function name, text) - copy the
             * run verbatim.  The letter run is consumed from st to p. */
            memcpy(o, st, (size_t) (p - st));
            o += p - st;
        }
        *o++ = *p++;
    }
    *o = '\0';
    return out;
}

/* shift every (non-bang) reference row/col across an insert/delete line.
   iscol: 0 -> rows, 1 -> cols.  insert: 1 for insert (+1 at & beyond),
   0 for delete (-1 strictly beyond).  Returns fresh string or NULL if
   nothing was changed. */
/* shift every coordinate reference in s by `delta` (iscol: columns vs
 * rows).  With ge != 0 references AT `at` shift too (insertion mode,
 * delta +1), otherwise only those beyond it (deletion mode). */
static char *shift_refs_core(const char *s, int iscol, int at, int delta, int ge)
{
    char *out = (char *) malloc(strlen(s) * 2 + 16);
    char *o = out;
    const char *p = s;
    int changed = 0, inq = 0;
    if (!out) return NULL;
    while (*p) {
        if (*p == '"') { inq = !inq; *o++ = *p++; continue; }
        if (*p == '\\' && !inq) {
            /* '\' comment: the rest is copied verbatim */
            size_t rest = strlen(p);
            memcpy(o, p, rest);
            o += rest;
            break;
        }
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
            const char *st = p;
            int nl = 0;
            while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) { p++; nl++; }
            if (nl <= 2 && *p >= '0' && *p <= '9') {
                while (*p >= '0' && *p <= '9') p++;
                if (*p == '!' || *p == '>' || *p == ',' || *p == ')' ||
                    *p == ' ' || *p == ':' || *p == '+' || *p == '-' ||
                    *p == '*' || *p == '/' || *p == '%' || *p == '\0' ||
                    *p == '<' || *p == '=') {
                    char tmp[8], tmp2[4];
                    int i, newc, newr, ok, bang;
                    for (i = 0; st + i < p; i++) tmp[i] = (char) ((st[i] >= 'a' && st[i] <= 'z') ? st[i] - 32 : st[i]);
                    tmp[i] = '\0';
                    ok = cs_parse_coord(tmp, &newc, &newr);
                    bang = *p == '!';
                    if (ok && !bang) {
                        int did = 0;
                        if (iscol) {
                            if (ge ? newc >= at : newc > at) { newc += delta; did = 1; }
                        } else {
                            if (ge ? newr >= at : newr > at) { newr += delta; did = 1; }
                        }
                        if (did && newc >= 0 && newc < CS_MAXCOL &&
                            newr >= 0 && newr < CS_MAXROW) {
                            cs_col_name(newc, tmp2);
                            o += sprintf(o, "%s%d", tmp2, newr + 1);
                            changed = 1;
                            if (bang) *o++ = '!';
                            if (*p == '!') p++;
                            continue;
                        }
                    }
                    memcpy(o, st, (size_t) (p - st));
                    o += p - st;
                    if (bang) *o++ = '!';
                    if (*p == '!') p++;
                    continue;
                }
            }
            /* not a coordinate: a word (fn name etc.) - copy verbatim */
            memcpy(o, st, (size_t) (p - st));
            o += p - st;
        }
        *o++ = *p++;
    }
    *o = '\0';
    if (!changed) { free(out); return NULL; }
    return out;
}

char *cs_shift_refs(const char *s, int iscol, int at, int insert)
{
    return shift_refs_core(s, iscol, at, insert ? 1 : -1, insert);
}

/* range-delete variant: every reference BEYOND `at` shifts by `delta`
 * (negative for deletions) */
char *cs_shift_refs_n(const char *s, int iscol, int at, int delta)
{
    return shift_refs_core(s, iscol, at, delta, 0);
}

/* ------------------------------------------------------------+
 | numeric formatting: truncate (not round) to prec decimals;   |
 | fall back to exponential form if it does not fit the width   |
 | (all digit work handled by the BCD engine)                   |
 +----------------------------------------------------------- */
void cs_fmt_num(const Csn *v, int width, int prec, int iserr, char *buf, int bufn)
{
    if (iserr) { sprintf(buf, "?n?"); return; }
    /* integer digits of the fixed form; if too wide for the buffer, go
     * straight to the exponential fallback */
    {
        int lead = v->exp10 - 1;
        int ipart = (v->ndig && lead >= 0) ? lead + 1 : 1;
        int need = ipart + (prec > 0 ? 1 + prec : 0) + 2;
        if (need > bufn - 1) {
            /* exponential form per the manual (e.g. 7.35E-6) */
            int frac = width - 6;
            if (frac < 0) frac = 0;
            while (frac > 0) {
                char e[64];
                csn_fmt_sci(v, frac, e, (int) sizeof(e));
                if ((int) strlen(e) <= width) break;
                frac--;
            }
            csn_fmt_sci(v, frac, buf, bufn);
            if ((int) strlen(buf) <= width) return;
        } else {
            csn_fmt_fixed(v, prec, buf, bufn);
            if (buf[0] != '\0' && (int) strlen(buf) <= width) return;
        }
    }
    /* exponential fallback (value does not fit even in "long" form) */
    {
        int frac = width - 6;
        if (frac < 0) frac = 0;
        while (frac > 0) {
            char e[64];
            csn_fmt_sci(v, frac, e, (int) sizeof(e));
            if ((int) strlen(e) <= width) break;
            frac--;
        }
        csn_fmt_sci(v, frac, buf, bufn);
        if ((int) strlen(buf) <= width) return;
    }
    /* does not fit in any form: the manual's "!n! -- column is not wide
     * enough" token */
    if (width > 0 && width >= 3 && width < bufn) {
        strcpy(buf, "!n!");
    } else buf[0] = '\0';
}
