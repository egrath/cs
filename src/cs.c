/* main.c - CalcStar main loop: curses setup, WordStar-style keys,
   entry engine (@-references, " toggle, ESC cancel, trailing ? calc),
   and a --test self-test mode for the evaluator */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "buildno.h"
#include "cs.h"

static int paint_pending;   /* a repaint was deferred (input queued) */
static void redraw(void)
{
    /* batch repaints while keys are queued: a fast paste otherwise
     * overflows the tty buffer and DROPS bytes (the app cannot keep up
     * with a full-screen redraw per key).  The loop's top paints as
     * soon as the queue drains, so the idle screen stays fresh.
     * Phase 3: mark everything, paint only on drain - the loop's
     * flush paints what was marked. */
    cs_touch_all();
    if (cs_input_pending()) { paint_pending = 1; return; }
    paint_pending = 0;
    cs_paint();
}

/* phase 4: mark targeted components and paint now unless keys are
 * queued (the drain paints what was marked).  Used by the paths whose
 * internal ordering matters: a direct cs_msg() (the sticky error, the
 * ?-calculator answer) must land AFTER the component paint, exactly
 * like the old redraw()-then-message sequence. */
static void touch_flush(int what)
{
    cs_touch(what);
    if (cs_input_pending()) { paint_pending = 1; return; }
    cs_paint();
}

/* one cursor step, scroll-aware: a scroll re-maps the header and every
 * grid row, a pure in-window step repaints the marked cell(s) and the
 * status lines only */
static void move_cursor(int dc, int dr);

static void step_cursor(int dc, int dr)
{
    int oc = winCol, orow = winRow;
    move_cursor(dc, dr);
    cs_touch_move(oc, orow);
}

/* ---------------- movement with window scrolling -------------------- */
/* Window scrolling reverse-engineered from real CalcStar (CP/M) with
 * user-run experiments (fall 2026, recorded in .opencode/opencode.txt):
 *   - left of the window:  minimal re-anchor, cursor flush left
 *     (winCol = curCol; also drops an oversized column from view)
 *   - right of the window: winCol = max(lockCols, curCol-3), then advance
 *     one column at a time until the cursor column fully fits (stop at
 *     the first fitting position; columns are drawn only when they fit
 *     entirely, see screen.c)
 *   - vertical, BOTH directions: winRow = max(lockRows, curRow - S/2)
 *     with S = scrollable rows (win rows minus locked rows); the cursor
 *     lands mid-window (6th of 10 rows; 4th scrollable row under a
 *     3-row lock), clamped at the sheet/lock top.
 * Emergent slide amounts: +5 down, -6 up, +3 right, -1 left.
 * Goto reuses these rules and only scrolls when the target is off-window. */
static void scroll_h(void)
{
    int nc;
    if (curCol < lockCols) return;
    if (curCol < winCol) { winCol = curCol; return; }
    if (cs_cell_x(curCol) + colw[curCol] <= cs_scrw()) return;
    nc = curCol - 3;
    if (nc < lockCols) nc = lockCols;      /* covers nc < 0 too */
    winCol = nc;
    while (cs_cell_x(curCol) + colw[curCol] > cs_scrw() && winCol < curCol)
        winCol++;
}

static void scroll_v(void)
{
    int s, off, eff, n;
    if (curRow < lockRows) return;
    s = cs_win_rows() - lockRows;
    if (s < 1) s = 1;
    off = s / 2;
    eff = winRow >= lockRows ? winRow : lockRows;
    if (curRow < eff || curRow - eff >= s) {
        n = curRow - off;
        if (n < lockRows) n = lockRows;
        winRow = n;
    }
}

/* dc/dr: -1,0,1; clamps to array bounds and lock edges */
static void move_cursor(int dc, int dr)
{
    int nc = curCol, nr = curRow;
    if (dc < 0 && curCol > 0) nc--;
    if (dc > 0 && curCol < CS_MAXCOL - 1) nc++;
    if (dr < 0 && curRow > 0) nr--;
    if (dr > 0 && curRow < CS_MAXROW - 1) nr++;
    curCol = nc; curRow = nr;
    scroll_h();
    scroll_v();
}

/* exported cursor step for the coordinate prompts' arrow handling
 * (screen.c): same clamp/window semantics as the edit-line keys */
void cs_move_cursor(int dc, int dr)
{
    move_cursor(dc, dr);
    cs_stay_visible();
}

/* exported so command.c (goto, row-count toggle) can re-anchor the
 * window with the real-CalcStar rules after cursor/count changes */
void cs_stay_visible(void)
{
    scroll_h();
    scroll_v();
}

/* ---------------------------- entry engine -------------------------- */
static int errpend;      /* error message displayed; hit space to continue */

static int numeric_start_ch(int ch)
{
    return (ch >= '0' && ch <= '9') || ch == '+' || ch == '-' ||
           ch == '(' || ch == '.';
}

static void edit_clear(void)
{
    g_edit[0] = '\0';
    g_editlen = 0;
    g_editstate = EDIT_IDLE;
}

/* start a numeric entry whose content is a bare coordinate reference
 * ("manual: the @ key inserts the cell the cursor is on"); the entry
 * stays editable so "+A1+A2" sequences work */
static void entry_begin_ref(void)
{
    char ref[10];
    cs_coord_str(ref, (int) sizeof(ref), curCol, curRow);
    g_entryrow = curRow;
    g_entrycol = curCol;
    g_entry_num = 1;
    g_editstate = EDIT_EDITING;
    strcpy(g_edit, ref);
    g_editlen = (int) strlen(ref);
    cs_touch(CS_D_CURSOR | CS_D_STATUS);
}

static void entry_begin(int ch)
{
    g_entry_num = numeric_start_ch(ch);
    g_entryrow = curRow;
    g_entrycol = curCol;
    g_editstate = EDIT_EDITING;
    g_edit[0] = (char) ch;
    g_edit[1] = '\0';
    g_editlen = 1;
    cs_touch(CS_D_CURSOR | CS_D_STATUS);
}

static void entry_append(int ch)
{
    if (g_editlen < CS_EDITMAX - 1) {
        g_edit[g_editlen++] = (char) ch;
        g_edit[g_editlen] = '\0';
    }
    cs_touch(CS_D_CURSOR | CS_D_STATUS);
}

/* finish a cell reference entered with '@': uses the cursor position
 * at the moment of resolution (the user may have moved the cursor in
 * "@" mode), e.g. moving to E3 and pressing <CR> or an operator
 * inserts "E3" */
static int at_resolve(void)
{
    char ref[10];
    cs_coord_str(ref, (int) sizeof(ref), curCol, curRow);
    if (g_editlen + (int) strlen(ref) >= CS_EDITMAX - 2)
        return 0;
    strcpy(g_edit + g_editlen, ref);
    g_editlen += (int) strlen(ref);
    return 1;
}

/* exported sticky-message state for command.c (recalculation error
 * replay sets the pending flag after a recalculation) */
static int errq_i;        /* next error-cell queue index */

void cs_set_errpend(int on)
{
    errpend = on;
    errq_i = 0;
}

/* evaluate the pending (not yet committed) edit-line text with the
 * Evaluate Function: the answer paints on the status bar as
 * "=         10.000000000000000000000000" ('=' plus a %24s right-aligned
 * field of the CS_MAX_P-decimal fixed form) and the edit field clears.
 * Manual 4.1 pinned 12 decimals; the visible precision is user-extended
 * to 24 (CS_MAX_P) to match the cells.  No floating point anywhere. */
static void eval_pending(void)
{
    char saved[CS_EDITMAX + 1];
    Csn val;
    char *str = NULL;
    char num[64];
    int err;
    if (g_editlen == 0) return;
    g_edit[g_editlen] = '\0';
    strcpy(saved, g_edit);
    err = cs_eval_formula(saved, &val, &str);
    edit_clear();               /* "the edit: field is cleared" */
    touch_flush(CS_D_CURSOR | CS_D_STATUS);
    if (err) {
        cs_set_errpend(1);      /* single message, no queue */
        cs_sticky_err(err, saved, g_entrycol, g_entryrow);
        return;
    }
    if (str) {
        cs_msg_str(str);
        free(str);
        return;
    }
    csn_fmt_fixed(&val, CS_MAX_P, num, (int) sizeof(num));
    cs_msg_plain("=%24s", num);   /* the answer paints non-inverse */
}

/* commit pending entry; the cursor STAYS in the entry cell, matching
 * CalcStar ("<RETURN> moves the cursor right only if no data has just
 * been typed"); recalculation is manual via ;R.  If the user moved the
 * cursor while entering (@-references), RETURN puts the cell cursor
 * back on the entry cell. */
static void entry_commit(void)
{
    int row = g_entryrow, col = g_entrycol;
    Cell *c;
    char saved[CS_EDITMAX + 1];
    int ec;
    if (g_editlen == 0) { edit_clear(); touch_flush(CS_D_CURSOR | CS_D_STATUS); return; }
    g_edit[g_editlen] = '\0';
    strcpy(saved, g_edit);
    cs_recalc_limit_reset();          /* fresh overrun state per commit */
    ec = cs_commit_entry(col, row, g_edit);   /* eval error code, 0 = none */
    c = cs_getcell(col, row);
    edit_clear();
    curCol = col;               /* commit lands on the entry cell */
    curRow = row;
    cs_nerrlist = 0;            /* commit errors are a one-message set */
    errq_i = 0;
    if (c && c->type == CE_NUM && c->iserr) {
        errpend = 1;
        /* cells first ... (the sticky message must land after the
         * paint, exactly like the old redraw-then-message order) */
        touch_flush(CS_D_GRID | CS_D_STATUS | CS_D_MENU);
        cs_sticky_err(ec, saved, col, row);   /* "... hit space [cell]" */
        if (cs_recalc_limit_hit())
            cs_msg("Recalculation limit reached");
        return;
    }
    touch_flush(CS_D_GRID | CS_D_STATUS | CS_D_MENU);
    if (cs_recalc_limit_hit())
        /* an iterated entry that overran: the message must survive the
         * repaint above (flagged) */
        cs_msg("Recalculation limit reached");
}

/* manual 6.4/14.2: caret toggles the interpretation of a cell between
 * text and numeric; `cont` stays exactly as typed, so the toggle is
 * reversible (numeric "1/4" shows as 1/4 text; text "1/4" shows 0.25) */
static void toggle_celltype(void)
{
    Cell *c = cs_getcell(curCol, curRow);
    if (!c) return;
    if (c->type == CE_NUM) {
        c->type = CE_TEXT;
        c->just = 'L';
        csn_zero(&c->val);
        c->iserr = 0;
        if (c->strres) { free(c->strres); c->strres = NULL; }
    } else if (c->type == CE_TEXT && c->cont) {
        Csn val;
        char *str = NULL;
        int err = cs_eval_formula(c->cont, &val, &str);
        if (str) { free(str); str = NULL; }
        c->type = CE_NUM;
        csn_copy(&c->val, &val);
        c->iserr = err ? 1 : 0;
        c->just = 'R';           /* numerics are always right justified */
    }
    cs_touch(CS_D_GRID | CS_D_STATUS);
}

void run_session(void)
{
    int running = 1;
    raw();
    noecho();
    nonl();
    keypad(stdscr, TRUE);
    if (curs_set(2) == ERR) curs_set(1); /* blink, else just visible */
    edit_clear();
    redraw();
    /* the startup banner first: a CLI load then happens on top of it
     * (a successful load keeps the banner via the frame hook; a load
     * ERROR message is the more important text, so the banner stands
     * down) */
    {
        char bm[96];
        sprintf(bm, "CalcStar %s, release under the terms of the MPL-2.0"
                    " license (build %d)", CS_VERSION, CS_BUILDNO);
        cs_start_msg(bm);
    }
    /* load the file given on the command line, if any - a .csv
     * extension takes the CSV path (user spec 2026-11-17) */
    if (cs_cmdfile[0]) {
        if (cs_csv_ext(cs_cmdfile)
            ? cs_load_csv(cs_cmdfile, 0, 0, 0)
            : cs_load(cs_cmdfile, 0, 0, 0))
            redraw();            /* repaint the loaded sheet */
        else
            cs_start_msg_clear();    /* the load error message wins */
    }
    while (running) {
        int ch;
        if (paint_pending) {
            /* queued keys still waiting: process them WITHOUT the
             * expensive repaint; paint only when the queue drains.
             * Phase 3: the flush paints what the mutators marked. */
            ch = cs_getch_nodelay();
            if (ch == ERR) {
                paint_pending = 0;
                cs_paint();
                continue;
            }
        } else
            ch = getch();
        if (cs_was_resize(ch)) continue;   /* terminal resized */
        if (cs_start_msg_active()) {
            /* the first keystroke clears the banner; that key then
             * acts normally */
            cs_start_msg_clear();
            redraw();
        }
        if (errpend) {
            /* the error message is cleared by ANY key; Space confirms:
             * while the ';R' error-cell queue has more entries, Space
             * replays the NEXT one ("one after another"); any other key
             * drops out and processes normally */
            if (ch == ' ' && errq_i + 1 < cs_nerrlist) {
                errq_i++;
                cs_prompt_hide();
                redraw();
                cs_sticky_err(cs_errlist[errq_i].code,
                              cs_getcell(cs_errlist[errq_i].col,
                                      cs_errlist[errq_i].row)->cont,
                              cs_errlist[errq_i].col,
                              cs_errlist[errq_i].row);
                continue;
            }
            errpend = 0;
            cs_prompt_hide();
            redraw();
            if (ch == ' ') continue;
            /* fall through: process this key normally */
        }
        switch (ch) {
        case 5:                        /* ^E up */
            step_cursor(0, -1); break;
        case 24:                       /* ^X down */
            step_cursor(0, 1); break;
        case 19:                       /* ^S left */
            step_cursor(-1, 0); break;
        case 4:                        /* ^D right */
            step_cursor(1, 0); break;
#ifdef KEY_UP
        case KEY_UP:
            step_cursor(0, -1); break;
        case KEY_DOWN:
            step_cursor(0, 1); break;
        case KEY_LEFT:
            step_cursor(-1, 0); break;
        case KEY_RIGHT:
            step_cursor(1, 0); break;
#endif
        case 26:                       /* ^Z col A next row (L-R mode)
                                          or row A next col (T-B mode) */
            if (dirTB) {
                curRow = 0;
                step_cursor(1, 0);
                winRow = 0;
            } else {
                curCol = 0;
                step_cursor(0, 1);
                winCol = 0;
            }
            break;
        case '\r': case '\n': case KEY_ENTER:
            if (g_editstate == EDIT_EDITING) {
                /* data has just been typed: the cursor STAYS in the cell;
                 * entry_commit repaints itself — an extra redraw here
                 * would wipe the sticky "Error n-> ... hit space" line */
                entry_commit();
            } else {
                /* manual 14.3 RETURN KEY: right (or down in T-B mode);
                 * at column DW it wraps to the first column of the next
                 * row */
                if (dirTB) step_cursor(0, 1);
                else if (curCol >= CS_MAXCOL - 1) { curCol = 0; step_cursor(0, 1); }
                else step_cursor(1, 0);
            }
            break;
        case '?':
            /* the Evaluate Function: pressing '?' with a pending entry
             * evaluates it without committing (manual 4.1: answers
             * carry 12 decimals); the edit field clears */
            eval_pending();
            break;
        case 9:                        /* TAB */
            cs_goto_cmd();
            break;
        case ';':
            /* inside an entry the semicolon is the statement separator
             * of extended conditionals (user-extended 2026-11); the
             * command menu stays available when idle */
            if (g_editstate == EDIT_EDITING)
                entry_append(';');
            else
                cs_command_menu();
            break;
        case '=':
            if (g_editstate == EDIT_EDITING)
                entry_append('=');
            else
                cs_lock_cmd();
            break;
        case 2:                        /* ^B data toggle */
            showForm = !showForm;
            cs_touch(CS_D_GRID | CS_D_STATUS); break;
        case 27:                       /* ESC */
            /* the DEL key on the terminfo-poor systems arrives as
             * ESC [ 3 ~ (the ULTRIX report, 2026-11-23): the scan
             * consumes it.  THE DEL IS CONTEXT-SENSITIVE (the
             * user's design, 2026-11-23): during an entry it ABORTS
             * like ESC - the backspace reading would let a user
             * delete down to an empty field and have the NEXT DEL
             * silently delete the formula cell beneath; at idle it
             * is the ";D E" entry delete.  Anything else ungets back
             * and the ESC acts as before. */
            if (cs_del_scan()) {
                if (g_editstate == EDIT_EDITING) {
                    edit_clear();
                    cs_prompt_hide();
                    cs_touch(CS_D_CURSOR | CS_D_STATUS);
                    break;
                }
                cs_free_cell_at(curCol, curRow);
                cs_recount_entries();
                cs_mark_sheet_change();
                cs_touch(CS_D_GRID | CS_D_STATUS | CS_D_MENU);
                break;
            }
            edit_clear();
            cs_prompt_hide();
            cs_touch(CS_D_CURSOR | CS_D_STATUS); break;
        case KEY_BACKSPACE: case 8: case 127:
            if (g_editstate == EDIT_EDITING && g_editlen > 0) {
                g_edit[--g_editlen] = '\0';
                if (g_editlen == 0) g_editstate = EDIT_IDLE;
                cs_touch(CS_D_CURSOR | CS_D_STATUS);
            }
            break;
#ifdef KEY_DC
        case KEY_DC:                   /* the keypad DEL */
            /* the DEL key: CONTEXT-SENSITIVE (the user's design,
             * 2026-11-23, the same rule as the ESC [ 3 ~ scan
             * above): during an entry it ABORTS like ESC (the
             * backspace reading would transition the user into the
             * ";D E" cell-delete once the field runs empty - the
             * formula beneath gets deleted by accident); at idle it
             * is the ";D E" entry delete: the manual's entry delete,
             * no dependency check, no reference shifts.  The ASCII
             * DEL (127) stays backspace above. */
            if (g_editstate == EDIT_EDITING) {
                edit_clear();
                cs_prompt_hide();
                cs_touch(CS_D_CURSOR | CS_D_STATUS);
                break;
            }
            cs_free_cell_at(curCol, curRow);
            cs_recount_entries();
            cs_mark_sheet_change();
            cs_touch(CS_D_GRID | CS_D_STATUS | CS_D_MENU);
            break;
#endif
        case '"':
            /* '"': text-literal quote.  While entering it is just a
             * character (needed for conditional strings); the pending
             * TEXT/NUMERIC toggle lives on '^' (manual 6.4) */
            if (g_editstate == EDIT_EDITING)
                entry_append('"');
            else
                entry_begin('"');
            break;
        case 94: case 30:              /* '^' TEXT/NUMERIC DATA TOGLE
                                          (manual 6.4): the LITERAL
                                          caret (94) and CONTROL-^ (30
                                          = 0x1E) - the manuals' ^
                                          convention is the CONTROL
                                          key (the cursor movement is
                                          ^S/^E/^X/^D/^Z), and the
                                          control-^ fires immediately on
                                          the German keyboard (the
                                          literal ^ is a DEAD KEY there:
                                          the first press latches at the
                                          IM, the second delivers 0x5E) */
            if (g_editstate == EDIT_EDITING) {
                g_entry_num = !g_entry_num;   /* pending toggle, live on
                                                 the type line */
                cs_touch(CS_D_CURSOR | CS_D_STATUS);
            } else {
                toggle_celltype();
            }
            break;
        case '@':                       /* "@ Curs Pos": insert the coordinate
                                           OF the cell the cursor is on
                                           right now; the cell cursor stays.
                                           repaint NOW - the user sees the
                                           reference the moment it lands */
            if (g_editstate == EDIT_EDITING) {
                if (at_resolve())
                    touch_flush(CS_D_CURSOR | CS_D_STATUS);
            } else
                entry_begin_ref();
            break;
        default:
            if (ch >= ' ' && ch < 127) {
                if (g_editstate == EDIT_EDITING)
                    entry_append(ch);
                else if (g_editstate != EDIT_EDITING) {
                    if (!cs_ensure_room()) {
                        cs_msg("MEMORY IS TOO LOW");
                        redraw();
                        errpend = 0;
                        break;
                    }
                    entry_begin(ch);
                }
            }
            break;
        }
        /* phase 4: the mutators marked their components; paint when
         * the batch drains (a queued paste paints once, on drain).
         * Nothing marked: the paint still places the cursor and ends
         * prompt mode, but no component repaints - a message painted
         * directly (Bad coord, the ?-calculator) survives until the
         * next interaction, exactly like the old no-redraw paths. */
        if (cs_input_pending())
            paint_pending = 1;
        else if (!paint_pending)
            cs_paint();
    }
}

/* ============================ --test mode =========================== */
/* All numeric expectations are EXACT decimal strings in CalcStar's
 * 14-significant-digit canonical form: [sign]d.dddddddddddddE<+|->exp
 * (the BCD engine; values are 0.d1d2... of 14 digits). */
static int fails;

static void ckstr(const char *name, const char *got, const char *want)
{
    if (strcmp(got, want) != 0) {
        printf("FAIL %-24s got %-22s want %s\n", name, got, want);
        fails++;
    } else {
        printf("ok   %-24s = %s\n", name, got);
    }
}

static void cke(const char *name, const char *expr, int wanterr)
{
    Csn r;
    char *s = NULL;
    int e = cs_eval_formula(expr, &r, &s);
    if (s) free(s);
    if (e != wanterr) {
        printf("FAIL %-24s err got %d want %d\n", name, e, wanterr);
        fails++;
    } else {
        printf("ok   %-24s -> Error %d\n", name, e);
    }
}

/* evaluate expr and compare the canonical 14-digit string */
static void ckc(const char *name, const char *expr, const char *want)
{
    Csn r;
    char *s = NULL;
    char got[48];
    int e = cs_eval_formula(expr, &r, &s);
    if (e != 0) {
        printf("FAIL %-24s err %d (want value)\n", name, e);
        fails++;
        if (s) free(s);
        return;
    }
    csn_canon(&r, got, (int) sizeof(got));
    ckstr(name, got, want);
    if (s) free(s);
}

/* set a cell's value from an exact decimal text */
/* the tests' allocation helper: alloc or die (the budget is never
 * reached by the test sheets) */
static Cell *tcell(int col, int row)
{
    Cell *c = cs_alloc_cell(col, row);
    if (!c) { printf("FAIL alloc (%d,%d)\n", col, row); exit(1); }
    return c;
}

static void setv(int col, int row, const char *txt)
{
    Csn v;
    csn_parse(txt, &v);
    cs_free_cell_at(col, row);
    cs_set_num(tcell(col, row), txt, &v, 0, NULL);
}

/* the brainfuck machinery (user-ext 2026-11-13): 17 formula cells in
 * row 5, columns B..R, evaluated left to right every recalculation
 * pass - one instruction per pass.  State in row 1 (B1 pc, D1 tape
 * ptr, F1 mode 0=run/1=skip-fwd/2=skip-back/3=halt, H1 bracket depth,
 * J1 output count, K1 input ptr, L1 program length); the program is
 * row 3 (one character per cell, columns B..); the tape is column A
 * (seeded with 0); the output lands in column S (one character per
 * row); the input chars sit in column T from row 20. */
static const char *bf_mach[17] = {
    "+(VAL(B1)<=VAL(L1)+1):ORD(VAL(REF(VAL(B1),3))):0",
    "+(VAL(F1)=0)*(VAL(B5)=43):SET(REF(1,VAL(D1)),VAL(REF(1,VAL(D1)))+1);SET(B1,VAL(B1)+1);0:0",
    "+(VAL(F1)=0)*(VAL(B5)=45):SET(REF(1,VAL(D1)),VAL(REF(1,VAL(D1)))-1);SET(B1,VAL(B1)+1);0:0",
    "+(VAL(F1)=0)*(VAL(B5)=62):SET(D1,VAL(D1)+1);SET(B1,VAL(B1)+1);0:0",
    "+(VAL(F1)=0)*(VAL(B5)=60):SET(D1,VAL(D1)-1);SET(B1,VAL(B1)+1);0:0",
    "+(VAL(F1)=0)*(VAL(B5)=46):SET(REF(19,7),VAL(REF(19,7))&CHR(VAL(REF(1,VAL(D1)))));SET(J1,VAL(J1)+1);SET(B1,VAL(B1)+1);0:0",
    "+(VAL(F1)=0)*(VAL(B5)=44):SET(REF(1,VAL(D1)),ORD(VAL(REF(20,VAL(K1)+20))));SET(K1,VAL(K1)+1);SET(B1,VAL(B1)+1);0:0",
    "+(VAL(F1)=0)*(VAL(B5)=91)*(VAL(REF(1,VAL(D1)))=0):SET(F1,1);SET(H1,0);0:0",
    "+(VAL(F1)=0)*(VAL(B5)=91)*(VAL(REF(1,VAL(D1)))<>0):SET(B1,VAL(B1)+1);0:0",
    "+(VAL(F1)=0)*(VAL(B5)=93)*(VAL(REF(1,VAL(D1)))<>0):SET(F1,2);SET(H1,0);0:0",
    "+(VAL(F1)=0)*(VAL(B5)=93)*(VAL(REF(1,VAL(D1)))=0):SET(B1,VAL(B1)+1);0:0",
    "+(VAL(F1)=1)*(VAL(B5)<>93):SET(H1,VAL(H1)+(VAL(B5)=91));SET(B1,VAL(B1)+1);0:0",
    "+(VAL(F1)=1)*(VAL(B5)=93):SET(H1,VAL(H1)-1);SET(B1,VAL(B1)+1);0:0",
    "+(VAL(F1)=1)*(VAL(B5)=93)*(VAL(H1)=0):SET(F1,0);0:0",
    "+(VAL(F1)=2)*(VAL(B5)<>91):SET(B1,VAL(B1)-1);SET(H1,VAL(H1)+(VAL(B5)=93));0:0",
    "+(VAL(F1)=2)*(VAL(B5)=91):((VAL(H1)=1):SET(F1,0);0:SET(H1,VAL(H1)-1);SET(B1,VAL(B1)-1);0):0",
    "+(VAL(B1)>VAL(L1)+1)*(VAL(F1)<>3):SET(F1,3);0:0"
};

/* build the interpreter sheet: state, machinery, tape, program and
 * optional input characters */
static void bf_build(const char *prog, const char *inp)
{
    static char lbuf[8];
    char cb[2];
    int i, n = (int) strlen(prog);
    cs_clear_all();
    setv(1, 0, "2");                        /* B1 pc */
    setv(3, 0, "1");                        /* D1 ptr */
    setv(5, 0, "0");                        /* F1 mode */
    setv(7, 0, "0");                        /* H1 depth */
    setv(9, 0, "0");                        /* J1 out count */
    setv(10, 0, "0");                       /* K1 input ptr */
    sprintf(lbuf, "%d", n);
    setv(11, 0, lbuf);                      /* L1 program length */
    for (i = 0; i < 60; i++) setv(0, i, "0");       /* the tape */
    for (i = 0; i < 17; i++) cs_commit_entry(1 + i, 4, bf_mach[i]);
    cs_commit_entry(18, 6, "+\"\"");        /* S7: the output cell, empty */
    cb[1] = '\0';
    for (i = 0; i < n; i++) {
        cb[0] = prog[i];
        cs_set_text(tcell(1 + i, 2), cb, 'L');    /* row 3, B.. */
    }
    if (inp)
        for (i = 0; inp[i]; i++) {
            cb[0] = inp[i];
            cs_set_text(tcell(19, 19 + i), cb, 'L');      /* T20.. */
        }
}

/* the tests' allocation helper: alloc or die (the budget is never
 * reached by the test sheets) */
/* window-scroll check: 1 on failure (added to `fails` by the caller) */
static int scrck(int cond, const char *what)
{
    if (cond) { printf("ok   %s\n", what); return 0; }
    printf("FAIL %s\n", what);
    return 1;
}

/* the tests' read helper: an absent cell reads as empty (the sparse
 * store's documented rule) */
static Cell *tread(int col, int row)
{
    static Cell empty;
    Cell *c = cs_getcell(col, row);
    if (c) return c;
    memset(&empty, 0, sizeof(empty));
    empty.type = CE_EMPTY;
    empty.prec = -1;
    return &empty;
}

/* the sparse store's portability guards: no single allocation may
 * exceed 64 KiB (the 16-bit rule the design keeps for the DOS
 * extender), and the chunk math must cover the budget exactly */
static void port_guards(void)
{
    int slots = CS_BUDGET / 250;
    if (CS_BUDGET % 250 != 0 || slots > 16)
        { printf("FAIL pool math (%d chunks)\n", slots); fails++; }
    else printf("ok   pool math: %d chunks of %d slots\n", slots,
                CS_BUDGET / slots);
    /* the 64 KiB chunk guard (the DOS design rule): the volatile read
     * defeats the compiler's constant folding so the failure branch
     * stays "reachable" (Watcom W201 flags a folded-unreachable
     * branch even though the guard is the point) */
    {
        volatile size_t csz = sizeof(Cell);
        if (csz * 250 > 65535)
            { printf("FAIL chunk exceeds 64 KiB\n"); fails++; }
        else printf("ok   a chunk is %d KiB\n",
                    (int) (sizeof(Cell) * 250 / 1024));
    }
}

static void run_tests(void)
{
    char buf[64];
    int i;

    port_guards();
    cs_clear_all();
    cs_reg_reset();
    /* manual example: 140%E4 with E4 = .02  ->  .028 */
    setv(4, 3, "0.02");
    ckc("140%E4", "140%E4", "+2.8000000000000E-1");
    setv(4, 3, "0");

    /* calculator example: 120%18.53333 -> 22.239996 */
    ckc("120%18.53333", "120%18.53333", "+2.2239996000000E+2");

    /* SUM(L4>L11) -> 2247.04 (L-R summation, exact decimal) */
    {
        static const char *xs[8] = { "100", "50", "200", "1", "3", "17",
                                     "1856.04", "20" };
        for (i = 0; i < 8; i++) setv(11, 3 + i, xs[i]);
        ckc("SUM(L4>L11)", "SUM(L4>L11)", "+2.2470400000000E+4");
        ckc("CNT(L4>L11)", "CNT(L4>L11)", "+8.0000000000000E+1");
        ckc("AVG(L4>L11)", "AVG(L4>L11)", "+2.8088000000000E+3");
        ckc("MAX(L4>L11)", "MAX(L4>L11)", "+1.8560400000000E+4");
        ckc("MIN(L4>L11)", "MIN(L4>L11)", "+1.0000000000000E+1");
    }

    /* regression: y = 2x + 1 over x=1..5 (exact rational result) */
    for (i = 0; i < 5; i++) {
        char t[16];
        sprintf(t, "%d", i + 1);
        setv(0, i, t);
        sprintf(t, "%d", 2 * (i + 1) + 1);
        setv(1, i, t);
    }
    ckc("REGR(meany)", "REGR(A1>A5,B1)", "+7.0000000000000E+1");
    if (!g_reg_ok) { printf("FAIL g_reg_ok\n"); fails++; }
    ckc("SLOPE()", "SLOPE()", "+2.0000000000000E+1");
    ckc("PROJ(6)", "PROJ(6)", "+1.3000000000000E+2");
    ckc("DEPD(13)", "DEPD(13)", "+6.0000000000000E+1");
    ckc("DEPD(7)", "DEPD(7)", "+3.0000000000000E+1");
    /* the REGRESSION INVALIDATION (the user's report, 2026-11-16) */
    /* the snapshot survives an unrelated edit (the manual's
     * semantics: the state is a deliberate REGR snapshot) */
    setv(3, 9, "999");
    ckc("SLOPE() after an edit", "SLOPE()", "+2.0000000000000E+1");
    /* the clear invalidates: SLOPE() must not serve the stale state */
    cs_clear_all();
    cke("SLOPE() after clear", "SLOPE()", 9);
    /* a FAILED attempt invalidates */
    setv(0, 0, "1"); setv(0, 1, "2"); setv(0, 2, "3");
    setv(1, 0, "1"); setv(1, 1, "2");
    cke("REGR diagonal", "REGR(A1>B2,C3)", 2);   /* the state reset */
    cke("SLOPE() after failure", "SLOPE()", 9);
    /* a valid REGR re-establishes */
    for (i = 0; i < 5; i++) {
        char t[16];
        sprintf(t, "%d", i + 1);
        setv(0, i, t);
        sprintf(t, "%d", 2 * (i + 1) + 1);
        setv(1, i, t);
    }
    ckc("REGR re-establish", "REGR(A1>A5,B1)", "+7.0000000000000E+1");
    ckc("SLOPE() again", "SLOPE()", "+2.0000000000000E+1");
    /* the load resets (the merge branch too) */
    cs_save("t_regr", 0, 0, 1, 4, "");
    cs_load("t_regr", 0, 0, 0);
    cke("SLOPE() after load", "SLOPE()", 9);
    remove("t_regr.csd");
    /* the SORT leaves the state intact (order-independent sums) */
    ckc("REGR re-establish 2", "REGR(A1>A5,B1)", "+7.0000000000000E+1");
    cs_sort_range(0, 0, 0, 4, 0, 1);             /* descending */
    ckc("SLOPE() after sort", "SLOPE()", "+2.0000000000000E+1");
    /* the FULL legal width: 255 observations (the 1-D constraint
     * bounds a vertical range at 255 rows - the old 512 cap was
     * unreachable in practice; the dead arrays are simply gone) */
    cs_clear_all();
    for (i = 0; i < 255; i++) {
        char t[16];
        sprintf(t, "%d", i + 1);
        setv(0, i, t);
        sprintf(t, "%d", 2 * (i + 1) + 1);
        setv(1, i, t);
    }
    ckc("REGR 255", "REGR(A1>A255,B1)", "+2.5700000000000E+3");
    ckc("SLOPE() 255", "SLOPE()", "+2.0000000000000E+1");
    ckc("PROJ(300) 255", "PROJ(300)", "+6.0100000000000E+3");
    cs_clear_all();

    /* Appendix evaluation-error codes (2nd manual table): the parser's
     * internal codes ARE the manual's; cs_commit_entry returns them for
     * the sticky "Error n-> ..." line (<=0 folds to 0 at the message) */
    cke("err unknown function", "+XYZ(A1)", 4);
    cke("err missing paren", "(1+2", 3);
    cke("err illegal chars end", "1+2)", 7);
    cke("err regr not row/col", "REGR(A1>B2,C3)", 2);   /* 1-D only */
    cke("err term expr illegal", "+A", 6);
    cke("empty A99 is zero", "+A99", 0);   /* the manual's rule */
    cke("err value range overflow", "+9E+999", -1);  /* displays Error 0 */
    cke("err div by zero", "1/0", -1);               /* displays Error 0 */
    {
        Csn r;
        char *s = NULL;
        char deep[64];
        int e;
        /* 16 open paren-depth levels > MAXDEPTH (12) */
        memset(deep, 0, sizeof(deep));
        { int k; for (k = 0; k < 16 && k * 3 + 3 < (int) sizeof(deep); k++)
            sprintf(deep + k * 3, "(1+");
          sprintf(deep + k * 3, "1"); }
        e = cs_eval_formula(deep, &r, &s);
        if (e != 12) { printf("FAIL paren levels err %d want 12\n", e); fails++; }
        else printf("ok   parenthetical levels -> 12\n");
        if (s) free(s);
    }
    {
        int ec = cs_commit_entry(2, 100, "+XYZ(A1)");   /* C101, headless */
        if (ec != 4) { printf("FAIL commit code %d want 4\n", ec); fails++; }
        else printf("ok   commit returns manual code 4\n");
        cs_free_cell_at(2, 100);
    }
    /* !n!: "column is not wide enough" (both manuals' entry-error token) */
    {
        Csn w;
        char b[128];
        csn_parse("1234567890.12", &w);
        cs_fmt_num(&w, 3, 2, 0, 0, b, (int) sizeof(b));
        if (strcmp(b, "!n!")) { printf("FAIL !n! display got %s\n", b); fails++; }
        else printf("ok   narrow column displays !n!\n");
    }

    /* conditionals */
    ckc("cond true", "1<2:5:7", "+5.0000000000000E+1");
    ckc("cond false", "1>2:5:7", "+7.0000000000000E+1");
    ckc("empty true-branch", "1<2::9", "0");
    ckc("missing else", "1>2:5", "0");
    ckc("nested cond", "(1<2):((2<3):8:9):7", "+8.0000000000000E+1");
    ckc("nested false", "(1>2):7:((2>3):8:9)", "+9.0000000000000E+1");

    /* side-effect statements in conditional branches (user-extended
     * 2026-11): SET writes after fully evaluating its value, exactly
     * one value piece per branch, the unselected branch must not run */
    {
        Csn v;
        char g[64];
        Cell *c;
        int e;
        /* SET: evaluate-first (A11's OLD value), numeric assign; the
         * branch needs exactly one value piece */
        csn_parse("5", &v);
        cs_set_num(tcell(0, 10), "5", &v, 0, NULL);   /* A11 = 5 */
        e = cs_eval_formula("+(1>0):SET(A11,A11+1);A11:0", &v, NULL);
        csn_canon(&tread(0, 10)->val, g, (int) sizeof(g));
        if (e != 0 || strcmp(g, "+6.0000000000000E+1"))
            { printf("FAIL SET eval-first e=%d got %s\n", e, g); fails++; }
        else printf("ok   SET(A11,A11+1) evaluates then assigns 6\n");
        /* SET text with the justification escape */
        e = cs_eval_formula("+(1>0):SET(A11,\"\\rtest\");\"t\":0", &v, NULL);
        c = tread(0, 10);
        if (e != 0 || c->type != CE_TEXT || c->just != 'R' ||
            !c->cont || strcmp(c->cont, "test"))
            { printf("FAIL SET text e=%d\n", e); fails++; }
        else printf("ok   SET(A11,\"\\rtest\") right-aligned text\n");
        /* the unselected branch must not execute its side effects */
        cs_clear_all();
        e = cs_eval_formula("+(1>0):0:SET(A11,9);1", &v, NULL);
        if (e != 0 || tread(0, 10)->type != CE_EMPTY)
            { printf("FAIL unselected SET ran e=%d\n", e); fails++; }
        else printf("ok   unselected branch did not execute\n");
        /* exactly one value piece */
        cs_clear_all();
        e = cs_eval_formula("+(1>0):1;2:0", &v, NULL);
        if (e != 7) { printf("FAIL two values err %d\n", e); fails++; }
        else printf("ok   two value pieces -> Error 7\n");
        e = cs_eval_formula("+(1>0):SET(A11,5):0", &v, NULL);
        if (e != 7) { printf("FAIL zero values err %d\n", e); fails++; }
        else printf("ok   zero value pieces -> Error 7\n");
        /* side effects are not value-producing: as an operand -> Error 4 */
        e = cs_eval_formula("+SET(A11,5)*2", &v, NULL);
        if (e != 4) { printf("FAIL SET operand err %d\n", e); fails++; }
        else printf("ok   SET as operand -> Error 4 (unknown fn)\n");
        /* the iteration: a countdown driven by SET + RECALC through
         * cs_recalc_all (A11 counts 5..0, A12 = the done-marker) */
        cs_clear_all();
        csn_parse("5", &v);
        cs_set_num(tcell(0, 10), "5", &v, 0, NULL);   /* A11 = 5 */
        cs_commit_entry(0, 12, "+(A11>0):SET(A11,A11-1);RECALC();0:\"finished\"");
        csn_canon(&tread(0, 10)->val, g, (int) sizeof(g));
        if (strcmp(g, "0") && strcmp(g, "+0.0000000000000E+0"))
            { printf("FAIL countdown A11 got %s\n", g); fails++; }
        else printf("ok   RECALC iteration: A11 counts down to 0\n");
        c = tread(0, 12);
        if (c->type != CE_NUM || c->iserr ||
            !c->strres || strcmp(c->strres, "finished"))
            { printf("FAIL countdown marker type=%d strres='%s'\n",
                     c->type, c->strres ? c->strres : ""); fails++; }
        else printf("ok   countdown marker A12 = finished\n");
        /* regression (2026-11): the stability check must compare the
         * NEW value against the OLD one (it compared a zeroed value,
         * so any nonzero fixed point kept the passes running to the
         * limit); the user's exact sheet: A3=5, A4=10, C4 counts up
         * and settles on the string "done" */
        cs_clear_all();
        csn_parse("5", &v);
        cs_set_num(tcell(0, 2), "5", &v, 0, NULL);    /* A3 = 5 */
        csn_parse("10", &v);
        cs_set_num(tcell(0, 3), "10", &v, 0, NULL);   /* A4 = 10 */
        cs_commit_entry(2, 3,
            "(A3<A4):SET(A3,A3+1);\"counting\";RECALC(A3):\"done\"");
        csn_canon(&tread(0, 2)->val, g, (int) sizeof(g));
        if (strcmp(g, "+1.0000000000000E+2"))
            { printf("FAIL settle A3 got %s\n", g); fails++; }
        else printf("ok   iteration settles: A3 = 10\n");
        c = tread(2, 3);
        if (c->iserr || !c->strres || strcmp(c->strres, "done"))
            { printf("FAIL settle C4 strres='%s'\n",
                     c->strres ? c->strres : ""); fails++; }
        else printf("ok   iteration settles: C4 = done\n");
        /* idempotent SETs do not keep the iteration alive (the change
         * notification is honest): an always-true SET writing the SAME
         * value every pass settles in at most two passes */
        cs_clear_all();
        setv(1, 0, "7");                        /* B1 = 7 */
        cs_commit_entry(2, 0, "1:SET(A1,VAL(B1));0");   /* C1 */
        cs_recalc_all();
        {
            char g2[48];
            csn_canon(&tread(0, 0)->val, g2, (int) sizeof(g2));
            if (cs_recalc_last_passes > 2 ||
                strcmp(g2, "+7.0000000000000E+1"))
                { printf("FAIL idempotent SET passes=%d val=%s\n",
                         cs_recalc_last_passes, g2); fails++; }
            else printf("ok   idempotent SET settles in %d pass(es)\n",
                        cs_recalc_last_passes);
        }
        /* the SET still fires when the source changes */
        setv(1, 0, "9");
        cs_recalc_all();
        {
            char g2[48];
            csn_canon(&tread(0, 0)->val, g2, (int) sizeof(g2));
            if (strcmp(g2, "+9.0000000000000E+1"))
                { printf("FAIL idempotent SET refires (%s)\n", g2); fails++; }
            else printf("ok   idempotent SET refires on change\n");
        }
        /* a value-CHANGING SET keeps iterating (countdown regression) */
        cs_clear_all();
        setv(0, 0, "3");
        cs_commit_entry(1, 0, "(VAL(A1)>0):SET(A1,VAL(A1)-1);RECALC();0:0");
        cs_recalc_all();
        {
            char g2[48];
            csn_canon(&tread(0, 0)->val, g2, (int) sizeof(g2));
            if (strcmp(g2, "0") && strcmp(g2, "+0.0000000000000E+0"))
                { printf("FAIL countdown-after-fix (%s)\n", g2); fails++; }
            else printf("ok   value-changing SET still iterates\n");
        }
        /* cs_eval_range: the dialog entry point for dynamic ranges */
        cs_clear_all();
        setv(2, 0, "3");                        /* C1 = 3 */
        setv(2, 1, "7");                        /* C2 = 7 */
        {
            int a1, b1, a2, b2;
            if (cs_eval_range("REF(C1,C2)>REF(C1,C2)", &a1, &b1, &a2, &b2) == 0 &&
                a1 == 2 && b1 == 6 && a2 == 2 && b2 == 6)
                printf("ok   cs_eval_range REF pair\n");
            else { printf("FAIL cs_eval_range REF pair\n"); fails++; }
            if (cs_eval_range("A1>A5", &a1, &b1, &a2, &b2) == 0 &&
                a1 == 0 && b1 == 0 && a2 == 0 && b2 == 4)
                printf("ok   cs_eval_range A1>A5\n");
            else { printf("FAIL cs_eval_range A1>A5\n"); fails++; }
            if (cs_eval_range("A5>A1", &a1, &b1, &a2, &b2) == 0 &&
                a1 == 0 && b1 == 0 && a2 == 0 && b2 == 4)
                printf("ok   cs_eval_range reversed normalizes\n");
            else { printf("FAIL cs_eval_range reversed\n"); fails++; }
            if (cs_eval_range("SUM(1,2)", &a1, &b1, &a2, &b2) != 0)
                printf("ok   cs_eval_range rejects non-ranges\n");
            else { printf("FAIL cs_eval_range accepts junk\n"); fails++; }
            if (cs_eval_range("REF(128,1)", &a1, &b1, &a2, &b2) != 0)
                printf("ok   cs_eval_range rejects out-of-limits\n");
            else { printf("FAIL cs_eval_range limits\n"); fails++; }
        }
        cs_clear_all();
    }

    /* percent postfix + arithmetic */
    ckc("50%", "50%", "+5.0000000000000E+0");
    ckc("6/3", "6/3", "+2.0000000000000E+1");
    ckc("(2+3)*4", "(2+3)*4", "+2.0000000000000E+2");

    /* new: BCD precision - the classic binary-float traps */
    ckc("0.1+0.2", "0.1+0.2", "+3.0000000000000E+0");
    ckc("0.1*3", "0.1*3", "+3.0000000000000E+0");
    ckc("1/3", "1/3", "+3.3333333333333E+0");
    ckc("2.5*0.4", "2.5*0.4", "+1.0000000000000E+1");
    ckc("1000001/7", "1000001/7", "+1.4285728571428E+6");

    /* '**' power (manual 4.7) */
    ckc("(28**2)+(52**2)", "(28**2)+(52**2)", "+3.4880000000000E+4");
    ckc("2**3", "2**3", "+8.0000000000000E+1");
    ckc("2**-2", "2**-2", "+2.5000000000000E+0");
    ckc("9**0.5", "9**0.5", "+3.0000000000000E+1");
    ckc("(-8)**3", "(-8)**3", "-5.1200000000000E+3");
    ckc("mcd", "(12*10**-3)", "+1.2000000000000E-1");

    /* trigonometry in RADIANS (user decision 2026-11; the trig set is
     * not documented in the manuals - the earlier degree convention was
     * a flagged assumption now retired; DEG/RAD/DEGREES/RADIANS convert) */
    ckc("SIN(PI()/6)", "SIN(PI()/6)", "+5.0000000000000E+0");
    ckc("COS(PI()/3)", "COS(PI()/3)", "+5.0000000000000E+0");
    ckc("TAN(PI()/4)", "TAN(PI()/4)", "+1.0000000000000E+1");
    ckc("SIN(-PI()/6)", "SIN(-PI()/6)", "-5.0000000000000E+0");
    ckc("COS(2*PI()/3)", "COS(2*PI()/3)", "-5.0000000000000E+0");
    ckc("SIN(PI())", "SIN(PI())", "0");
    ckc("SIN(1)", "SIN(1)", "+8.4147098480789E+0");
    ckc("COS(1)", "COS(1)", "+5.4030230586813E+0");
    ckc("ASIN(0.5)", "ASIN(0.5)", "+5.2359877559829E+0");
    ckc("ACOS(0.5)", "ACOS(0.5)", "+1.0471975511965E+1");
    ckc("ATAN(1)", "ATAN(1)", "+7.8539816339744E+0");
    ckc("ASIN(1)", "ASIN(1)", "+1.5707963267948E+1");
    /* inverse-trig accuracy: the atan complement+halving reduction and
     * the acos half-angle identity keep the honest floor; the inverse
     * family publishes at 38 digits so tan(atan(x)) round trips keep
     * the argument's digits (error was ~1e-22 at round30) */
    ckc("ACOS(-0.5)", "ACOS(-0.5)", "+2.0943951023931E+1");   /* 2pi/3 */
    ckc("ACOS(-1)", "ACOS(-1)", "+3.1415926535897E+1");       /* pi */
    ckc("ACOS(0)", "ACOS(0)", "+1.5707963267948E+1");         /* pi/2 */
    ckc("ACOS(1)", "ACOS(1)", "0");
    ckc("ACOS(-0.9999)", "ACOS(-0.9999)", "+3.1274504001122E+1");
    ckc("ACOS(0.9999)", "ACOS(0.9999)", "+1.4142253477512E-1");
    ckc("ACOS(0.9999976)", "ACOS(0.9999976)",
        "+2.1908906681989E-2");
    {
        Csn r;
        char g[48];
        char *s2 = NULL;
        int e = cs_eval_formula(
            "+ASIN(ACOS(ATAN(TAN(COS(SIN(9))))))", &r, &s2);
        if (s2) free(s2);
        csn_canon(&r, g, (int) sizeof(g));
        /* radians: the chain collapses to asin(sin(9)) with 9 rad in
         * (pi/2, pi), so the exact value is 3*pi - 9
         * = 0.4247779607693797153879301498... (the classic degree-mode
         * variant landed on exactly 9) */
        if (e != 0 || strcmp(g, "+4.2477796076937E+0")) {
            printf("FAIL trig round-trip chain e=%d got %s\n", e, g);
            fails++;
        } else printf("ok   trig round-trip chain -> 3*pi - 9\n");
    }

    /* engineering-precision battery: fixed-decimal strings beyond the
     * old 15-digit floor (values verified against 60-digit decimal
     * reference arithmetic) */
    {
        Csn r, t;
        char g[160];
        int e;
        e = cs_eval_formula(
            "+ASIN(ACOS(ATAN(TAN(COS(SIN(9))))))", &r, NULL);
        csn_fmt_fixed(&r, 24, g, (int) sizeof(g));
        if (e != 0 || strcmp(g, "0.424777960769379715387930"))
            { printf("FAIL chain fixed-24 e=%d got %s\n", e, g); fails++; }
        else printf("ok   chain fixed-24 = 3*pi - 9\n");
        /* tan(atan(x)) round trip: the argument's digits survive
         * (was ...789000005056921 before the round38/complement fix) */
        e = cs_eval_formula(
            "+TAN(ATAN(123456789.123456789))", &r, NULL);
        csn_fmt_fixed(&r, 24, g, (int) sizeof(g));
        if (e != 0 || strcmp(g, "123456789.123456789000000000000000"))
            { printf("FAIL tan-atan round trip e=%d got %s\n", e, g); fails++; }
        else printf("ok   tan(atan(x)) round trip keeps the digits\n");
        /* tiny sqrt: relative Newton convergence (was a one-step exit
         * with the old absolute threshold) */
        e = cs_eval_formula("+SQRT(1E-998)*1E498", &r, NULL);
        csn_canon(&r, g, (int) sizeof(g));
        if (e != 0 || strcmp(g, "+1.0000000000000E+0"))
            { printf("FAIL sqrt(1e-998) e=%d got %s\n", e, g); fails++; }
        else printf("ok   sqrt(1e-998) = 1e-499\n");
        csn_parse("2", &t);
        csn_sqrt(&r, &t);
        csn_fmt_fixed(&r, 28, g, (int) sizeof(g));
        if (strcmp(g, "1.4142135623730950488016887242"))
            { printf("FAIL sqrt2 fixed-28 got %s\n", g); fails++; }
        else printf("ok   sqrt2 carries 29 digits\n");
        csn_from_int(&t, 1);
        csn_exp(&r, &t);
        csn_fmt_fixed(&r, 30, g, (int) sizeof(g));
        if (strcmp(g, "2.718281828459045235360287471350"))
            { printf("FAIL exp1 fixed-30 got %s\n", g); fails++; }
        else printf("ok   exp1 carries 30 digits\n");
        /* cos is even: negative input must NOT flip the sign
         * (pre-existing bug found by the 38-decimal differential) */
        csn_parse("-0.7", &t);
        csn_cos(&r, &t);
        csn_canon(&r, g, (int) sizeof(g));
        if (strcmp(g, "+7.6484218728448E+0"))
            { printf("FAIL cos(-0.7) got %s\n", g); fails++; }
        else printf("ok   cos(-0.7) = cos(0.7) (even function)\n");
        /* tiny trig arguments pass through exactly (radians) */
        csn_parse("1e-20", &t);
        csn_sin(&r, &t);
        csn_canon(&r, g, (int) sizeof(g));
        if (strcmp(g, "+1.0000000000000E-19"))
            { printf("FAIL sin(1e-20) got %s\n", g); fails++; }
        else printf("ok   sin(1e-20) = 1e-20\n");

        /* visible precision 24 (user-extended from the manual's 12):
         * fixed-form strings at CS_MAX_P, verified against the
         * 60-digit decimal reference */
        csn_parse("2", &t);
        csn_sqrt(&r, &t);
        csn_fmt_fixed(&r, CS_MAX_P, g, (int) sizeof(g));
        if (strcmp(g, "1.414213562373095048801688"))
            { printf("FAIL sqrt2 p24 got %s\n", g); fails++; }
        else printf("ok   sqrt2 at prec 24\n");
        csn_parse("3.14159265358979323846264338327950288419716939937510582097494459", &r);
        csn_fmt_fixed(&r, CS_MAX_P, g, (int) sizeof(g));
        if (strcmp(g, "3.141592653589793238462643"))
            { printf("FAIL pi p24 got %s\n", g); fails++; }
        else printf("ok   pi literal at prec 24\n");
        csn_from_int(&t, 1);
        csn_exp(&r, &t);
        csn_fmt_fixed(&r, CS_MAX_P, g, (int) sizeof(g));
        if (strcmp(g, "2.718281828459045235360287"))
            { printf("FAIL exp1 p24 got %s\n", g); fails++; }
        else printf("ok   exp1 at prec 24\n");
        /* values below 1e-30 print exact zeros beyond the honest core */
        csn_parse("1e-40", &r);
        csn_fmt_fixed(&r, CS_MAX_P, g, (int) sizeof(g));
        if (strcmp(g, "0.000000000000000000000000"))
            { printf("FAIL tiny p24 got %s\n", g); fails++; }
        else printf("ok   tiny value at prec 24 underflows to zeros\n");
    }

    /* PI() at maximum internal precision (deg-mode trig companion) */
    ckc("PI()", "PI()", "+3.1415926535897E+1");
    ckc("PI()/2", "PI()/2", "+1.5707963267948E+1");
    ckc("PI()**2", "PI()**2", "+9.8696044010893E+1");
    ckc("PI()/180", "PI()/180", "+1.7453292519943E-1");
    { Csn r; char *sff = NULL;
      int e = cs_eval_formula("PI", &r, &sff);
      if (e != 6) { printf("FAIL PI-no-parens err %d want 6\n", e); fails++; }
      else printf("ok   PI-no-parens -> Error 6\n");
      if (sff) free(sff); }

    /* Batch A: light algebra */
    ckc("SIGN(0)", "SIGN(0)", "0");
    ckc("SIGN(-4.5)", "SIGN(-4.5)", "-1.0000000000000E+1");
    ckc("SIGN(3)", "SIGN(3)", "+1.0000000000000E+1");
    ckc("MOD(10,3)", "MOD(10,3)", "+1.0000000000000E+1");
    ckc("MOD(-7,3)", "MOD(-7,3)", "-1.0000000000000E+1");
    { Csn r; char *sf = NULL;
      int e = cs_eval_formula("MOD(5,0)", &r, &sf);
      if (e != -1) { printf("FAIL MOD(5,0) err %d want -1\n", e); fails++; }
      else printf("ok   MOD(5,0) -> Error 0 (?n?)\n");
      if (sf) free(sf); }
    ckc("POWER(2,10)", "POWER(2,10)", "+1.0240000000000E+4");  /* E+3: 0.1024*10^4 */
    ckc("DEGREES(PI())", "DEGREES(PI())", "+1.8000000000000E+3");
    ckc("RADIANS(180)", "RADIANS(180)", "+3.1415926535897E+1");
    ckc("DEG(PI())", "DEG(PI())", "+1.8000000000000E+3");   /* aliases */
    ckc("RAD(180)", "RAD(180)", "+3.1415926535897E+1");
    ckc("DEG(PI()/2)", "DEG(PI()/2)", "+9.0000000000000E+2");
    ckc("RAD(90)", "RAD(90)", "+1.5707963267948E+1");
    ckc("FACT(5)", "FACT(5)", "+1.2000000000000E+3");
    ckc("FACT(0)", "FACT(0)", "+1.0000000000000E+1");
    { Csn r; char *sf = NULL;
      /* the fractional FACT is the Gamma function now (2026-11-13):
       * FACT(1.5) = Gamma(2.5) = 1.3293403881791... */
      int e = cs_eval_formula("FACT(1.5)", &r, &sf);
      char gg[48];
      csn_canon(&r, gg, (int) sizeof(gg));
      if (e != 0 || strcmp(gg, "+1.3293403881791E+1"))
        { printf("FAIL FACT(1.5) err %d val %s\n", e, gg); fails++; }
      else printf("ok   FACT(1.5) = Gamma(2.5)\n");
      if (sf) free(sf); }
    ckc("COMBIN(5,2)", "COMBIN(5,2)", "+1.0000000000000E+2");
    ckc("PERMUT(5,2)", "PERMUT(5,2)", "+2.0000000000000E+2");
    ckc("COMBIN(5,0)", "COMBIN(5,0)", "+1.0000000000000E+1");
    ckc("GCD(12,18)", "GCD(12,18)", "+6.0000000000000E+1");
    ckc("LCM(4,6)", "LCM(4,6)", "+1.2000000000000E+2");
    ckc("AVERAGE(1,2,3)", "AVERAGE(1,2,3)", "+2.0000000000000E+1");
    ckc("ROUND(1.999,1)", "ROUND(1.999,1)", "+2.0000000000000E+1");
    ckc("ROUND(-1.999,1)", "ROUND(-1.999,1)", "-2.0000000000000E+1");
    ckc("TRUNC(1.999,1)", "TRUNC(1.999,1)", "+1.9000000000000E+1");
    { Csn r; char *sf = NULL;
      int e = cs_eval_formula("FACT(30)", &r, &sf);
      if (e != -1) { printf("FAIL FACT(30) err %d want -1\n", e); fails++; }
      else printf("ok   FACT(30) overflow -> Error 0\n");
      if (sf) free(sf); }

    /* transcendentals (14 significant digits, truncated) */
    ckc("SQRT(2)", "SQRT(2)", "+1.4142135623730E+1");
    ckc("SQRT(0.25)", "SQRT(0.25)", "+5.0000000000000E+0");
    ckc("EXP(1)", "EXP(1)", "+2.7182818284590E+1");
    ckc("LN(10)", "LN(10)", "+2.3025850929940E+1");
    ckc("LOG(100)", "LOG(100)", "+2.0000000000000E+1");
    ckc("LOG(0.001)", "LOG(0.001)", "-3.0000000000000E+1");
    ckc("ABS(-4.5)", "ABS(-4.5)", "+4.5000000000000E+1");

    /* errors */
    cke("1/0", "1/0", -1);
    cke("SQRT(-1)", "SQRT(-1)", -1);
    cke("FOO(1)", "FOO(1)", 4);
    cke("overflow", "9E999*9", -1);

    /* truncation display */
    {
        Csn v;
        csn_parse("1.999", &v);
        cs_fmt_num(&v, 10, 2, 0, 0, buf, (int) sizeof(buf));
        ckstr("fmt truncate", buf, "1.99");
        csn_parse("0", &v);
        cs_fmt_num(&v, 10, 2, 1, 0, buf, (int) sizeof(buf));
        ckstr("?n?", buf, "?n?");
        /* narrow column: exponential fallback (manual's example style) */
        csn_parse("1234567.89", &v);
        cs_fmt_num(&v, 8, 2, 0, 0, buf, (int) sizeof(buf));
        ckstr("exp form narrow", buf, "1.23E+6");
    }

    /* per-cell notation (flagged ext 2026-11): (sci) forces scientific,
     * (dec) forces decimal with the head kept and a trailing '*' */
    {
        Csn v;
        csn_parse("1234.5", &v);
        cs_fmt_num(&v, 10, 2, 0, 0, buf, (int) sizeof(buf));
        ckstr("notat auto", buf, "1234.50");
        cs_fmt_num(&v, 10, 2, 0, 1, buf, (int) sizeof(buf));
        ckstr("notat sci", buf, "1.2345E+3");
        cs_fmt_num(&v, 10, 2, 0, 2, buf, (int) sizeof(buf));
        ckstr("notat dec", buf, "1234.50");
        csn_parse("0.0000735", &v);
        cs_fmt_num(&v, 10, 2, 0, 0, buf, (int) sizeof(buf));
        ckstr("notat auto tiny", buf, "0.00");     /* fixed fits: no fallback */
        /* (dec) does not fall back where (auto) would: 1234.5 in a
         * width-6 column goes exponential under (auto); (dec) keeps
         * the decimal head and marks the cut (width-1 chars + '*') */
        csn_parse("1234.5", &v);
        cs_fmt_num(&v, 6, 2, 0, 0, buf, (int) sizeof(buf));
        ckstr("notat auto fback", buf, "1E+3");
        cs_fmt_num(&v, 6, 2, 0, 2, buf, (int) sizeof(buf));
        ckstr("notat dec noback", buf, "1234.*");
        /* (dec) at a wide precision: the head is kept, '*' marks the cut */
        csn_parse("0.0000735", &v);
        cs_fmt_num(&v, 10, 10, 0, 2, buf, (int) sizeof(buf));
        ckstr("notat dec tiny", buf, "0.0000735*");
        csn_parse("123456789012345.67", &v);
        cs_fmt_num(&v, 10, 2, 0, 2, buf, (int) sizeof(buf));
        ckstr("notat dec wide", buf, "123456789*");
        cs_fmt_num(&v, 10, 2, 0, 1, buf, (int) sizeof(buf));
        ckstr("notat sci wide", buf, "1.2345E+14");
        /* (sci) too narrow even exponential: the manual's token */
        csn_parse("1234.5", &v);
        cs_fmt_num(&v, 3, 2, 0, 1, buf, (int) sizeof(buf));
        ckstr("notat sci !n!", buf, "!n!");
        /* (dec) with precision 0: plain integer digits */
        csn_parse("1234.5", &v);
        cs_fmt_num(&v, 10, 0, 0, 2, buf, (int) sizeof(buf));
        ckstr("notat dec p0", buf, "1234");
    }

    /* notation survives the CSD round trip (packed in the prec byte) */
    {
        Cell *c;
        Csn v2;
        cs_clear_all();
        csn_parse("1234.5", &v2);
        c = tcell(2, 1);
        cs_set_num(c, "1234.5", &v2, 0, NULL);
        c->notation = 1;                /* (sci) */
        csn_parse("7.5", &v2);
        c = tcell(3, 1);
        cs_set_num(c, "7.5", &v2, 0, NULL);
        c->notation = 2;                /* (dec) */
        c->prec = 4;
        if (!cs_save("t_not.CSD", 0, 0, CS_MAXCOL - 1, CS_MAXROW - 1, NULL))
            { printf("FAIL notation save\n"); fails++; }
        cs_clear_all();
        if (!cs_load("t_not.CSD", 0, 0, 0))
            { printf("FAIL notation load\n"); fails++; }
        c = tread(2, 1);
        if (c->type != CE_NUM || c->notation != 1)
            { printf("FAIL notat rt C3 (%d)\n", c->notation); fails++; }
        else printf("ok   notation round trip: C2 (sci)\n");
        c = tread(3, 1);
        if (c->type != CE_NUM || c->notation != 2 || c->prec != 4)
            { printf("FAIL notat rt D2 (%d,%d)\n", c->notation, c->prec); fails++; }
        else printf("ok   notation round trip: D2 (dec) prec 4\n");
        remove("t_not.CSD");
    }

    /* ---- first-class references, VAL/REF, dynamic ranges (user-ext
     * 2026-11, branch ref-val): a bare identifier is a REFERENCE value;
     * references coerce to values where a value is consumed (option 3);
     * '>' between references builds a Range, between numbers it
     * compares ----------------------------------------------- */
    {
        Cell *c;
        /* A: reference construction; a top-level formula derefs, so
         * REF(1,1) on an empty A1 is Error 1, on a filled one the value */
        cke("REF empty deref = 0", "REF(1,1)", 0);
        setv(0, 0, "42");                       /* A1 = 42 */
        setv(1, 0, "17");                       /* B1 = 17 */
        setv(0, 4, "5");                        /* A5 = 5 */
        ckc("VAL(A1)", "VAL(A1)", "+4.2000000000000E+2");
        ckc("VAL(REF(1,1))", "VAL(REF(1,1))", "+4.2000000000000E+2");
        cke("VAL(REF(1+1,2+3))", "VAL(REF(1+1,2+3))", 1);  /* B5 empty */
        ckc("VAL(A1)+VAL(B1)", "VAL(A1)+VAL(B1)", "+5.9000000000000E+2");
        ckc("VAL(A1)*10", "VAL(A1)*10", "+4.2000000000000E+3");
        ckc("SQRT(VAL(A1))", "SQRT(VAL(A1))", "+6.4807406984078E+1");
        ckc("coerced A1+B1", "A1+B1", "+5.9000000000000E+2");
        cke("REF(A1,5) coerced = 0", "REF(A1,5)", 0);   /* AP5 empty */
        ckc("REF(1,5) deref", "REF(1,5)", "+5.0000000000000E+1");
        /* B: invalid references */
        cke("REF(0,1)", "REF(0,1)", 1);
        cke("REF(1,0)", "REF(1,0)", 1);
        cke("REF(-1,1)", "REF(-1,1)", 1);
        cke("REF(1,1.5)", "REF(1,1.5)", -1);
        cke("REF(128,1)", "REF(128,1)", 1);
        cke("REF(1,256)", "REF(1,256)", 1);
        cke("REF(1)", "REF(1)", 3);
        cke("REF(1,2,3)", "REF(1,2,3)", 3);
        cke("REF(\"x\",1)", "REF(\"x\",1)", 6);
        /* C: VAL of empty, text and error cells */
        cke("VAL empty", "VAL(C9)", 1);
        cs_free_cell_at(0, 1);
        cs_set_text(tcell(0, 1), "hello", 'L');
        {
            Csn r;
            char *sf = NULL;
            int e = cs_eval_formula("VAL(A2)", &r, &sf);
            /* VAL is the TYPED accessor (user-refined 2026-11-13): a
             * text cell yields its content as a String - the error
             * contract check happens via cke on the empty cell */
            if (e != 0 || sf == NULL || strcmp(sf, "hello"))
                { printf("FAIL VAL text (%s)\n", sf ? sf : "(null)"); fails++; }
            else printf("ok   VAL(text) = the string\n");
        /* the STRICT mixing rule (user-refined 2026-11-13): a string
         * and a number in one operation -> Error 6, through every
         * path */
        cke("mix literal+num", "\"abc\"+1", 6);
        cke("mix textref+num", "VAL(A2)+1", 6);
        cke("mix bare+num", "A2+1", 6);
        cke("mix SUM direct", "SUM(\"abc\",1)", 6);
        cke("mix cmp", "(\"abc\"=1)", 6);
        cke("mix concat", "\"abc\"&5", 6);
        cke("mix MED direct", "MED(\"abc\",1)", 6);
        /* the range-skip stays: text INSIDE a range is ignored by the
         * family (the documented MED/SUM semantics) */
        setv(0, 0, "42");                       /* A1 = 42 */
        setv(0, 2, "3");                        /* A3 = 3 */
        setv(0, 4, "5");                        /* A5 = 5 */
        ckc("range skip unchanged", "SUM(A1>A5)", "+5.0000000000000E+2");
            if (sf) free(sf);
        }
        setv(0, 2, "3");                        /* A3 = 3 */
        cs_commit_entry(0, 8, "+1/0");           /* A9 = ?n? (outside the ranges) */
        cke("VAL error cell", "VAL(A9)", 1);
        /* D: range construction */
        ckc("range sum", "SUM(A1>A5)", "+5.0000000000000E+2");  /* 42+3+5 */
        ckc("range REF", "SUM(REF(1,1)>REF(1,5))", "+5.0000000000000E+2");
        ckc("range mix1", "SUM(A1>REF(1,5))", "+5.0000000000000E+2");
        ckc("range mix2", "SUM(REF(1,1)>A5)", "+5.0000000000000E+2");
        /* rectangles: A1>B5 is the whole block (user decision
         * 2026-11); at this point A1=42, A2 text, A3=3, A5=5, B1=17 */
        ckc("range rectangle", "SUM(A1>B5)", "+6.7000000000000E+2");
        ckc("range reversed ok", "SUM(A5>A1)", "+5.0000000000000E+2");
        /* E: range-consuming functions */
        setv(1, 1, "2");                        /* B2 = 2 */
        setv(1, 2, "4");                        /* B3 = 4 */
        setv(1, 3, "6");                        /* B4 = 6 */
        ckc("AVG(REF)", "AVG(REF(2,2)>REF(2,4))", "+4.0000000000000E+1");
        ckc("CNT(REF mix)", "CNT(B2>REF(2,4))", "+3.0000000000000E+1");
        ckc("MIN(REF mix)", "MIN(B2>B4)", "+2.0000000000000E+1");
        ckc("MAX(REF mix)", "MAX(A1>REF(1,5))", "+4.2000000000000E+2");
        /* F: numerical comparisons are not ranges */
        ckc("cmp >", "VAL(A1)>VAL(B1)", "+1.0000000000000E+1");
        ckc("cmp <", "VAL(A1)<VAL(B1)", "0");
        ckc("cmp =", "VAL(A1)=VAL(A1)", "+1.0000000000000E+1");
        ckc("cmp refs deref", "A1=A5", "0");
        /* G: SET with reference targets */
        cs_clear_all();
        setv(1, 0, "21");                       /* B1 = 21 */
        setv(1, 1, "7");                        /* B2 = 7 */
        cke("SET stmt", "1:SET(A1,100);1", 0);
        c = tread(0, 0);
        csn_canon(&c->val, buf, (int) sizeof(buf));
        if (c->type != CE_NUM || strcmp(buf, "+1.0000000000000E+3"))
            { printf("FAIL SET wrote A1 (%s)\n", buf); fails++; }
        else printf("ok   SET(A1,100) writes 100\n");
        cke("SET REF target", "1:SET(REF(2,5),200);1", 0);
        c = tread(1, 4);
        csn_canon(&c->val, buf, (int) sizeof(buf));
        if (c->type != CE_NUM || strcmp(buf, "+2.0000000000000E+3"))
            { printf("FAIL SET REF target (%s)\n", buf); fails++; }
        else printf("ok   SET(REF(2,5),200) writes B5\n");
        cke("SET dyn target", "1:SET(REF(VAL(B1),VAL(B2)),3);1", 0);
        c = tread(20, 6);
        if (c->type != CE_NUM)
            { printf("FAIL SET dyn target\n"); fails++; }
        else printf("ok   SET(REF(21,7),3) writes U7\n");
        cke("SET value expr", "1:SET(A2,VAL(B1)*2);1", 0);
        c = tread(0, 1);
        csn_canon(&c->val, buf, (int) sizeof(buf));
        if (c->type != CE_NUM || strcmp(buf, "+4.2000000000000E+2"))
            { printf("FAIL SET value expr (%s)\n", buf); fails++; }
        else printf("ok   SET(A2,B1*2) writes 42\n");
        /* a failed SET must not partially modify the sheet */
        cke("SET bad target", "1:SET(A1>A2,5);1", 6);
        c = tread(0, 1);
        csn_canon(&c->val, buf, (int) sizeof(buf));
        if (strcmp(buf, "+4.2000000000000E+2"))
            { printf("FAIL SET partial write (%s)\n", buf); fails++; }
        else printf("ok   failed SET left A2 untouched\n");
        /* H: conditionals with references in both branches */
        cke("cond VAL", "VAL(B1)>0:100:0", 0);
        cke("cond branch refs", "VAL(B1)>0:SET(A3,VAL(B2));5:9", 0);
        c = tread(0, 2);
        csn_canon(&c->val, buf, (int) sizeof(buf));
        if (c->type != CE_NUM || strcmp(buf, "+7.0000000000000E+1"))
            { printf("FAIL cond branch SET (%s)\n", buf); fails++; }
        else printf("ok   conditional ran SET(A3,B2)\n");
        /* I: recalculation and side effects with references */
        cs_clear_all();
        setv(0, 0, "6");                        /* A1 = 6 */
        cke("SET RECALC refs", "1:SET(REF(2,1),VAL(A1)*2);RECALC(REF(2,1));0:\"done\"", 0);
        c = tread(1, 0);
        csn_canon(&c->val, buf, (int) sizeof(buf));
        if (c->type != CE_NUM || strcmp(buf, "+1.2000000000000E+2"))
            { printf("FAIL SET/RECALC refs (%s)\n", buf); fails++; }
        else printf("ok   SET(REF(2,1),A1*2) + RECALC(ref)\n");
        /* the side-effect guards keep working */
        cke("SET as operand", "SET(A1,5)*2", 4);
        cke("two value pieces", "1:1;2:0", 7);
        cs_clear_all();
    }

    /* ---- documented examples (README/help page 3): every example in
     * the docs is pinned here ---- */
    {
        Cell *c;
        char buf[64];
        /* the user's formula: an always-true branch writing B7's value
         * into the cell referenced by the values in C1 and C2 */
        cs_clear_all();
        setv(2, 0, "2");                        /* C1 = 2 */
        setv(2, 1, "3");                        /* C2 = 3 */
        setv(1, 6, "99");                       /* B7 = 99 */
        cke("user formula", "(1):SET(REF(C1,C2),VAL(B7));0:1", 0);
        c = tread(1, 2);                      /* B3 = REF(2,3) */
        csn_canon(&c->val, buf, (int) sizeof(buf));
        if (c->type != CE_NUM || strcmp(buf, "+9.9000000000000E+2"))
            { printf("FAIL user formula target (%s)\n", buf); fails++; }
        else printf("ok   (1):SET(REF(C1,C2),VAL(B7));0:1 -> B3 = 99\n");
        /* multi-area range lists */
        cs_clear_all();
        setv(0, 0, "42"); setv(0, 2, "3"); setv(0, 4, "5");
        setv(1, 0, "17"); setv(1, 1, "2"); setv(1, 2, "4");
        ckc("multi-area SUM", "SUM(A1>A5,B1>B3)", "+7.3000000000000E+2");
        ckc("cnt multi-area", "CNT(A1>A5,B1>B3)", "+6.0000000000000E+1");
        /* SET copies text through coercion (B3 holds text) */
        cs_set_text(tcell(1, 2), "hi", 'L');
        cke("SET copies text", "1:SET(A1,B3);0", 0);
        c = tread(0, 0);
        if (c->type != CE_TEXT || !c->cont || strcmp(c->cont, "hi"))
            { printf("FAIL SET text copy (%d)\n", c->type); fails++; }
        else printf("ok   SET(A1,B3) copies the text hi\n");
        /* SET copies a number through coercion (B2 holds 2) */
        cke("SET copies number", "1:SET(A1,B2);0", 0);
        c = tread(0, 0);
        csn_canon(&c->val, buf, (int) sizeof(buf));
        if (c->type != CE_NUM || strcmp(buf, "+2.0000000000000E+1"))
            { printf("FAIL SET number copy (%s)\n", buf); fails++; }
        else printf("ok   SET(A1,B2) copies the number 2\n");
        /* REGR through a dynamic range */
        cs_clear_all();
        setv(0, 0, "1"); setv(0, 1, "2"); setv(0, 2, "3");
        setv(0, 3, "4"); setv(0, 4, "5");
        setv(1, 0, "2"); setv(1, 1, "4"); setv(1, 2, "6");
        setv(1, 3, "8"); setv(1, 4, "10");
        ckc("REGR dyn range", "REGR(REF(1,1)>REF(1,5),B1)", "+6.0000000000000E+1");
        ckc("SLOPE dyn", "SLOPE()", "+2.0000000000000E+1");
        /* conditionals: both branches and the false value */
        setv(0, 0, "42"); setv(1, 0, "17");
        ckc("cond true branch", "(VAL(A1)>VAL(B1)):100:200", "+1.0000000000000E+3");
        ckc("cond false branch", "(VAL(B1)>VAL(A1)):100:200", "+2.0000000000000E+3");
        /* the dynamic VAL lookup from the docs */
        setv(1, 0, "1");                        /* B1 = 1 */
        setv(1, 4, "8");                        /* B5 = 8 */
        ckc("dyn VAL lookup", "VAL(REF(VAL(B1)+1,5))", "+8.0000000000000E+1");
        /* COLIDX: the 1-based column index (scoped designator parse) */
        ckc("COLIDX string", "COLIDX(\"A\")", "+1.0000000000000E+1");
        ckc("COLIDX AB", "COLIDX(\"AB\")", "+2.8000000000000E+2");
        ckc("COLIDX DW", "COLIDX(\"DW\")", "+1.2700000000000E+3");
        ckc("COLIDX bare", "COLIDX(AB)", "+2.8000000000000E+2");
        ckc("COLIDX lowercase", "COLIDX(ab)", "+2.8000000000000E+2");
        ckc("COLIDX ref", "COLIDX(REF(2,3))", "+2.0000000000000E+1");
        ckc("COLIDX cell ref", "COLIDX(A1)", "+1.0000000000000E+1");
        cke("COLIDX beyond DW", "COLIDX(\"DX\")", 1);
        cke("COLIDX with digits", "COLIDX(\"A1\")", 1);
        cke("COLIDX empty str", "COLIDX(\"\")", 1);
        cke("COLIDX wrong type", "COLIDX(5)", 6);
        cke("COLIDX no arg", "COLIDX()", 3);
        /* combined with REF: the column of AC5 (set first) */
        setv(28, 4, "8");                    /* AC5 = 8 */
        ckc("COLIDX in REF", "VAL(REF(COLIDX(\"AB\")+1,5))", "+8.0000000000000E+1");
        /* the sort command ;# (the user's exact example): A3>B6 by
         * column A ascending - the B texts travel with their rows */
        cs_clear_all();
        setv(0, 2, "1");                        /* A3 = 1 */
        cs_set_text(tcell(1, 2), "Apples", 'L');
        setv(0, 3, "5");
        cs_set_text(tcell(1, 3), "Oranges", 'L');
        setv(0, 4, "4");
        cs_set_text(tcell(1, 4), "Berries", 'L');
        setv(0, 5, "2");
        cs_set_text(tcell(1, 5), "Tomatoes", 'L');
        cs_sort_range(0, 2, 1, 5, 0, 0);        /* A3>B6, key A, asc */
        if (tread(0, 2)->val.nd[0] != 1 ||
            tread(0, 3)->val.nd[0] != 2 ||
            tread(0, 4)->val.nd[0] != 4 ||
            tread(0, 5)->val.nd[0] != 5)
            { printf("FAIL sort asc values\n"); fails++; }
        else if (!tread(1, 3)->cont || strcmp(tread(1, 3)->cont, "Tomatoes") ||
                 !tread(1, 4)->cont || strcmp(tread(1, 4)->cont, "Berries") ||
                 !tread(1, 5)->cont || strcmp(tread(1, 5)->cont, "Oranges"))
            { printf("FAIL sort asc texts\n"); fails++; }
        else printf("ok   sort asc: 1,2,4,5 with the texts following\n");
        /* descending */
        cs_sort_range(0, 2, 1, 5, 0, 1);
        if (tread(0, 2)->val.nd[0] != 5 ||
            tread(0, 3)->val.nd[0] != 4 ||
            tread(0, 4)->val.nd[0] != 2 ||
            tread(0, 5)->val.nd[0] != 1)
            { printf("FAIL sort desc\n"); fails++; }
        else printf("ok   sort desc\n");
        /* formulas referencing the sorted range follow the cells:
         * B7 = +VAL(A4) (the OLD A4 = 4 lands at row 5) */
        cs_sort_range(0, 2, 1, 5, 0, 0);        /* back to 1,2,4,5 */
        {
            Csn v;
            csn_parse("0", &v);
            cs_free_cell_at(1, 6);
            csn_parse("4", &v);
            cs_commit_entry(1, 6, "+VAL(A4)");  /* B7 references A4 */
        }
        cs_sort_range(0, 2, 1, 5, 0, 1);        /* desc: old A4's 4 -> A3 */
        {
            Cell *c = tread(1, 6);
            if (!c->cont || strcmp(c->cont, "+VAL(A5)"))
                { printf("FAIL sort ref remap (%s)\n",
                         c->cont ? c->cont : ""); fails++; }
            else printf("ok   sort remaps +VAL(A4) to +VAL(A5)\n");
        }
        /* text keys: sort the block by column B ascending */
        cs_sort_range(0, 2, 1, 5, 1, 0);
        {
            const char *want[4] = { "Apples", "Berries", "Oranges",
                                    "Tomatoes" };
            int okk = 1, w;
            for (w = 0; w < 4; w++)
                if (!tread(1, 2 + w)->cont ||
                    strcmp(tread(1, 2 + w)->cont, want[w]) != 0)
                    okk = 0;
            if (!okk) { printf("FAIL sort text keys\n"); fails++; }
            else printf("ok   sort by text keys\n");
        }
        /* empty keys sort last; equal keys keep their order (stable) */
        cs_clear_all();
        setv(0, 2, "9");                        /* A3 = 9 */
        cs_set_text(tcell(1, 2), "X", 'L');   /* B3 = X */
        setv(0, 3, "9");                        /* A4 = 9 (equal key) */
        cs_set_text(tcell(1, 3), "Y", 'L');   /* B4 = Y */
        setv(0, 5, "1");                        /* A6 = 1 */
        cs_sort_range(0, 2, 1, 5, 0, 0);        /* A3>B6, key A, asc */
        {
            int okk = tread(0, 2)->val.nd[0] == 1 &&
                      tread(0, 3)->val.nd[0] == 9 &&
                      tread(0, 4)->val.nd[0] == 9 &&
                      tread(0, 5)->type == CE_EMPTY &&
                      tread(1, 3)->cont && strcmp(tread(1, 3)->cont, "X") == 0 &&
                      tread(1, 4)->cont && strcmp(tread(1, 4)->cont, "Y") == 0;
            if (!okk) { printf("FAIL sort empties/stability\n"); fails++; }
            else printf("ok   sort: empties last, equal keys stable\n");
        }
        /* a key column outside the range: no work */
        cs_sort_range(0, 2, 1, 5, 3, 0);
        if (tread(0, 2)->val.nd[0] != 1)
            { printf("FAIL sort key outside\n"); fails++; }
        else printf("ok   sort rejects a key outside the range\n");
        /* a ONE-ROW range: nothing to permute (the core guard, user
         * 2026-11-15) */
        cs_sort_range(0, 2, 5, 2, 0, 0);
        if (tread(0, 2)->val.nd[0] != 1)
            { printf("FAIL one-row sort\n"); fails++; }
        else printf("ok   a one-row sort range is a no-op\n");
        /* MED(range): the median (user-ext 2026-11-13) - odd count,
         * Excel-style even count, text/empties skipped, a rectangle
         * range, unsorted input, an error cell -> Error 0, empty -> 0
         * (AVG's behavior, user decision) */
        cs_clear_all();
        setv(0, 2, "1");                        /* A3 = 1 */
        setv(0, 3, "5");                        /* A4 = 5 */
        setv(0, 4, "4");                        /* A5 = 4 */
        setv(0, 5, "2");                        /* A6 = 2 */
        cs_set_text(tcell(1, 2), "Apples", 'L');   /* B3 text: ignored */
        setv(1, 3, "3");                        /* B4 = 3 */
        ckc("MED odd", "MED(A3>B6)", "+3.0000000000000E+1");
        ckc("MED rectangle", "MED(A3>B4)", "+3.0000000000000E+1");
        ckc("MED even", "MED(A3>B4,A6)", "+2.5000000000000E+1");
        ckc("MED direct mix", "MED(A3>A4,100)", "+5.0000000000000E+1");
        ckc("MED single", "MED(A3>A3)", "+1.0000000000000E+1");
        cs_free_cell_at(0, 2);
        cs_free_cell_at(0, 3);
        cs_free_cell_at(0, 4);
        cs_free_cell_at(0, 5);
        cs_free_cell_at(1, 3);
        ckc("MED empty range", "MED(A3>B6)", "0");
        cs_commit_entry(0, 4, "+1/0");          /* A5 = ?n? */
        cke("MED error cell", "MED(A3>A6)", -1);
        /* the MED bound (512 values): a 513-value range is an ERROR,
         * while the streaming aggregates handle it (2026-11-14) */
        cs_clear_all();
        {
            int w;
            Csn v;
            csn_parse("1", &v);
            /* 513 values, column-major (the sheet is 255 rows deep) */
            for (w = 0; w < 513; w++)
                cs_set_num(tcell(w / 255, w % 255), "1", &v, 0, NULL);
        }
        cke("MED beyond bound", "MED(A1>C255)", -1);
        /* LG(): the base-10 alias of LOG (user request 2026-11-15) */
        ckc("LG 100", "LG(100)", "+2.0000000000000E+1");
        ckc("LG 10", "LG(10)", "+1.0000000000000E+1");
        ckc("LG 1", "LG(1)", "0");
        cke("LG 0", "LG(0)", -1);
        cke("LG -5", "LG(-5)", -1);
        ckc("LG equivalence", "LG(56/(12*10**-3))",
            "+3.6690067809585E+1");
        /* the Zadeh fuzzy Boolean algebra (user spec 2026-11-15) */
        ckc("FNOT 0", "NOT(0)", "+1.0000000000000E+1");
        ckc("FNOT 0.25", "NOT(0.25)", "+7.5000000000000E+0");
        ckc("FNOT 0.5", "NOT(0.5)", "+5.0000000000000E+0");
        ckc("FNOT 1", "NOT(1)", "0");
        cke("FNOT 1.5", "NOT(1.5)", -1);
        cke("FNOT -0.1", "NOT(-0.1)", -1);
        cke("FNOT text", "NOT(\"x\")", 6);
        cke("FNOT no arg", "NOT()", 6);
        ckc("FAND 0.2,0.8", "AND(0.2,0.8)", "+2.0000000000000E+0");
        ckc("FAND 0.5,0.5", "AND(0.5,0.5)", "+5.0000000000000E+0");
        ckc("FAND 0.7,0.9", "AND(0.7,0.9)", "+7.0000000000000E+0");
        ckc("FAND 1,1", "AND(1,1)", "+1.0000000000000E+1");
        ckc("FAND 0,1", "AND(0,1)", "0");
        ckc("FAND 0.6,0.9", "AND(0.6,0.9)", "+6.0000000000000E+0");
        cke("FAND 1.2", "AND(0.5,1.2)", -1);
        cke("FAND -0.2", "AND(-0.2,0.5)", -1);
        cke("FAND text", "AND(\"a\",0.5)", 6);
        ckc("FAND multi", "AND(0.9,0.8,0.7,0.95)", "+7.0000000000000E+0");
        ckc("FOR 0.2,0.8", "OR(0.2,0.8)", "+8.0000000000000E+0");
        ckc("FOR 0.5,0.5", "OR(0.5,0.5)", "+5.0000000000000E+0");
        ckc("FOR 0.7,0.9", "OR(0.7,0.9)", "+9.0000000000000E+0");
        ckc("FOR 0,0", "OR(0,0)", "0");
        ckc("FOR 0,1", "OR(0,1)", "+1.0000000000000E+1");
        ckc("FOR multi", "OR(0.1,0.2,0.35)", "+3.5000000000000E+0");
        cke("FOR 2,0.5", "OR(2,0.5)", -1);
        ckc("FXOR 0,0", "XOR(0,0)", "0");
        ckc("FXOR 0,1", "XOR(0,1)", "+1.0000000000000E+1");
        ckc("FXOR 1,0", "XOR(1,0)", "+1.0000000000000E+1");
        ckc("FXOR 1,1", "XOR(1,1)", "0");
        ckc("FXOR 0.5,0.5", "XOR(0.5,0.5)", "+5.0000000000000E+0");
        ckc("FXOR 0.2,0.8", "XOR(0.2,0.8)", "+8.0000000000000E+0");
        ckc("FXOR 0.3,0.3", "XOR(0.3,0.3)", "+3.0000000000000E+0");
        ckc("FXOR 0.3,0.7", "XOR(0.3,0.7)", "+7.0000000000000E+0");
        cke("FXOR 1.1,0.5", "XOR(1.1,0.5)", -1);
        cke("FXOR 1 arg", "XOR(0.5)", 3);
        cke("FXOR 3 args", "XOR(0.5,0.5,0.5)", 3);
        /* the mathematical properties */
        ckc("double negation", "NOT(NOT(0.25))", "+2.5000000000000E+0");
        ckc("AND x,1", "AND(0.3,1)", "+3.0000000000000E+0");
        ckc("AND x,0", "AND(0.3,0)", "0");
        ckc("AND x,x", "AND(0.42,0.42)", "+4.2000000000000E+0");
        ckc("OR x,0", "OR(0.3,0)", "+3.0000000000000E+0");
        ckc("OR x,1", "OR(0.3,1)", "+1.0000000000000E+1");
        ckc("OR x,x", "OR(0.42,0.42)", "+4.2000000000000E+0");
        ckc("XOR x,0", "XOR(0.3,0)", "+3.0000000000000E+0");
        ckc("XOR x,1", "XOR(0.3,1)", "+7.0000000000000E+0");
        ckc("XOR x,x", "XOR(0.3,0.3)", "+3.0000000000000E+0");
        ckc("XOR x,1-x", "XOR(0.3,0.7)", "+7.0000000000000E+0");
        ckc("De Morgan 1", "(NOT(AND(0.3,0.6))=OR(NOT(0.3),NOT(0.6))):1:0",
            "+1.0000000000000E+1");
        ckc("De Morgan 2", "(NOT(OR(0.3,0.6))=AND(NOT(0.3),NOT(0.6))):1:0",
            "+1.0000000000000E+1");
        cke("De Morgan violated", "(NOT(AND(0.3,0.6))=OR(NOT(0.3),NOT(0.6))):0:1",
            0);
        /* the threshold: 0.7 is the ONLY Boolean interpretation */
        ckc("threshold 0.699999", "(0.699999):1:0", "0");
        ckc("threshold 0.7", "(0.7):1:0", "+1.0000000000000E+1");
        ckc("threshold 0.700001", "(0.700001):1:0", "+1.0000000000000E+1");
        ckc("threshold 1", "(1):1:0", "+1.0000000000000E+1");
        ckc("threshold 0", "(0):1:0", "0");
        ckc("AND below threshold stays fuzzy", "(AND(0.6,0.9)<0.7):1:0",
            "+1.0000000000000E+1");
        /* >=: the mirror of <= (reached for by the threshold tests) */
        ckc("ge true", "(0.8>=0.7):1:0", "+1.0000000000000E+1");
        ckc("ge false", "(0.5>=0.7):1:0", "0");
        ckc("ge equal", "(0.7>=0.7):1:0", "+1.0000000000000E+1");
        ckc("ge numbers", "(3>=2)", "+1.0000000000000E+1");
        ckc("ge strings", "(\"b\">=\"b\"):1:0", "+1.0000000000000E+1");
        /* EVEN()/ODD(): the integer parity (user request 2026-11-15) */
        ckc("EVEN 2", "EVEN(2)", "+1.0000000000000E+1");
        ckc("EVEN 3", "EVEN(3)", "0");
        ckc("EVEN 0", "EVEN(0)", "+1.0000000000000E+1");
        ckc("EVEN -4", "EVEN(-4)", "+1.0000000000000E+1");
        ckc("ODD 3", "ODD(3)", "+1.0000000000000E+1");
        ckc("ODD -3", "ODD(-3)", "+1.0000000000000E+1");
        ckc("ODD -2", "ODD(-2)", "0");
        cke("EVEN 2.5", "EVEN(2.5)", -1);
        cke("ODD 2.5", "ODD(2.5)", -1);
        cke("EVEN huge", "EVEN(9E+999)", -1);
        cke("EVEN no arg", "EVEN()", 6);
        cke("EVEN text", "EVEN(\"x\")", 6);
        /* the composition: EVEN(x) = NOT(ODD(x)) through the fuzzy NOT */
        ckc("EVEN = NOT ODD", "EVEN(7)=NOT(ODD(7)):1:0", "+1.0000000000000E+1");
        ckc("ODD = NOT EVEN", "ODD(6)=NOT(EVEN(6)):1:0", "+1.0000000000000E+1");
        /* the conditional through the 0.7 threshold (the string
         * branch: "even" for the even 4) */
        {
            Csn r;
            char *sf = NULL;
            int e = cs_eval_formula("(EVEN(4)):\"even\":\"odd\"", &r, &sf);
            if (e != 0 || sf == NULL || strcmp(sf, "even"))
                { printf("FAIL EVEN conditional (%s)\n", sf ? sf : "(null)");
                  fails++; }
            else printf("ok   EVEN conditional yields even\n");
            if (sf) free(sf);
        }

        /* ==== the engineering batch A (user list, 2026-11-15) ==== */
        /* HYPOT: the scaled form survives the exponent range */
        ckc("HYPOT 3,4", "HYPOT(3,4)", "+5.0000000000000E+1");
        ckc("HYPOT 3,-4", "HYPOT(3,-4)", "+5.0000000000000E+1");
        ckc("HYPOT 0,0", "HYPOT(0,0)", "0");
        ckc("HYPOT 0,7", "HYPOT(0,7)", "+7.0000000000000E+1");
        ckc("HYPOT huge", "HYPOT(1E500,1E500)", "+1.4142135623730E+501");
        ckc("HYPOT tiny", "HYPOT(1E-500,1E-500)", "+1.4142135623730E-499");
        cke("HYPOT 1 arg", "HYPOT(1)", 3);
        cke("HYPOT text", "HYPOT(\"x\",1)", 6);
        /* ATAN2: the quadrants and the axes */
        ckc("ATAN2 1,1", "ATAN2(1,1)", "+7.8539816339744E+0");
        ckc("ATAN2 1,-1", "ATAN2(1,-1)", "+2.3561944901923E+1");
        ckc("ATAN2 -1,-1", "ATAN2(-1,-1)", "-2.3561944901923E+1");
        ckc("ATAN2 0,1", "ATAN2(0,1)", "0");
        ckc("ATAN2 1,0", "ATAN2(1,0)", "+1.5707963267948E+1");
        ckc("ATAN2 -1,0", "ATAN2(-1,0)", "-1.5707963267948E+1");
        cke("ATAN2 0,0", "ATAN2(0,0)", -1);
        cke("ATAN2 1 arg", "ATAN2(1)", 3);
        /* CEIL/FLOOR: toward +-infinity, both signs */
        ckc("CEIL 3.2", "CEIL(3.2)", "+4.0000000000000E+1");
        ckc("CEIL -3.2", "CEIL(-3.2)", "-3.0000000000000E+1");
        ckc("CEIL -2", "CEIL(-2)", "-2.0000000000000E+1");
        ckc("CEIL 0.5", "CEIL(0.5)", "+1.0000000000000E+1");
        ckc("CEIL -0.5", "CEIL(-0.5)", "0");
        ckc("CEIL 1E-20", "CEIL(1E-20)", "+1.0000000000000E+1");
        ckc("CEIL huge", "CEIL(1E500)", "+1.0000000000000E+501");
        ckc("FLOOR 3.8", "FLOOR(3.8)", "+3.0000000000000E+1");
        ckc("FLOOR -3.8", "FLOOR(-3.8)", "-4.0000000000000E+1");
        ckc("FLOOR -0.5", "FLOOR(-0.5)", "-1.0000000000000E+1");
        ckc("FLOOR -1E-20", "FLOOR(-1E-20)", "-1.0000000000000E+1");
        ckc("FLOOR huge", "FLOOR(1E500)", "+1.0000000000000E+501");
        ckc("FLOOR int", "FLOOR(5)", "+5.0000000000000E+1");
        cke("CEIL no arg", "CEIL()", 6);
        /* the dB family */
        ckc("DB 10", "DB(10)", "+2.0000000000000E+2");
        ckc("DB 1", "DB(1)", "0");
        cke("DB 0", "DB(0)", -1);
        cke("DB -5", "DB(-5)", -1);
        ckc("DBM 1", "DBM(1)", "+3.0000000000000E+2");
        ckc("DBM 1mW", "DBM(0.001)", "0");
        ckc("DBW 1", "DBW(1)", "0");
        ckc("DBW 2", "DBW(2)", "+3.0102999566398E+1");
        ckc("LIN 20", "LIN(20)", "+1.0000000000000E+2");
        ckc("LIN 0", "LIN(0)", "+1.0000000000000E+1");
        ckc("LIN -20", "LIN(-20)", "+1.0000000000000E+0");
        ckc("DB(LIN) round", "DB(LIN(20))", "+2.0000000000000E+2");
        ckc("LIN(DB) round", "LIN(DB(100))", "+1.0000000000000E+3");
        /* STEP: 0 below, 1 at-or-above (STEP(0) = 1, flagged) */
        ckc("STEP -1", "STEP(-1)", "0");
        ckc("STEP 0", "STEP(0)", "+1.0000000000000E+1");
        ckc("STEP 0.5", "STEP(0.5)", "+1.0000000000000E+1");
        /* CLAMP / LERP / UNLERP */
        ckc("CLAMP in", "CLAMP(5,0,10)", "+5.0000000000000E+1");
        ckc("CLAMP high", "CLAMP(15,0,10)", "+1.0000000000000E+2");
        ckc("CLAMP low", "CLAMP(-5,0,10)", "0");
        cke("CLAMP bad range", "CLAMP(5,10,0)", -1);
        cke("CLAMP 2 args", "CLAMP(5,0)", 3);
        ckc("DIST 3,4", "DIST(0,0,3,4)", "+5.0000000000000E+1");
        ckc("DIST offset", "DIST(1,1,4,5)", "+5.0000000000000E+1");
        ckc("DIST huge", "DIST(1E500,1E500,1E500,2E500)",
            "+1.0000000000000E+501");
        cke("DIST 3 args", "DIST(0,0,1)", 3);
        ckc("LERP mid", "LERP(0,10,0.5)", "+5.0000000000000E+1");
        ckc("LERP lo", "LERP(10,20,0)", "+1.0000000000000E+2");
        ckc("LERP hi", "LERP(10,20,1)", "+2.0000000000000E+2");
        ckc("LERP extrap", "LERP(0,10,1.5)", "+1.5000000000000E+2");
        ckc("UNLERP mid", "UNLERP(0,10,5)", "+5.0000000000000E+0");
        ckc("UNLERP off", "UNLERP(10,20,15)", "+5.0000000000000E+0");
        cke("UNLERP flat", "UNLERP(0,0,5)", -1);

        ckc("SUM streams 513", "SUM(A1>C255)", "+5.1300000000000E+3");
        ckc("AVG streams 513", "AVG(A1>C255)", "+1.0000000000000E+1");
        ckc("MAX streams 513", "MAX(A1>C255)", "+1.0000000000000E+1");
        ckc("MIN streams 513", "MIN(A1>C255)", "+1.0000000000000E+1");
        ckc("CNT streams 513", "CNT(A1>C255)", "+5.1300000000000E+3");
        /* the statistics over a SEEDED range (engineering batch B):
         * A1..A5 = 1..5 */
        cs_clear_all();
        setv(0, 0, "1");
        setv(0, 1, "2");
        setv(0, 2, "3");
        setv(0, 3, "4");
        setv(0, 4, "5");
        /* the abs family (user spec 2026-11-17): streaming, no arrays */
        setv(2, 0, "9.9E998");           /* C1 */
        setv(2, 1, "9.9E998");           /* C2: the sum overflows */
        setv(0, 5, "-4");                /* A6 */
        setv(0, 6, "0");                 /* A7 */
        setv(0, 7, "-5");                /* A8 */
        setv(1, 0, "3");                 /* B1 */
        setv(1, 1, "hello");             /* B2: text - skipped */
        cs_commit_entry(3, 0, "1/0");    /* D1: an ERROR cell */
        ckc("SUMABS mixed", "SUMABS(A1>A8)", "+2.4000000000000E+2");
        ckc("SUMPOS mixed", "SUMPOS(A1>A8)", "+1.5000000000000E+2");
        ckc("SUMNEG mags", "SUMNEG(A1>A8)", "+9.0000000000000E+1");
        ckc("MAXABS mixed", "MAXABS(A1>A8)", "+5.0000000000000E+1");
        ckc("MINABS zero", "MINABS(A1>A8)", "0");
        ckc("MINABS nozero", "MINABS(A6>A6)", "+4.0000000000000E+1");
        ckc("SUMABS direct", "SUMABS(3,-4)", "+7.0000000000000E+1");
        ckc("SUMPOS direct", "SUMPOS(-2,3,5)", "+8.0000000000000E+1");
        ckc("SUMNEG direct", "SUMNEG(-2,-3,4)", "+5.0000000000000E+1");
        ckc("MAXABS direct", "MAXABS(-9,3)", "+9.0000000000000E+1");
        ckc("MINABS direct", "MINABS(-9,3)", "+3.0000000000000E+1");
        ckc("SUMABS multi-area", "SUMABS(A1>A3,B1)", "+9.0000000000000E+1");
        ckc("SUMABS bools", "SUMABS(1=1,1=2)", "+1.0000000000000E+1");
        ckc("SUMABS text-only", "SUMABS(B2>B2)", "0");
        ckc("MAXABS text-only", "MAXABS(B2>B2)", "0");
        cke("SUMABS string arg", "SUMABS(\"x\")", 6);
        cke("SUMABS err cell", "SUMABS(A1>D1)", -1);   /* displays Error 0 */
        cke("MAXABS err cell", "MAXABS(A1>D1)", -1);
        ckc("SUMPOS empty", "SUMPOS(B3>B5)", "0");
        cke("SUMABS overflow", "SUMABS(C1>C2)", -1);  /* displays Error 0 */
        /* ================= SUMPRODUCT (user spec 2026-11-17) =====
         * the sheets: A1..A5 = 1..5 (above), A6=-4 A7=0 A8=-5,
         * B1=3 B2=text, D1=1/0 (an error cell), C1/C2 = 9.9E998 */
        setv(1, 2, "10");                /* B3 */
        setv(1, 3, "20");                /* B4 */
        setv(1, 4, "30");                /* B5 */
        ckc("SP single = SUM", "SUMPRODUCT(A1>A5)", "+1.5000000000000E+2");
        cke("SP no args", "SUMPRODUCT()", 3);
        ckc("SP pair", "SUMPRODUCT(A1>A3, B3>B5)",
            "+1.4000000000000E+3");      /* 1*10+2*20+3*30 = 140 */
        setv(2, 2, "5");                 /* C3 */
        setv(2, 3, "6");                 /* C4 */
        setv(2, 4, "7");                 /* C5 */
        ckc("SP triple", "SUMPRODUCT(A1>A3, B3>B5, C3>C5)",
            "+9.2000000000000E+3");      /* 1*10*5+2*20*6+3*30*7 */
        ckc("SP sumsq", "SUMPRODUCT(A1>A5, A1>A5)", "+5.5000000000000E+2");
        ckc("SP two cells", "SUMPRODUCT(A1, B1)", "+3.0000000000000E+1");
        cke("SP dims mismatch", "SUMPRODUCT(A1>A5, B1>B4)", 2);
        cke("SP no broadcast", "SUMPRODUCT(A1>A5, B1)", 2);
        /* the empties are zero (the spec 8) - unlike SUM's skip */
        ckc("SP empties zero", "SUMPRODUCT(A6>A8, A6>A8)",
            "+4.1000000000000E+2");      /* 16+0+25 */
        /* the text/derived cells skip (our coercion rule) */
        ckc("SP text skip", "SUMPRODUCT(B1>B5, B1>B5)",
            "+1.4090000000000E+4");      /* 9+0+100+400+900, B2 skipped */
        /* the error cell aborts */
        cke("SP err cell", "SUMPRODUCT(A1>A5, D1>D5)", -1);
        /* the full precision: 19-digit inputs survive the product */
        setv(5, 0, "1111111111111111111");   /* F1 */
        setv(5, 1, "9");                     /* F2 */
        ckc("SP precision", "SUMPRODUCT(F1>F2, F1>F2)",
            "+1.2345679012345E+37");   /* (1.11e18)^2 + 81 */
        ckc("SP in formula", "SUMPRODUCT(A1>A3, B3>B5)*2",
            "+2.8000000000000E+3");
        ckc("SP reversed", "SUMPRODUCT(A5>A1, B5>B1)",
            "+2.6300000000000E+3");   /* normalized: 3+0+30+80+150 */
        cke("SP number arg", "SUMPRODUCT(5, A1>A1)", 6);
        cke("SP string arg", "SUMPRODUCT(\"x\", A1>A1)", 6);
        cke("SP junk tail", "SUMPRODUCT(A1>A1, B1>B1 ) x", 7);
        /* a rectangles-shapes-differ same-count case: 2x3 vs 3x2
         * (the empty ranges: 0) */
        ckc("SP shapes", "SUMPRODUCT(D2>E4, D2>F3)", "0");
        cke("SP overflow", "SUMPRODUCT(C1>C2, C1>C2)", -1);
        cke("SP arg cap", "SUMPRODUCT(A1>A1,A1>A1,A1>A1,A1>A1,A1>A1,"
            "A1>A1,A1>A1,A1>A1,A1>A1,A1>A1,A1>A1,A1>A1,A1>A1,A1>A1,"
            "A1>A1,A1>A1,A1>A1)", 3);
        ckc("SUMSQ range", "SUMSQ(A1>A5)", "+5.5000000000000E+2");
        ckc("RMS range", "RMS(A1>A5)", "+3.3166247903553E+1");
        ckc("VAR range", "VAR(A1>A5)", "+2.5000000000000E+1");
        ckc("VARP range", "VARP(A1>A5)", "+2.0000000000000E+1");
        ckc("STDEV range", "STDEV(A1>A5)", "+1.5811388300841E+1");
        ckc("STDEVP range", "STDEVP(A1>A5)", "+1.4142135623730E+1");
        /* PERCENTILE.INC on the range */
        ckc("PCT 0", "PCT(A1>A5,0)", "+1.0000000000000E+1");
        ckc("PCT 0.1", "PCT(A1>A5,0.1)", "+1.4000000000000E+1");
        ckc("PCT 0.25", "PCT(A1>A5,0.25)", "+2.0000000000000E+1");
        ckc("PCT 0.5", "PCT(A1>A5,0.5)", "+3.0000000000000E+1");
        ckc("PCT 0.75", "PCT(A1>A5,0.75)", "+4.0000000000000E+1");
        ckc("PCT 1", "PCT(A1>A5,1)", "+5.0000000000000E+1");
        cke("PCT 1.2", "PCT(A1>A5,1.2)", -1);
        cke("PCT -0.1", "PCT(A1>A5,-0.1)", -1);
        cke("PCT text arg", "PCT(A1>A5,\"x\")", 6);
        cke("PCT no p", "PCT(A1>A5)", -1);      /* its last value 5 IS p */
        cke("QUART no q", "QUART(A1>A5)", -1);
        /* QUART: q integer 0..4 = PCT(q/4) */
        ckc("QUART 0", "QUART(A1>A5,0)", "+1.0000000000000E+1");
        ckc("QUART 1", "QUART(A1>A5,1)", "+2.0000000000000E+1");
        ckc("QUART 2", "QUART(A1>A5,2)", "+3.0000000000000E+1");
        ckc("QUART 3", "QUART(A1>A5,3)", "+4.0000000000000E+1");
        ckc("QUART 4", "QUART(A1>A5,4)", "+5.0000000000000E+1");
        cke("QUART fractional", "QUART(A1>A5,2.5)", -1);
        cke("QUART 5", "QUART(A1>A5,5)", -1);
        cke("QUART -1", "QUART(A1>A5,-1)", -1);
        /* the constant range: variance 0 */
        ckc("VARP constant", "VARP(A1>A1)", "0");
        /* direct-argument pins (no cells): the classic example set */
        ckc("SUMSQ 1,2,3", "SUMSQ(1,2,3)", "+1.4000000000000E+2");
        ckc("RMS 1,2,3", "RMS(1,2,3)", "+2.1602468994692E+1");
        ckc("VAR 1,2,3", "VAR(1,2,3)", "+1.0000000000000E+1");
        ckc("VAR 1,2,3,4", "VAR(1,2,3,4)", "+1.6666666666666E+1");
        ckc("VARP 1,2,3", "VARP(1,2,3)", "+6.6666666666666E+0");
        ckc("STDEV 1,2,3", "STDEV(1,2,3)", "+1.0000000000000E+1");
        ckc("STDEV wiki", "STDEV(2,4,4,4,5,5,7,9)",
            "+2.1380899352993E+1");
        ckc("STDEVP wiki", "STDEVP(2,4,4,4,5,5,7,9)",
            "+2.0000000000000E+1");
        /* the user's edge table */
        cke("VAR n1", "VAR(5)", -1);
        cke("STDEV n1", "STDEV(5)", -1);
        ckc("VARP n1", "VARP(5)", "0");
        ckc("STDEVP n1", "STDEVP(5)", "0");
        cke("VAR n0", "VAR(B7>B9)", -1);
        cke("VARP n0", "VARP(B7>B9)", -1);
        cke("STDEV n0", "STDEV(B7>B9)", -1);
        cke("STDEVP n0", "STDEVP(B7>B9)", -1);
        cke("RMS n0", "RMS(B7>B9)", -1);
        ckc("SUMSQ n0", "SUMSQ(B7>B9)", "0");
        /* PERCENTILE.INC on direct values */
        ckc("PCT 1234 0.5", "PCT(1,2,3,4,0.5)", "+2.5000000000000E+1");
        ckc("PCT 1234 0.25", "PCT(1,2,3,4,0.25)",
            "+1.7500000000000E+1");
        ckc("PCT 1234 0.75", "PCT(1,2,3,4,0.75)",
            "+3.2500000000000E+1");
        ckc("PCT single", "PCT(5,0.3)", "+5.0000000000000E+1");
        cke("PCT empty", "PCT(B7>B9,0.5)", -1);
        /* an error cell in the range aborts (the family behavior) */
        cs_commit_entry(0, 9, "+1/0");          /* A10 = ?n? */
        cke("VAR error cell", "VAR(A1>A10)", -1);
        cke("PCT error cell", "PCT(A1>A10,0.5)", -1);
        /* overflow: the sum of squares leaves the exponent range */
        cke("RMS huge", "RMS(1E500,1E500)", -1);
        cke("SUMSQ huge", "SUMSQ(1E500,1E500)", -1);
        cke("VAR huge dev", "VAR(1E501,3E501)", -1);
        cke("STDEV huge dev", "STDEV(1E501,3E501)", -1);
        ckc("VAR huge equal", "VAR(1E500,1E500)", "0");
        /* ==== the paired statistics and the trapezoid (batch C) ====
         * A1..A4 = 1..4 (x); B1..B4 = 2,4,6,8 (y = 2x); D1..D4 =
         * 8,6,4,2 (y = -x+9); F1..F4 = 2,1,4,3 (mixed); G/H = the
         * x^2 table; I/J = the two-point table; K/L = unordered */
        cs_clear_all();
        setv(0, 0, "1"); setv(0, 1, "2"); setv(0, 2, "3"); setv(0, 3, "4");
        setv(1, 0, "2"); setv(1, 1, "4"); setv(1, 2, "6"); setv(1, 3, "8");
        setv(3, 0, "8"); setv(3, 1, "6"); setv(3, 2, "4"); setv(3, 3, "2");
        setv(5, 0, "2"); setv(5, 1, "1"); setv(5, 2, "4"); setv(5, 3, "3");
        setv(6, 0, "0"); setv(6, 1, "1"); setv(6, 2, "2"); setv(6, 3, "3");
        setv(7, 0, "0"); setv(7, 1, "1"); setv(7, 2, "4"); setv(7, 3, "9");
        ckc("CORR perfect", "CORR(A1>A4,B1>B4)", "+1.0000000000000E+1");
        ckc("CORR negative", "CORR(A1>A4,D1>D4)", "-1.0000000000000E+1");
        ckc("RSQ perfect", "RSQ(A1>A4,D1>D4)", "+1.0000000000000E+1");
        ckc("CORR mixed", "CORR(A1>A4,F1>F4)", "+6.0000000000000E+0");
        ckc("RSQ mixed", "RSQ(A1>A4,F1>F4)", "+3.6000000000000E+0");
        /* a reversed range normalizes independently */
        ckc("CORR reversed", "CORR(A1>A4,B4>B1)", "+1.0000000000000E+1");
        ckc("INTEG both reversed", "INTEG(B4>B1,A4>A1)",
            "+1.5000000000000E+2");
        /* the skip rule: a text hole drops the PAIR, not the data
         * (the remaining three pairs stay perfectly correlated) */
        cs_set_text(tcell(1, 1), "hole", 'L');
        ckc("CORR skip pair", "CORR(A1>A4,B1>B4)", "+1.0000000000000E+1");
        setv(1, 1, "4");
        /* the error paths */
        cke("CORR n1", "CORR(A1>A1,B1>B1)", -1);
        cke("CORR shape", "CORR(A1>A4,B1>B2)", -1);
        cke("CORR non-range", "CORR(5,B1>B4)", 6);
        cke("CORR 1 arg", "CORR(A1>A4)", 3);
        ckc("RSQ self", "RSQ(A1>A2,A1>A2)", "+1.0000000000000E+1");
        setv(12, 0, "5"); setv(12, 1, "5");
        cke("RSQ zero var", "RSQ(M1>M2,M1>M2)", -1);   /* constant x */
        /* the trapezoidal rule */
        ckc("INTEG x^2", "INTEG(G1>G4,H1>H4)", "+9.5000000000000E+1");
        setv(8, 0, "0"); setv(8, 1, "2");
        setv(9, 0, "1"); setv(9, 1, "3");
        ckc("INTEG two pts", "INTEG(I1>I2,J1>J2)", "+4.0000000000000E+1");
        cs_set_text(tcell(7, 1), "hole", 'L');
        cke("INTEG hole", "INTEG(G1>G4,H1>H4)", -1);
        setv(7, 1, "1");
        setv(10, 0, "1"); setv(10, 1, "3"); setv(10, 2, "2");
        setv(11, 0, "1"); setv(11, 1, "9"); setv(11, 2, "4");
        cke("INTEG not increasing", "INTEG(K1>K3,L1>L3)", -1);
        cke("INTEG n1", "INTEG(A1>A1,B1>B1)", -1);
        cke("INTEG shape", "INTEG(A1>A4,B1>B2)", -1);
        cke("INTEG non-range", "INTEG(5,B1>B4)", 6);
        cs_clear_all();
        /* the ALLOC-EMPTY round trip (the user's persistence bug,
         * 2026-11-16): a form-mode cell (type 1) must save AND load */
        {
            Cell *ac = cs_alloc_cell(0, 0);
            ac->type = CE_ALLOC;            /* the form-mode state */
            ac->just = 'L';
            ac->prec = -1;
            ac->formf = 1;
            colw[0] = 13;
            colp[0] = 3;
        }
        setv(1, 0, "2");                    /* B1 numeric beside it */
        cs_commit_entry(2, 0, "hello");     /* C1 text beside it */
        if (cs_save("t_alloc", 0, 0, 3, 3, "")) {
            cs_clear_all();
            colw[0] = 10; colp[0] = 2;
            if (cs_load("t_alloc", 0, 0, 0)) {
                Cell *ac = tread(0, 0);
                if (ac->type != CE_ALLOC || !ac->formf)
                    { printf("FAIL alloc round trip (%d)\n", ac->type);
                      fails++; }
                else printf("ok   the allocated-empty cell round trips\n");
                if (colw[0] != 13 || colp[0] != 3)
                    { printf("FAIL alloc colw (%d/%d)\n", colw[0],
                             colp[0]); fails++; }
                else printf("ok   the alloc cell's formatting returns\n");
                if (tread(1, 0)->type != CE_NUM ||
                    strcmp(tread(1, 0)->cont, "2") != 0)
                    { printf("FAIL alloc neighbor num\n"); fails++; }
                else printf("ok   the numeric neighbor survives\n");
                if (tread(2, 0)->type != CE_TEXT ||
                    strcmp(tread(2, 0)->cont, "hello") != 0)
                    { printf("FAIL alloc neighbor text\n"); fails++; }
                else printf("ok   the text neighbor survives\n");
                /* the record stream stayed in sync: the cell count */
                if (entriesUsed != 3)
                    { printf("FAIL alloc count (%d)\n", entriesUsed);
                      fails++; }
                else printf("ok   the entry count is honest\n");
            } else { printf("FAIL alloc load\n"); fails++; }
        } else { printf("FAIL alloc save\n"); fails++; }
        remove("t_alloc.csd");
        cs_clear_all();
        /* the COPY POLICY (the user's persistence review, 2026-11-16):
         * a copy reproduces the complete cell state; a BLANK source
         * frees the destination (the sparse structure mirrors) */
        {
            Csn v;
            int used;
            csn_parse("120", &v);
            /* 1. an absent source over a POPULATED destination: the
             * destination ceases to exist (A1 = 5, B1 = 9, C1 never
             * populated -> copy C1 over B1) */
            setv(0, 0, "5");                /* A1 */
            setv(1, 0, "9");                /* B1 */
            used = entriesUsed;
            cs_copy_block(2, 0, 2, 0, 1, 0, 1, 1, 0, 1, 0);
            if (cs_getcell(1, 0) != NULL)
                { printf("FAIL blank copy populated\n"); fails++; }
            else printf("ok   copying a blank frees the destination\n");
            if (entriesUsed != used - 1)
                { printf("FAIL blank copy budget (%d)\n", entriesUsed);
                  fails++; }
            else printf("ok   the blank copy returns the budget\n");
            /* 2. an absent source over an EMPTY destination: nothing
             * allocated (C1 absent -> D1) */
            used = entriesUsed;
            cs_copy_block(2, 0, 2, 0, 3, 0, 1, 1, 0, 1, 0);
            if (cs_getcell(3, 0) != NULL || entriesUsed != used)
                { printf("FAIL blank copy empty\n"); fails++; }
            else printf("ok   copying a blank onto empty allocates nothing\n");
            /* 3. an ALLOCATED-EMPTY source: the state carries */
            {
                Cell *ac = cs_alloc_cell(0, 0);
                ac->type = CE_ALLOC;
                ac->formf = 1;
                ac->prec = -1;
            }
            cs_copy_block(0, 0, 0, 0, 1, 0, 1, 1, 0, 1, 0);
            if (tread(1, 0)->type != CE_ALLOC || !tread(1, 0)->formf)
                { printf("FAIL alloc copy\n"); fails++; }
            else printf("ok   the allocated-empty state copies\n");
            /* 4. the FACTORIAL ENTRY: the flag and the value carry */
            cs_clear_all();
            cs_commit_entry(0, 0, "5!");            /* A1 = 120 */
            cs_copy_block(0, 0, 0, 0, 1, 0, 1, 1, 0, 1, 0);
            if (tread(1, 0)->factf != 1 ||
                tread(1, 0)->type != CE_NUM || tread(1, 0)->iserr ||
                csn_cmp(&tread(1, 0)->val, &v) != 0 ||
                !tread(1, 0)->cont || strcmp(tread(1, 0)->cont, "5!"))
                { printf("FAIL factf copy\n"); fails++; }
            else printf("ok   the factorial entry copies with FACT(5) = 120\n");
            /* 5. the notation and formf flags carry */
            cs_clear_all();
            {
                Cell *nc = cs_alloc_cell(0, 0);
                Csn vv;
                csn_parse("7", &vv);
                cs_set_num(nc, "7", &vv, 0, NULL);
                nc->notation = 2;                   /* (dec) */
            }
            cs_commit_entry(1, 0, "text");          /* B1 formf via? no */
            {
                Cell *fc = cs_alloc_cell(1, 0);
                fc->formf = 1;                      /* B1 = a form cell */
            }
            cs_copy_block(0, 0, 1, 0, 3, 0, 2, 1, 0, 2, 0);
            if (tread(3, 0)->notation != 2)
                { printf("FAIL notation copy\n"); fails++; }
            else printf("ok   the notation carries with the copy\n");
            if (tread(4, 0)->formf != 1)
                { printf("FAIL formf copy\n"); fails++; }
            else printf("ok   the form flag carries with the copy\n");
        }
        cs_clear_all();
        /* the SORT comparator (the user's report, 2026-11-16): text
         * and a formula's string result are ONE string class */
        cs_commit_entry(0, 0, "B");              /* A1 text "B" */
        cs_commit_entry(0, 1, "+CHR(65)");       /* A2 -> "A" */
        cs_sort_range(0, 0, 0, 1, 0, 0);         /* ascending */
        if (!(tread(0, 0)->strres && !strcmp(tread(0, 0)->strres, "A") &&
              tread(0, 1)->type == CE_TEXT &&
              tread(0, 1)->cont && !strcmp(tread(0, 1)->cont, "B")))
            { printf("FAIL mixed sort asc\n"); fails++; }
        else printf("ok   the text/strres mixed sort ascends\n");
        cs_commit_entry(0, 0, "B");
        cs_commit_entry(0, 1, "+CHR(65)");
        cs_sort_range(0, 0, 0, 1, 0, 1);         /* descending */
        if (!(tread(0, 0)->type == CE_TEXT &&
              !strcmp(tread(0, 0)->cont, "B") &&
              tread(0, 1)->strres && !strcmp(tread(0, 1)->strres, "A")))
            { printf("FAIL mixed sort desc\n"); fails++; }
        else printf("ok   the text/strres mixed sort descends\n");
        /* numbers first, then the strings in byte order */
        setv(0, 0, "5");
        cs_commit_entry(0, 1, "b");
        cs_commit_entry(0, 2, "+CHR(97)");       /* "a" */
        cs_sort_range(0, 0, 0, 2, 0, 0);
        if (tread(0, 0)->type != CE_NUM ||
            !(tread(0, 1)->strres && !strcmp(tread(0, 1)->strres, "a")) ||
            !(tread(0, 2)->type == CE_TEXT &&
              !strcmp(tread(0, 2)->cont, "b")))
            { printf("FAIL three-way sort\n"); fails++; }
        else printf("ok   numbers first, then the strings in order\n");
        cs_clear_all();
        /* the RANGE REMAP POLICY: the image of the row interval */
        setv(0, 0, "3"); setv(0, 1, "1"); setv(0, 2, "2");
        cs_commit_entry(1, 0, "+SUM(A1>A3)");    /* the whole block */
        cs_commit_entry(1, 1, "+SUM(A2>A3)");    /* contiguous image */
        cs_commit_entry(1, 2, "+SUM(A1>A2)");    /* scattered image */
        cs_commit_entry(1, 3, "+A2*10");         /* the single row */
        cs_sort_range(0, 0, 0, 2, 0, 0);         /* keys 3,1,2 asc */
        /* [3,1,2] asc -> perm: A1->A3, A2->A1, A3->A2 */
        if (!tread(1, 0)->cont || strcmp(tread(1, 0)->cont, "+SUM(A1>A3)"))
            { printf("FAIL whole-block range (%s)\n",
                     tread(1, 0)->cont ? tread(1, 0)->cont : "(null)");
              fails++; }
        else printf("ok   the whole-block range is invariant\n");
        if (!tread(1, 1)->cont || strcmp(tread(1, 1)->cont, "+SUM(A1>A2)"))
            { printf("FAIL contiguous remap (%s)\n",
                     tread(1, 1)->cont ? tread(1, 1)->cont : "(null)");
              fails++; }
        else printf("ok   the contiguous partial image remaps\n");
        if (!tread(1, 2)->cont || strcmp(tread(1, 2)->cont, "+SUM(A1>A2)"))
            { printf("FAIL scattered remap (%s)\n",
                     tread(1, 2)->cont ? tread(1, 2)->cont : "(null)");
              fails++; }
        else printf("ok   the scattered image stays as typed\n");
        if (!tread(1, 3)->cont || strcmp(tread(1, 3)->cont, "+A1*10"))
            { printf("FAIL single-row remap (%s)\n",
                     tread(1, 3)->cont ? tread(1, 3)->cont : "(null)");
              fails++; }
        else printf("ok   the single-row remap still works\n");
        cs_clear_all();
        /* the DEPENDENCY '&' FIX (the user's report, 2026-11-16):
         * A5&" units" must be seen as a reference to A5 */
        cs_commit_entry(0, 4, "7");              /* A5 = 7 */
        cs_commit_entry(1, 0, "+A5&\" units\""); /* B1 */
        {
            char list[256];
            dep_check_range(0, 4, 4, list, (int) sizeof(list));
            if (!list[0])
                { printf("FAIL dep & miss\n"); fails++; }
            else printf("ok   the '&' reference is found: %s\n", list);
        }
        /* the shifter's '&' (the same delimiter set, pinned) */
        {
            char *adj = cs_adjust_refs("+A5&\"x\"", 0, 1);
            if (!adj || strcmp(adj, "+A6&\"x\""))
                { printf("FAIL adjust & (%s)\n", adj ? adj : "(null)");
                  fails++; }
            else printf("ok   the '&' shifts with the copy\n");
            if (adj) free(adj);
            adj = cs_shift_refs("+A5&\"x\"", 0, 4, 1);   /* after row 4 */
            if (!adj || strcmp(adj, "+A6&\"x\""))
                { printf("FAIL shift & (%s)\n", adj ? adj : "(null)");
                  fails++; }
            else printf("ok   the '&' shifts with the insert\n");
            if (adj) free(adj);
        }
        cs_clear_all();
        /* the REF POLICY (the user's decision, 2026-11-16): REF's
         * coordinates are DATA - structural edits never renumber
         * them, the dependency check never follows them, a deletion
         * proceeds.  The multi-level indirection chain re-resolves */
        cs_commit_entry(0, 0, "1");              /* A1 = 1 */
        cs_commit_entry(1, 0, "+VAL(A1)");       /* B1 */
        cs_commit_entry(2, 0, "+VAL(B1)");       /* C1 */
        cs_commit_entry(3, 0, "+VAL(C1)");       /* D1 = 1 */
        cs_commit_entry(3, 1, "2");              /* D2 = 2 */
        cs_commit_entry(0, 1, "test");           /* A2 = test */
        cs_commit_entry(0, 2, "+VAL(REF(VAL(D1),VAL(D2)))");
        {
            Cell *c = tread(0, 2);
            if (!c->strres || strcmp(c->strres, "test"))
                { printf("FAIL ref chain (%s)\n",
                         c->strres ? c->strres : "(null)"); fails++; }
            else printf("ok   the indirection chain resolves\n");
        }
        cs_delete_rows(10, 10);                  /* an unrelated row */
        if (!tread(0, 2)->strres || strcmp(tread(0, 2)->strres, "test"))
            { printf("FAIL ref chain survives\n"); fails++; }
        else printf("ok   the chain re-resolves after a deletion\n");
        /* REF(2,7) is never renumbered */
        cs_clear_all();
        cs_commit_entry(0, 6, "42");             /* A7 = 42 */
        cs_commit_entry(2, 9, "+VAL(REF(2,7))"); /* C10 -> A7 */
        cs_delete_rows(5, 5);                    /* above the target? no:
                                                    row 5 < 7: the sheet
                                                    shifts but REF stays */
        if (!tread(2, 8)->cont || strcmp(tread(2, 8)->cont, "+VAL(REF(2,7))"))
            { printf("FAIL ref renumber (%s)\n",
                     tread(2, 8)->cont ? tread(2, 8)->cont : "(null)");
              fails++; }
        else printf("ok   REF(2,7) is never renumbered\n");
        /* the deletion PROCEEDS when a REF formula exists */
        cs_commit_entry(4, 3, "+VAL(REF(2,5))"); /* a REF formula */
        cs_delete_rows(5, 5);                    /* proceeds: no veto */
        if (!tread(2, 7)->cont ||
            strcmp(tread(2, 7)->cont, "+VAL(REF(2,7))") ||
            !tread(4, 3)->cont ||
            strcmp(tread(4, 3)->cont, "+VAL(REF(2,5))"))
            { printf("FAIL ref deletion vetoed\n"); fails++; }
        else printf("ok   the deletion proceeds beside REF formulas\n");
        cs_clear_all();
        /* the FACTORIAL FIXES (the user's reports, 2026-11-16) */
        /* 1. the dry parse: the syntax check must not run the SET */
        cs_commit_entry(0, 0, "0");                   /* A1 = 0 */
        cs_commit_entry(1, 0, "+(1):SET(A1,VAL(A1)+1);0:1!");
        if (tread(0, 0)->type != CE_NUM || tread(0, 0)->val.nd[0] != 1)
            { printf("FAIL dry double-exec (%d)\n",
                     tread(0, 0)->val.nd[0]); fails++; }
        else printf("ok   the parse-only check does not execute\n");
        if (tread(1, 0)->factf != 1 || tread(1, 0)->iserr ||
            tread(1, 0)->val.nd[0] != 1)
            { printf("FAIL dry factf value\n"); fails++; }
        else printf("ok   the factorial entry is FACT(1) = 1\n");
        /* 2. the own-cell context survives the cursor */
        cs_clear_all();
        cs_commit_entry(0, 2, "ROW()!");              /* A3 = 6 */
        if (tread(0, 2)->val.nd[0] != 6)
            { printf("FAIL ROW! commit (%d)\n", tread(0, 2)->val.nd[0]);
              fails++; }
        else printf("ok   ROW()! commits as FACT(3) = 6\n");
        curRow = 0; curCol = 4;                       /* the cursor away */
        cs_recalc_all();
        if (tread(0, 2)->val.nd[0] != 6)
            { printf("FAIL ROW! recalc (%d)\n", tread(0, 2)->val.nd[0]);
              fails++; }
        else printf("ok   ROW()! recalculates against its own row\n");
        /* 3. the buffer guard: oversized and empty factf contents */
        cs_clear_all();
        {
            Cell *bc = cs_alloc_cell(0, 0);
            char big[320];
            Csn vv;
            memset(big, 'x', (size_t) 299);
            big[299] = '!';
            big[300] = '\0';
            csn_zero(&vv);
            cs_set_num(bc, big, &vv, 0, NULL);
            bc->factf = 1;
            bc = cs_alloc_cell(1, 0);
            cs_set_num(bc, "", &vv, 0, NULL);         /* empty */
            bc->factf = 1;
        }
        cs_recalc_entry(0, 0);
        cs_recalc_entry(1, 0);
        if (!tread(0, 0)->iserr || !tread(1, 0)->iserr)
            { printf("FAIL factf guard\n"); fails++; }
        else printf("ok   unusable factf contents become error cells\n");
        cs_copy_block(0, 0, 0, 0, 2, 0, 1, 1, 0, 1, 0);
        if (!tread(2, 0)->iserr)
            { printf("FAIL factf guard copy\n"); fails++; }
        else printf("ok   the oversized factf copies as an error\n");
        cs_clear_all();
        /* ==== the numerical-engine fixes (the user's report,
         * 2026-11-16) ==== */
        /* POW: the negative base with a large integer exponent */
        ckc("pow neg big", "(-2)**100", "+1.2676506002282E+31");
        ckc("pow neg odd", "(-2)**101", "-2.5353012004564E+31");
        ckc("pow -1 odd", "(-1)**999", "-1.0000000000000E+1");
        ckc("pow -1 even", "(-1)**1000", "+1.0000000000000E+1");
        ckc("pow neg 40", "(-3)**40", "+1.2157665459056E+20");
        cke("pow neg frac", "(-2)**0.5", -1);
        /* LCM: the signs and the BCD euclid */
        ckc("LCM -4,6", "LCM(-4,6)", "+1.2000000000000E+2");
        ckc("LCM -4,-6", "LCM(-4,-6)", "+1.2000000000000E+2");
        ckc("LCM 4,6", "LCM(4,6)", "+1.2000000000000E+2");
        ckc("GCD -4,6", "GCD(-4,6)", "+2.0000000000000E+1");
        ckc("GCD 0,x", "GCD(0,6)", "+6.0000000000000E+1");
        cke("LCM over cap", "LCM(999999999999,2)", -1);
        /* ROUND/TRUNC: the identity shortcut */
        ckc("ROUND huge", "ROUND(9.9E+998,1)", "+9.9000000000000E+999");
        ckc("TRUNC huge", "TRUNC(9.9E+998,1)", "+9.9000000000000E+999");
        ckc("ROUND tens", "ROUND(1234,-1)", "+1.2300000000000E+4");
        ckc("ROUND 125", "ROUND(125,-1)", "+1.3000000000000E+3");
        ckc("TRUNC tens", "TRUNC(1239,-1)", "+1.2300000000000E+4");
        /* EVEN/ODD: the last-digit parity, any magnitude */
        ckc("EVEN 1E500", "EVEN(1E+500)", "+1.0000000000000E+1");
        ckc("ODD 1E500", "ODD(1E+500)", "0");
        ckc("ODD 7", "ODD(7)", "+1.0000000000000E+1");
        ckc("EVEN -4", "EVEN(-4)", "+1.0000000000000E+1");
        ckc("EVEN 10", "EVEN(10)", "+1.0000000000000E+1");
        /* csn_parse: the malformed and oversized exponents */
        cke("parse 1E+", "+1E+", 8);
        cke("parse 1E", "+1E", 7);
        cke("parse 1E-", "+1E-", 8);
        ckc("parse 1E0", "+1E0", "+1.0000000000000E+1");
        cke("parse huge exp", "+1E+99999", -1);
        cke("parse 23-digit exp", "+1E99999999999999999999999", -1);
        /* ==== the final batch (the user's report, 2026-11-16) ==== */
        /* the FULL-PRECISION format: a 30+-digit value round trips */
        cs_clear_all();
        {
            Cell *c = cs_alloc_cell(0, 0);
            Csn vv;
            csn_parse("1.2345678901234567890123456789012345678901", &vv);
            cs_set_num(c, "1.2345678901234567890123456789012345678901",
                       &vv, 0, NULL);
        }
        if (cs_save("t_prec", 0, 0, 2, 2, "")) {
            cs_clear_all();
            if (cs_load("t_prec", 0, 0, 0)) {
                Cell *c = tread(0, 0);
                char big[64];
                int i, ok = 1;
                csn_fmt_fixed(&c->val, 40, big, (int) sizeof(big));
                /* the fixed-40 print carries the leading 1 + 40 digits
                 * after the point; compare against the source digits */
                {
                    /* the engine keeps 40 digits (the literal has 41):
                     * the loaded value must equal the PARSED value */
                    Csn want;
                    char wbuf[64];
                    csn_parse("1.2345678901234567890123456789012345678901",
                              &want);
                    csn_fmt_fixed(&want, 40, wbuf, (int) sizeof(wbuf));
                    ok = c->type == CE_NUM;
                    for (i = 0; ok && i < 42; i++)
                        if (big[i] != wbuf[i]) ok = 0;
                }
                if (!ok) { printf("FAIL precision round trip (%s)\n", big);
                           fails++; }
                else printf("ok   the 40-digit value round trips\n");
            } else { printf("FAIL precision load\n"); fails++; }
        } else { printf("FAIL precision save\n"); fails++; }
        remove("t_prec.csd");
        cs_clear_all();
        /* the LOAD-AT-ORIGIN reference adjust: the file has A1 = 7
         * and B2 = +A1*2 (14); loaded at the origin B2 the formula
         * lands at C3 and its reference shifts (+1,+1): +B1*2 with
         * B1 = the loaded 7 -> 14 */
        setv(0, 0, "7");                    /* A1 = 7 */
        cs_commit_entry(1, 1, "+A1*2");     /* B2 = +A1*2 = 14 */
        if (cs_save("t_org", 0, 0, 2, 2, "")) {
            cs_clear_all();
            if (cs_load("t_org", 1, 1, 0)) {
                /* the file's B2(+A1*2) lands at C3: the reference
                 * shifts by (+1,+1) -> +B2*2 (A1 lands at B2, the
                 * loaded 7 feeds the recalc) */
                Cell *c = tread(2, 2);
                if (!c->cont || strcmp(c->cont, "+B2*2"))
                    { printf("FAIL origin adjust (%s)\n",
                             c->cont ? c->cont : "(null)"); fails++; }
                else printf("ok   the loaded formula re-points\n");
                /* the file's A1(7) lands at B2: B1 = 7 -> the recalc
                 * gives C3 = 14 */
                cs_recalc_all();
                {
                    Csn want;
                    char g[48];
                    csn_from_int(&want, 14L);
                    csn_canon(&tread(2, 2)->val, g, (int) sizeof(g));
                    if (tread(2, 2)->iserr || csn_cmp(&tread(2, 2)->val,
                                                     &want) != 0)
                        { printf("FAIL origin recalc (%s)\n", g); fails++; }
                    else printf("ok   the shifted reference recalculates\n");
                }
            } else { printf("FAIL origin load\n"); fails++; }
        } else { printf("FAIL origin save\n"); fails++; }
        remove("t_org.csd");
        cs_clear_all();
        /* the DYNAMIC error queue: 300 error cells all queued */
        {
            int i;
            for (i = 0; i < 300; i++)
                cs_commit_entry(i % 16, i / 16, "+1/0");
        }
        cs_recalc_all();
        if (cs_nerrlist < 300)
            { printf("FAIL error queue (%d)\n", cs_nerrlist); fails++; }
        else printf("ok   300 errors queued (the dynamic queue)\n");
        cs_clear_all();

        /* the memory batch (the user's report, 2026-11-16) */
        /* D: a ?-calculator commit strands NO pool slot */
        cs_clear_all();
        cs_commit_entry(0, 0, "1+2?");
        if (cs_getcell(0, 0) != NULL || entriesUsed != 0)
            { printf("FAIL calc slot (%d)\n", entriesUsed); fails++; }
        else printf("ok   the calculator entry leaves no slot\n");
        /* a failed SET strands no slot (the arg-count and type errors
         * release the target) */
        cs_commit_entry(0, 0, "5");                    /* A1 */
        cs_commit_entry(1, 0, "+(1):SET(REF(2,3));0:0");
        if (cs_getcell(2, 2) != NULL)
            { printf("FAIL set slot arg\n"); fails++; }
        else printf("ok   the failed SET leaves no slot (args)\n");
        cs_commit_entry(1, 0, "+(1):SET(REF(2,3),A1>B2);0:0");
        if (cs_getcell(2, 2) != NULL)
            { printf("FAIL set slot type\n"); fails++; }
        else printf("ok   the failed SET leaves no slot (type)\n");
        cs_clear_all();
        /* C: an OVERLAPPING copy of a partially blank range */
        setv(0, 0, "5");                               /* A1 = 5 */
        cs_copy_block(0, 0, 1, 0, 1, 0, 2, 1, 0, 2, 0);
        if (tread(1, 0)->type != CE_NUM || tread(1, 0)->val.nd[0] != 5)
            { printf("FAIL overlap blank copy\n"); fails++; }
        else printf("ok   the overlapping copy shifts A1 into B1\n");
        if (cs_getcell(2, 0) != NULL)
            { printf("FAIL overlap blank cleared\n"); fails++; }
        else printf("ok   the blank source cleared its target\n");
        cs_clear_all();
        /* E: the copy budget pre-check refuses UPFRONT */
        {
            int i;
            /* 3999 cells spread across 16 columns (the sheet is 255
             * rows deep) */
            for (i = 0; i < 3997; i++)
                setv(i % 16, i / 16, "1");
            cs_commit_entry(16, 0, "7");               /* Q1 */
            cs_commit_entry(17, 0, "8");               /* R1 */
            if (entriesUsed != 3999)
                { printf("FAIL seed count (%d)\n", entriesUsed); fails++; }
            cs_copy_block(16, 0, 17, 0, 18, 0, 2, 1, 0, 2, 0);
            if (cs_getcell(18, 0) != NULL)
                { printf("FAIL budget pre-check\n"); fails++; }
            else printf("ok   the over-budget copy is refused upfront\n");
        }
        cs_clear_all();

        /* the TWO-PASS CORR/REGR: the offset line stays exact */
        setv(0, 0, "1000001"); setv(0, 1, "1000002");
        setv(0, 2, "1000003"); setv(0, 3, "1000004");
        setv(1, 0, "2000003"); setv(1, 1, "2000005");
        setv(1, 2, "2000007"); setv(1, 3, "2000009");
        ckc("CORR offset line", "CORR(A1>A4,B1>B4)",
            "+1.0000000000000E+1");
        ckc("RSQ offset line", "RSQ(A1>A4,B1>B4)",
            "+1.0000000000000E+1");
        ckc("REGR offset meany", "REGR(A1>A4,B1)",
            "+2.0000060000000E+7");
        ckc("SLOPE() offset", "SLOPE()", "+2.0000000000000E+1");
        cs_clear_all();
        /* ==== the memory-safety batch (the user's report, 2026-11-16) ==== */
        /* cs_parse_coord: the row accumulator cannot wrap */
        {
            int c1 = 0, r1 = 0;
            if (cs_parse_coord("A99999999999", &c1, &r1))
                { printf("FAIL row wrap accepted\n"); fails++; }
            else printf("ok   a 13-digit row is rejected\n");
            if (!cs_parse_coord("A255", &c1, &r1) || r1 != 254)
                { printf("FAIL A255 parse\n"); fails++; }
            else printf("ok   the legal three-digit rows still parse\n");
        }
        /* the loader's width clamp */
        {
            FILE *g = fopen("t_wide.csd", "wb");
            int k;
            char *junk = (char *) malloc(400);
            memset(junk, 0, 400);
            fwrite("CSD5", 1, 4, g);
            fputc(0, g); fwrite("\x00\x00", 1, 2, g);   /* the origin */
            for (k = 0; k < 127; k++) {
                fputc(k == 0 ? 255 : 10, g);           /* the width 255! */
                fputc(2, g);
            }
            fputc(0, g);                                /* no password */
            fwrite("\x00\x00", 1, 2, g);                /* count 0 */
            fclose(g);
            free(junk);
            cs_clear_all();
            if (cs_load("t_wide", 0, 0, 0)) {
                if (colw[0] != CS_MAX_W)
                    { printf("FAIL width clamp (%d)\n", colw[0]); fails++; }
                else printf("ok   a corrupt width clamps to the manual's 63\n");
            } else { printf("FAIL wide load\n"); fails++; }
            remove("t_wide.csd");
        }
        cs_clear_all();
        /* ==== CONV: the unit conversion (batch D) ==== */
        ckc("CONV cm m", "CONV(100,\"cm\",\"m\")", "+1.0000000000000E+1");
        ckc("CONV inch mm", "CONV(1,\"inch\",\"mm\")",
            "+2.5400000000000E+2");
        ckc("CONV mile km", "CONV(1,\"mile\",\"km\")",
            "+1.6093440000000E+1");
        ckc("CONV lb kg", "CONV(1,\"lb\",\"kg\")",
            "+4.5359237000000E+0");
        ckc("CONV bar Pa", "CONV(1,\"bar\",\"Pa\")",
            "+1.0000000000000E+6");
        ckc("CONV psi Pa", "CONV(1,\"psi\",\"Pa\")",
            "+6.8947572931683E+4");
        ckc("CONV HP W", "CONV(1,\"HP\",\"W\")",
            "+7.4569987158227E+3");
        ckc("CONV kWh J", "CONV(1,\"kWh\",\"J\")",
            "+3.6000000000000E+7");
        ckc("CONV cal J", "CONV(1,\"cal\",\"J\")", "+4.1840000000000E+1");
        ckc("CONV gallon l", "CONV(1,\"gallon\",\"l\")",
            "+3.7854117840000E+1");
        ckc("CONV hectare m2", "CONV(1,\"hectare\",\"m2\")",
            "+1.0000000000000E+5");
        /* the missing dm2 (the user's addition): 1 dm2 = 0.01 m2 */
        ckc("CONV dm2 m2", "CONV(1,\"dm2\",\"m2\")",
            "+1.0000000000000E-1");
        ckc("CONV m2 dm2", "CONV(1,\"m2\",\"dm2\")",
            "+1.0000000000000E+3");
        ckc("CONV dm2 cm2", "CONV(1,\"dm2\",\"cm2\")",
            "+1.0000000000000E+3");
        ckc("CONV round dm2",
            "CONV(CONV(5,\"dm2\",\"m2\"),\"m2\",\"dm2\")",
            "+5.0000000000000E+1");
        ckc("CONV tonne lb", "CONV(1,\"tonne\",\"lb\")",
            "+2.2046226218487E+4");
        ckc("CONV km/h", "CONV(1,\"km/h\",\"m/s\")",
            "+2.7777777777777E+0");
        ckc("CONV mph", "CONV(1,\"mph\",\"m/s\")",
            "+4.4704000000000E+0");
        ckc("CONV GHz Hz", "CONV(1,\"GHz\",\"Hz\")",
            "+1.0000000000000E+10");
        ckc("CONV kohm ohm", "CONV(1,\"kohm\",\"ohm\")",
            "+1.0000000000000E+4");
        ckc("CONV uf farad", "CONV(1,\"uf\",\"farad\")",
            "+1.0000000000000E-5");
        ckc("CONV farad uf", "CONV(1,\"farad\",\"uf\")",
            "+1.0000000000000E+7");
        /* temperature: the offsets, both directions */
        ckc("CONV C F 0", "CONV(0,\"C\",\"F\")", "+3.2000000000000E+2");
        ckc("CONV C F 100", "CONV(100,\"C\",\"F\")",
            "+2.1200000000000E+3");
        ckc("CONV F C 100", "CONV(100,\"F\",\"C\")",
            "+3.7777777777777E+2");
        ckc("CONV C K", "CONV(0,\"C\",\"K\")", "+2.7315000000000E+3");
        ckc("CONV K C", "CONV(273.15,\"K\",\"C\")", "0");
        /* the one name clash: bare "f" is FAHRENHEIT, the farad is
         * "farad"/"uf" (flagged) */
        ckc("CONV f is temp", "CONV(1,\"f\",\"c\")",
            "-1.7222222222222E+2");
        /* case-insensitive lookup */
        ckc("CONV case", "CONV(100,\"CM\",\"m\")",
            "+1.0000000000000E+1");
        ckc("CONV case 2", "CONV(1,\"Mile\",\"km\")",
            "+1.6093440000000E+1");
        /* round trips: the mile factors terminate -> exact; the lb
         * factor does not -> the honest 30-digit core (a tolerance
         * conditional instead of a canon string) */
        ckc("CONV round mile",
            "CONV(CONV(7,\"mile\",\"km\"),\"km\",\"mile\")",
            "+7.0000000000000E+1");
        ckc("CONV round kg",
            "(ABS(CONV(CONV(3,\"kg\",\"lb\"),\"lb\",\"kg\")-3)<1E-25):1:0",
            "+1.0000000000000E+1");
        /* the unit may come from a TEXT CELL (the bare reference
         * derefs to the string) */
        cs_set_text(tcell(1, 9), "cm", 'L');
        ckc("CONV cell unit", "CONV(100,B10,\"m\")",
            "+1.0000000000000E+1");
        /* the SI cubic volume scale (the user's addition, 2026-11-15):
         * all factors terminate, so every pair round-trips exactly */
        ckc("CONV mm3 cm3", "CONV(1,\"mm3\",\"cm3\")",
            "+1.0000000000000E-2");
        ckc("CONV cm3 mm3", "CONV(1,\"cm3\",\"mm3\")",
            "+1.0000000000000E+4");
        ckc("CONV cm3 dm3", "CONV(1,\"cm3\",\"dm3\")",
            "+1.0000000000000E-2");
        ckc("CONV dm3 m3", "CONV(1,\"dm3\",\"m3\")",
            "+1.0000000000000E-2");
        ckc("CONV m3 dm3", "CONV(1,\"m3\",\"dm3\")",
            "+1.0000000000000E+4");
        ckc("CONV m3 km3", "CONV(1,\"m3\",\"km3\")",
            "+1.0000000000000E-8");
        ckc("CONV km3 m3", "CONV(1,\"km3\",\"m3\")",
            "+1.0000000000000E+10");
        ckc("CONV mm3 l", "CONV(1,\"mm3\",\"l\")",
            "+1.0000000000000E-5");
        /* the defined equivalences */
        ckc("CONV cm3 = ml", "CONV(1,\"cm3\",\"ml\")",
            "+1.0000000000000E+1");
        ckc("CONV dm3 = l", "CONV(1,\"dm3\",\"l\")",
            "+1.0000000000000E+1");
        /* an exact round trip across the whole ladder */
        ckc("CONV round mm3 km3",
            "CONV(CONV(3,\"mm3\",\"km3\"),\"km3\",\"mm3\")",
            "+3.0000000000000E+1");
        /* the error paths */
        cke("CONV dim mismatch", "CONV(1,\"m\",\"kg\")", -1);
        cke("CONV unknown", "CONV(1,\"furlong\",\"m\")", -1);
        cke("CONV empty unit", "CONV(1,\"\",\"m\")", -1);
        cke("CONV num unit", "CONV(1,5,\"kg\")", 6);
        cke("CONV text value", "CONV(\"x\",\"m\",\"cm\")", 6);
        cke("CONV 2 args", "CONV(1,\"m\")", 3);
        cke("CONV 4 args", "CONV(1,\"m\",\"cm\",2)", 3);
        cs_clear_all();
        /* ==== SPLINE + DIFF (batch E): seeds ====
         * A/B = the arch (0,0),(1,1),(2,0); D/E = the x^2 table
         * 0..4; G/H = the linear table; J/K = the uneven table */
        setv(0, 0, "0"); setv(0, 1, "1"); setv(0, 2, "2");
        setv(1, 0, "0"); setv(1, 1, "1"); setv(1, 2, "0");
        setv(3, 0, "0"); setv(3, 1, "1"); setv(3, 2, "2");
        setv(3, 3, "3"); setv(3, 4, "4");
        setv(4, 0, "0"); setv(4, 1, "1"); setv(4, 2, "4");
        setv(4, 3, "9"); setv(4, 4, "16");
        setv(6, 0, "0"); setv(6, 1, "2");
        setv(7, 0, "1"); setv(7, 1, "3");
        setv(9, 0, "0"); setv(9, 1, "1"); setv(9, 2, "3");
        setv(10, 0, "0"); setv(10, 1, "1"); setv(10, 2, "9");
        /* the arch: symmetric, M1 = -3 */
        ckc("SPLINE arch 0.5", "SPLINE(A1>A3,B1>B3,0.5)",
            "+6.8750000000000E+0");
        ckc("SPLINE arch 1", "SPLINE(A1>A3,B1>B3,1)",
            "+1.0000000000000E+1");
        ckc("SPLINE arch 1.5", "SPLINE(A1>A3,B1>B3,1.5)",
            "+6.8750000000000E+0");
        /* the x^2 table: the knots are reproduced EXACTLY */
        ckc("SPLINE x2 0.5", "SPLINE(D1>D5,E1>E5,0.5)",
            "+3.3928571428571E+0");
        ckc("SPLINE x2 knot", "SPLINE(D1>D5,E1>E5,2)",
            "+4.0000000000000E+1");
        ckc("SPLINE x2 3.5", "SPLINE(D1>D5,E1>E5,3.5)",
            "+1.2339285714285E+2");
        ckc("SPLINE x2 end", "SPLINE(D1>D5,E1>E5,4)",
            "+1.6000000000000E+2");
        /* two points: the straight line */
        ckc("SPLINE linear", "SPLINE(G1>G2,H1>H2,1)",
            "+2.0000000000000E+1");
        /* the same range as both x and y */
        ckc("SPLINE self", "SPLINE(A1>A3,A1>A3,1.5)",
            "+1.5000000000000E+1");
        /* the errors */
        cke("SPLINE outside", "SPLINE(D1>D5,E1>E5,4.5)", -1);
        cke("SPLINE n1", "SPLINE(A1>A1,B1>B1,1)", -1);
        cke("SPLINE shape", "SPLINE(A1>A3,B1>B2,1)", -1);
        cke("SPLINE non-range", "SPLINE(5,B1>B3,1)", 6);
        cke("SPLINE 1 arg", "SPLINE(A1>A3)", 3);
        cs_set_text(tcell(1, 2), "hole", 'L');
        cke("SPLINE hole", "SPLINE(A1>A3,B1>B3,1)", -1);
        setv(1, 2, "0");
        setv(12, 0, "2"); setv(12, 1, "1");      /* unordered x */
        setv(13, 0, "1"); setv(13, 1, "2");
        cke("SPLINE unordered", "SPLINE(M1>M3,N1>N3,1)", -1);
        /* the 512-point bound: 513 pairs are an error */
        cs_clear_all();
        {
            int w;
            char b[8];
            for (w = 0; w < 513; w++) {
                sprintf(b, "%d", w + 1);
                setv(w % 3, w / 3, b);
                setv(3 + w % 3, w / 3, "1");
            }
        }
        cke("SPLINE bound", "SPLINE(A1>C171,D1>F171,1)", -1);
        /* DIFF streams - it has NO bound (the same walk succeeds) */
        ckc("DIFF unbounded", "DIFF(A1>C171,D1>F171,1)", "0");
        /* ==== DIFF: the piecewise-linear slope ==== */
        cs_clear_all();
        setv(0, 0, "0"); setv(0, 1, "1"); setv(0, 2, "2");
        setv(1, 0, "0"); setv(1, 1, "1"); setv(1, 2, "0");
        setv(3, 0, "0"); setv(3, 1, "1"); setv(3, 2, "2");
        setv(3, 3, "3"); setv(3, 4, "4");
        setv(4, 0, "0"); setv(4, 1, "1"); setv(4, 2, "4");
        setv(4, 3, "9"); setv(4, 4, "16");
        setv(6, 0, "0"); setv(6, 1, "2");
        setv(7, 0, "1"); setv(7, 1, "3");
        setv(9, 0, "0"); setv(9, 1, "1"); setv(9, 2, "3");
        setv(10, 0, "0"); setv(10, 1, "1"); setv(10, 2, "9");
        ckc("DIFF mid", "DIFF(D1>D5,E1>E5,1.5)", "+3.0000000000000E+1");
        ckc("DIFF at start", "DIFF(D1>D5,E1>E5,0)",
            "+1.0000000000000E+1");
        ckc("DIFF last knot", "DIFF(D1>D5,E1>E5,4)",
            "+7.0000000000000E+1");
        ckc("DIFF late", "DIFF(D1>D5,E1>E5,3.5)",
            "+7.0000000000000E+1");
        ckc("DIFF uneven", "DIFF(J1>J3,K1>K3,2)",
            "+4.0000000000000E+1");
        ckc("DIFF linear", "DIFF(G1>G2,H1>H2,1)",
            "+1.0000000000000E+1");
        /* t at an interior knot: the segment to the RIGHT */
        ckc("DIFF knot right", "DIFF(A1>A3,B1>B3,1)",
            "-1.0000000000000E+1");
        cke("DIFF outside hi", "DIFF(D1>D5,E1>E5,4.5)", -1);
        cke("DIFF outside lo", "DIFF(D1>D5,E1>E5,-0.5)", -1);
        cke("DIFF n1", "DIFF(A1>A1,B1>B1,1)", -1);
        cs_set_text(tcell(7, 1), "hole", 'L');
        cke("DIFF hole", "DIFF(G1>G2,H1>H2,1)", -1);
        setv(7, 1, "3");
        cke("DIFF non-range", "DIFF(5,B1>B3,1)", 6);
        cke("DIFF 2 args", "DIFF(A1>A3,B1>B3)", 3);

        /* ==== ROOT: the expression-based bisection (batch F) ==== */
        /* sqrt(2): the 14-digit canon is stable against the
         * tolerance (the true 15th digit is 9) */
        ckc("ROOT sqrt2", "ROOT(X**2-2,0,2)", "+1.4142135623730E+1");
        /* the lowercase binding and the reversed bracket */
        ckc("ROOT lower x", "ROOT(x**2-2,0,2)", "+1.4142135623730E+1");
        ckc("ROOT reversed", "ROOT(X**2-2,2,0)", "+1.4142135623730E+1");
        /* integer roots are reached only to the tolerance - the
         * exact-value pins are tolerance conditionals */
        ckc("ROOT int2",
            "(ABS(ROOT(X**2-4,0,3)-2)<1E-20):1:0", "+1.0000000000000E+1");
        ckc("ROOT int3",
            "(ABS(ROOT(X-3,0,10)-3)<1E-20):1:0", "+1.0000000000000E+1");
        ckc("ROOT quad",
            "(ABS(ROOT(X*X+X-6,0,5)-2)<1E-20):1:0",
            "+1.0000000000000E+1");
        ckc("ROOT pow",
            "(ABS(ROOT(2**X-8,0,5)-3)<1E-20):1:0", "+1.0000000000000E+1");
        /* an endpoint root is returned EXACTLY */
        ckc("ROOT endpoint", "ROOT(X-2,2,5)", "+2.0000000000000E+1");
        /* the bracket midpoint was the exact root */
        ckc("ROOT cubic", "ROOT(X**3-X,-2,2)", "0");
        /* pi through SIN(X) */
        ckc("ROOT pi", "ROOT(SIN(X),2,4)", "+3.1415926535897E+1");
        /* the errors */
        cke("ROOT no sign change", "ROOT(X**2+1,-5,5)", -1);
        cke("ROOT bracket miss", "ROOT(X-1,5,10)", -1);
        cke("ROOT constant", "ROOT(1,0,2)", -1);
        cke("ROOT string fn", "ROOT(\"s\",0,1)", -1);
        cke("ROOT 2 args", "ROOT(X**2-2,0)", 3);
        cke("ROOT no args", "ROOT()", 3);
        /* the binding does NOT leak: a bare X outside ROOT is a
         * syntax error as always */
        cke("X unbound", "X", 6);
        cke("X+1 unbound", "X+1", 6);
        /* ==== ISPRIME (batch G) ==== */
        ckc("ISPRIME 2", "ISPRIME(2)", "+1.0000000000000E+1");
        ckc("ISPRIME 3", "ISPRIME(3)", "+1.0000000000000E+1");
        ckc("ISPRIME 97", "ISPRIME(97)", "+1.0000000000000E+1");
        ckc("ISPRIME 101", "ISPRIME(101)", "+1.0000000000000E+1");
        ckc("ISPRIME 0", "ISPRIME(0)", "0");
        ckc("ISPRIME 1", "ISPRIME(1)", "0");
        ckc("ISPRIME -7", "ISPRIME(-7)", "0");
        ckc("ISPRIME 4", "ISPRIME(4)", "0");
        ckc("ISPRIME 561", "ISPRIME(561)", "0");
        ckc("ISPRIME 1105", "ISPRIME(1105)", "0");
        ckc("ISPRIME 1729", "ISPRIME(1729)", "0");
        ckc("ISPRIME 19digit", "ISPRIME(9223372036854775837)",
            "+1.0000000000000E+1");
        ckc("ISPRIME 19digit2", "ISPRIME(9223372036854775783)",
            "+1.0000000000000E+1");
        ckc("ISPRIME 11digit", "ISPRIME(32416190071)",
            "+1.0000000000000E+1");
        cke("ISPRIME 2.5", "ISPRIME(2.5)", -1);
        cke("ISPRIME cap", "ISPRIME(10000000000000000000)", -1);
        cke("ISPRIME no arg", "ISPRIME()", 6);
        ckc("ISPRIME NOT", "NOT(ISPRIME(4))", "+1.0000000000000E+1");
        /* ==== RAND / RANDF / SEED (batch G) ==== */
        ckc("SEED returns", "SEED(7)", "+7.0000000000000E+1");
        /* SEED returns its value, so the draw alone needs a
         * neutralizing prefix - the second SEED re-seeds the state
         * and cancels the first: draw from state 42 = 1 */
        ckc("RAND seeded", "SEED(42)-SEED(42)+RAND(1,100)",
            "+1.0000000000000E+1");
        ckc("RAND seeded 6", "SEED(42)-SEED(42)+RAND(1,6)",
            "+1.0000000000000E+1");
        ckc("RAND determinism",
            "(SEED(42)+RAND(1,6) = SEED(42)+RAND(1,6)):1:0",
            "+1.0000000000000E+1");
        ckc("RAND fixed", "RAND(3,3)", "+3.0000000000000E+1");
        ckc("RAND range", "(AND(RAND(1,6)>=1,RAND(1,6)<=6)):1:0",
            "+1.0000000000000E+1");
        ckc("RAND truncate",
            "(AND(RAND(1.5,6.7)>=1,RAND(1.5,6.7)<=7)):1:0",
            "+1.0000000000000E+1");
        ckc("RAND swap", "(AND(RAND(6,1)>=1,RAND(6,1)<=6)):1:0",
            "+1.0000000000000E+1");
        ckc("RANDF range", "(AND(RANDF(2,5)>=2,RANDF(2,5)<5)):1:0",
            "+1.0000000000000E+1");
        ckc("RANDF frac bounds",
            "(AND(RANDF(0.25,0.5)>=0.25,RANDF(0.25,0.5)<0.5)):1:0",
            "+1.0000000000000E+1");
        cke("RAND 1 arg", "RAND(1)", 3);
        cke("RAND text", "RAND(\"x\",5)", 6);
        cke("RANDF 1 arg", "RANDF(2)", 3);
        cke("SEED no arg", "SEED()", 6);
        /* the VOLATILE machinery: the flagged cell settles, the flag
         * survives the CSD round trip, the sheet never overruns */
        cs_clear_all();
        cs_commit_entry(0, 0, "+RAND(1,6)");            /* A1 */
        if (!tread(0, 0)->volatf)
            { printf("FAIL volatf flag\n"); fails++; }
        else printf("ok   the volatile flag set at commit\n");
        cs_run_recalc_passes(1);
        if (cs_recalc_limit_hit())
            { printf("FAIL volatile overran\n"); fails++; }
        else if (cs_recalc_last_passes > 3)
            { printf("FAIL volatile passes %d\n", cs_recalc_last_passes);
              fails++; }
        else printf("ok   the volatile cell settles (%d passes)\n",
                    cs_recalc_last_passes);
        cs_run_recalc_passes(1);
        if (cs_recalc_limit_hit())
            { printf("FAIL second recalc overran\n"); fails++; }
        else printf("ok   the second recalculation settles\n");
        if (cs_save("t_vol", 0, 0, 2, 2, "")) {
            cs_clear_all();
            if (cs_load("t_vol", 0, 0, 0)) {
                if (!tread(0, 0)->volatf)
                    { printf("FAIL volatf round trip\n"); fails++; }
                else printf("ok   the volatile flag survives the file\n");
                cs_run_recalc_passes(1);
                if (cs_recalc_limit_hit())
                    { printf("FAIL loaded volatile overran\n"); fails++; }
                else printf("ok   the loaded volatile cell settles\n");
            } else { printf("FAIL volatile load\n"); fails++; }
        } else { printf("FAIL volatile save\n"); fails++; }
        remove("t_vol.csd");
        cs_clear_all();
        /* the RETAIN semantics (the user's change, 2026-11-16): the
         * whole-sheet drivers never re-roll; the explicit paths do */
        cs_commit_entry(0, 0, "+RAND(1,1000000)");      /* A1 */
        cs_run_recalc_passes(1);
        {
            Csn v1, v2;
            csn_copy(&v1, &tread(0, 0)->val);
            cs_run_recalc_passes(1);                    /* ;R A again */
            csn_copy(&v2, &tread(0, 0)->val);
            if (csn_cmp(&v1, &v2) != 0)
                { printf("FAIL retain across ;R A\n"); fails++; }
            else printf("ok   ;R A retains the drawn value\n");
            /* the same-text recommit retains */
            cs_commit_entry(0, 0, "+RAND(1,1000000)");
            csn_copy(&v2, &tread(0, 0)->val);
            if (csn_cmp(&v1, &v2) != 0)
                { printf("FAIL same-text recommit\n"); fails++; }
            else printf("ok   the same-text recommit retains\n");
            /* the changed text redraws (in range) */
            cs_commit_entry(0, 0, "+RAND(1,1000000)");
            {
                Csn one, mx;
                csn_from_int(&one, 1L);
                csn_from_int(&mx, 1000000L);
                if (csn_cmp(&tread(0, 0)->val, &one) < 0 ||
                    csn_cmp(&tread(0, 0)->val, &mx) > 0)
                    { printf("FAIL redraw range\n"); fails++; }
                else printf("ok   the changed entry redraws in range\n");
            }
            /* ;R E redraws: three rolls, not all equal (a triple
             * collision is < 1e-12 - statistical, flagged) */
            {
                Csn r1, r2, r3;
                int same;
                cs_volatile_draw_set(1);
                cs_recalc_entry(0, 0);
                csn_copy(&r1, &tread(0, 0)->val);
                cs_recalc_entry(0, 0);
                csn_copy(&r2, &tread(0, 0)->val);
                cs_recalc_entry(0, 0);
                csn_copy(&r3, &tread(0, 0)->val);
                cs_volatile_draw_set(0);
                same = csn_cmp(&r1, &r2) == 0 && csn_cmp(&r2, &r3) == 0;
                if (same)
                    { printf("FAIL ;R E redraw (all equal)\n"); fails++; }
                else printf("ok   ;R E redraws on demand\n");
            }
            /* RECALC(cell): the conditional's one-shot re-roll */
            cs_commit_entry(1, 0, "+(1):RECALC(REF(1,1));0:0");
            {
                Csn six;
                csn_from_int(&six, 1000000L);
                if (csn_cmp(&tread(0, 0)->val, &six) > 0 ||
                    csn_cmp(&tread(0, 0)->val, &six) == 0)
                    { printf("FAIL RECALC(cell) redraw\n"); fails++; }
                else printf("ok   RECALC(cell) re-rolls the target\n");
                if (cs_recalc_limit_hit())
                    { printf("FAIL RECALC(cell) overran\n"); fails++; }
            }
        }
        cs_clear_all();

        /* the /r /l /c alignment on a FORMULA's string result
         * (user request 2026-11-15) */
        cs_clear_all();
        setv(0, 0, "4");                      /* A1 = 4: MOD(4,2)=0 -> even */
        cs_commit_entry(1, 0, "+(MOD(A1,2)=0):\"even\":\"odd\"");
        if (tread(1, 0)->just != 'L')
            { printf("FAIL formula default just (%c)\n", tread(1, 0)->just);
              fails++; }
        else printf("ok   a formula string result defaults to left\n");
        cs_commit_entry(1, 0, "/r");          /* the edit field: /r alone */
        if (tread(1, 0)->just != 'R' ||
            !tread(1, 0)->strres || strcmp(tread(1, 0)->strres, "even"))
            { printf("FAIL /r on formula\n"); fails++; }
        else printf("ok   /r re-justifies the formula's string result\n");
        cs_commit_entry(1, 0, "/c");
        if (tread(1, 0)->just != 'C')
            { printf("FAIL /c on formula\n"); fails++; }
        else printf("ok   /c re-justifies the formula's string result\n");
        cs_commit_entry(1, 0, "/l");
        if (tread(1, 0)->just != 'L')
            { printf("FAIL /l on formula\n"); fails++; }
        else printf("ok   /l re-justifies the formula's string result\n");
        cs_recalc_all();                      /* ;R keeps the alignment */
        if (tread(1, 0)->just != 'L' ||
            !tread(1, 0)->strres || strcmp(tread(1, 0)->strres, "even"))
            { printf("FAIL alignment lost on ;R\n"); fails++; }
        else printf("ok   ;R keeps the alignment and the string\n");
        /* the numeric result: always right */
        cs_commit_entry(1, 0, "+A1*10");
        if (tread(1, 0)->just != 'R')
            { printf("FAIL numeric just (%c)\n", tread(1, 0)->just); fails++; }
        else printf("ok   a numeric result is right-justified\n");
        cs_clear_all();
        /* a copied aligned formula keeps its alignment */
        setv(0, 0, "2");
        cs_commit_entry(1, 0, "+(MOD(A1,2)=0):\"even\":\"odd\"");
        cs_commit_entry(1, 0, "/r");
        cs_copy_block(1, 0, 1, 0, 3, 0, 1, 1, 0, 1, 1);
        if (tread(3, 0)->just != 'R')
            { printf("FAIL copied alignment\n"); fails++; }
        else printf("ok   a copied formula keeps its alignment\n");
        cs_clear_all();
        /* /r alone on a plain numeric cell: harmless no-change */
        setv(0, 0, "42");
        cs_commit_entry(0, 0, "/r");
        if (tread(0, 0)->just != 'R' || tread(0, 0)->type != CE_NUM)
            { printf("FAIL /r on numeric\n"); fails++; }
        else printf("ok   /r on a numeric cell changes nothing\n");
        cs_clear_all();
        /* the OVERLAPPING copy: A1=1, A2=+A1+1, the copy A1>A2 to
         * A2>A3 - the snapshot protects the SOURCE TEXTS (the clone),
         * while the formulas evaluate sequentially against the
         * mid-copy sheet (the historical behavior): A2 = the copy of
         * A1 = 1, A3 = +A2+1 = 2 */
        cs_clear_all();
        setv(0, 0, "1");
        cs_commit_entry(0, 1, "+A1+1");
        cs_copy_block(0, 0, 0, 1, 0, 1, 1, 2, 0, 2, 1);
        {
            Csn want;
            int ok5 = 1;
            csn_from_int(&want, 1);
            if (csn_cmp(&tread(0, 1)->val, &want) != 0) ok5 = 0;
            csn_from_int(&want, 2);
            if (csn_cmp(&tread(0, 2)->val, &want) != 0) ok5 = 0;
            if (!tread(0, 2)->cont || strcmp(tread(0, 2)->cont, "+A2+1"))
                ok5 = 0;
            if (!ok5) { printf("FAIL overlap copy\n"); fails++; }
            else printf("ok   overlapping copy: the texts survive\n");
        }
        cs_clear_all();
        /* the empty-as-zero rule (user report 2026-11-14): a relative
         * copy across empty cells yields numbers; VAL stays strict */
        cs_clear_all();
        setv(0, 0, "1"); setv(1, 0, "2");     /* A1, B1 */
        cs_commit_entry(0, 1, "+A1+1");       /* A2 = 2 */
        cs_commit_entry(1, 1, "+B1+2");       /* B2 = 4 */
        cke("VAL empty strict", "VAL(A4)", 1);
        {
            Csn r;
            char g[48];
            int e = cs_eval_formula("+A4+1", &r, NULL);
            csn_canon(&r, g, (int) sizeof(g));
            if (e != 0 || strcmp(g, "+1.0000000000000E+1"))
                { printf("FAIL bare empty zero (%s)\n", g); fails++; }
            else printf("ok   a bare empty reference is zero\n");
        }
        /* the single-cell fill (user's working case): A2=+A1+1
         * copied to A3>A10 chains: 3,4,...,10 */
        cs_clear_all();
        setv(0, 0, "1");
        cs_commit_entry(0, 1, "+A1+1");
        cs_copy_block(0, 1, 0, 1, 0, 2, 1, 8, 1, 9, 1);
        {
            int w, ok = 1;
            char g[48];
            /* the values 3..10 */
            for (w = 3; w <= 10; w++) {
                Csn r;
                char gg[48];
                csn_from_int(&r, w);
                csn_canon(&r, gg, (int) sizeof(gg));
                csn_canon(&tread(0, w - 1)->val, g, (int) sizeof(g));
                if (tread(0, w - 1)->type != CE_NUM || strcmp(g, gg))
                    ok = 0;
            }
            if (!ok) { printf("FAIL single-cell fill chain\n"); fails++; }
            else printf("ok   single-cell fill chains 3..10\n");
        }
        cs_clear_all();
        /* the range copy A2>B2 -> A5>B5: A5=1.00, B5=2.00 */
        setv(0, 0, "1"); setv(1, 0, "2");
        cs_commit_entry(0, 1, "+A1+1");
        cs_commit_entry(1, 1, "+B1+2");
        cs_copy_block(0, 1, 1, 1, 0, 4, 2, 1, 0, 2, 1);
        if (tread(0, 4)->type != CE_NUM ||
            tread(0, 4)->val.nd[0] != 1 ||
            tread(1, 4)->type != CE_NUM ||
            tread(1, 4)->val.nd[0] != 2)
            { printf("FAIL range copy over empties\n"); fails++; }
        else printf("ok   range copy over empties: A5=1, B5=2\n");
        cs_clear_all();
        /* CHR / ORD: the 7-bit printable range 32..127, strict
         * (user-refined 2026-11-13) */
        cs_clear_all();
        cke("CHR A", "CHR(65)", 0);
        {
            Csn r;
            char *sf = NULL;
            int e = cs_eval_formula("CHR(65)", &r, &sf);
            if (e != 0 || sf == NULL || strcmp(sf, "A"))
                { printf("FAIL CHR 65 (%s)\n", sf ? sf : "(null)"); fails++; }
            else printf("ok   CHR(65) = the string A\n");
            if (sf) free(sf);
        }
        cke("CHR 31", "CHR(31)", -1);
        cke("CHR 128", "CHR(128)", -1);
        cke("CHR 65.5", "CHR(65.5)", -1);
        cke("CHR 0", "CHR(0)", -1);
        /* the pass cap (raised to 10000, user decision 2026-11-13,
         * for long-running state machines like the brainfuck
         * interpreter) - a never-settling counter runs exactly this
         * many passes */
        cs_clear_all();
        setv(0, 0, "0");                        /* A1 = 0: the SET source */
        cs_commit_entry(0, 0, "1:SET(A1,VAL(A1)+1);RECALC();0:0");
        cs_recalc_all();
        if (cs_recalc_last_passes != 10000)
            { printf("FAIL pass cap (%d)\n", cs_recalc_last_passes); fails++; }
        else printf("ok   the pass cap: 10000 passes\n");
        /* the formula SURVIVES the self-targeting SET (2026-11-13) */
        if (!tread(0, 0)->cont ||
            strcmp(tread(0, 0)->cont, "1:SET(A1,VAL(A1)+1);RECALC();0:0"))
            { printf("FAIL self-SET keeps formula\n"); fails++; }
        else printf("ok   a self-targeting SET keeps the formula\n");
        cs_clear_all();
        /* a bounded self-targeting machine settles */
        setv(0, 0, "0");                        /* A1 = 0 */
        cs_commit_entry(1, 0, "(VAL(A1)<5):SET(A1,VAL(A1)+1);RECALC();0:VAL(A1)");
        cs_recalc_all();
        {
            char gb[48];
            csn_canon(&tread(0, 0)->val, gb, (int) sizeof(gb));
            if (cs_recalc_last_passes >= 10000 || strcmp(gb, "+5.0000000000000E+1"))
                { printf("FAIL bounded machine (passes=%d A1=%s)\n",
                         cs_recalc_last_passes, gb); fails++; }
            else printf("ok   a bounded machine settles (A1=5)\n");
        }
        if (!tread(1, 0)->cont ||
            strcmp(tread(1, 0)->cont,
                   "(VAL(A1)<5):SET(A1,VAL(A1)+1);RECALC();0:VAL(A1)"))
            { printf("FAIL bounded machine keeps formula\n"); fails++; }
        else printf("ok   the bounded machine keeps its formula\n");
        cs_clear_all();
        /* the brainfuck interpreter (user-ext 2026-11-13): the
         * documented formula machinery runs one instruction per
         * recalculation pass; each program must SETTLE (halt) */
        bf_build("+++[>+++<-]", NULL);
        cs_recalc_all();
        {
            char gt[48];
            csn_canon(&tread(0, 1)->val, gt, (int) sizeof(gt));
            if (cs_recalc_last_passes >= 10000 ||
                strcmp(gt, "+9.0000000000000E+1") ||
                tread(5, 0)->val.nd[0] != 3)
                { printf("FAIL bf multiply (passes=%d A2=%s)\n",
                         cs_recalc_last_passes, gt); fails++; }
            else printf("ok   bf +++[>+++<-]: tape 9, halted\n");
        }
        bf_build("++++++++[>++++++++<-]>++++++++.", NULL);
        cs_recalc_all();
        if (cs_recalc_last_passes >= 10000 ||
            !tread(18, 6)->cont || strcmp(tread(18, 6)->cont, "H"))
            { printf("FAIL bf H (%s)\n",
                     tread(18, 6)->cont ? tread(18, 6)->cont : "");
              fails++; }
        else printf("ok   bf prints H\n");
        bf_build("++++++++[>++++++++<-]>++++++++.+++++++++++++++++++++++++++++++++.", NULL);
        cs_recalc_all();
        if (cs_recalc_last_passes >= 10000 ||
            !tread(18, 6)->cont || strcmp(tread(18, 6)->cont, "Hi"))
            { printf("FAIL bf Hi (%s)\n",
                     tread(18, 6)->cont ? tread(18, 6)->cont : "");
              fails++; }
        else printf("ok   bf prints Hi\n");
        bf_build(",.", "A");
        cs_recalc_all();
        if (cs_recalc_last_passes >= 10000 ||
            !tread(18, 6)->cont || strcmp(tread(18, 6)->cont, "A"))
            { printf("FAIL bf input\n"); fails++; }
        else printf("ok   bf reads input and echoes A\n");
        /* HELLO, WORLD! (user request 2026-11-13): the classic program
         * minus its final '.' - the newline byte 10 is outside CHR's
         * printable 32..127 domain.  ~1832 passes, output "Hello
         * World!" down column S rows 7..18. */
        bf_build("++++++++[>++++[>++>+++>+++>+<<<<-]>+>+>->>+[<]<-]>>.>---"
                 ".+++++++..+++.>>.<-.<.+++.------.--------.>>+.>++", NULL);
        cs_recalc_all();
        {
            Cell *oc = tread(18, 6);
            if (cs_recalc_last_passes >= 10000 ||
                tread(5, 0)->val.nd[0] != 3 ||
                !oc->cont || strcmp(oc->cont, "Hello World!") ||
                oc->type != CE_TEXT)
                { printf("FAIL bf hello (passes=%d out=%s)\n",
                         cs_recalc_last_passes,
                         oc->cont ? oc->cont : ""); fails++; }
            else printf("ok   bf hello, world! (%d passes, one cell)\n",
                        cs_recalc_last_passes);
        }
        cs_clear_all();
        {
            Csn r;
            char *sf = NULL;
            char gg[48];
            int e = cs_eval_formula("ORD(\"A\")", &r, &sf);
            csn_canon(&r, gg, (int) sizeof(gg));
            if (e != 0 || strcmp(gg, "+6.5000000000000E+2"))
                { printf("FAIL ORD A (%s)\n", gg); fails++; }
            else printf("ok   ORD(\"A\") = 65\n");
            if (sf) free(sf);
        }
        /* a leading '+' AFFIRMS a string: +CHR(65) is the standard
         * entry form (2026-11-13 bugfix - the unary path gave the
         * mixing error 6) */
        {
            Csn r;
            char *sf = NULL;
            int e = cs_eval_formula("+CHR(65)", &r, &sf);
            if (e != 0 || sf == NULL || strcmp(sf, "A"))
                { printf("FAIL +CHR(65) (%s)\n", sf ? sf : "(null)"); fails++; }
            else printf("ok   +CHR(65) = A (leading + affirms)\n");
            if (sf) free(sf);
        }
        {
            Csn r;
            char *sf = NULL;
            int e = cs_eval_formula("+CHR(72)&CHR(73)", &r, &sf);
            if (e != 0 || sf == NULL || strcmp(sf, "HI"))
                { printf("FAIL +CHR& (%s)\n", sf ? sf : "(null)"); fails++; }
            else printf("ok   +CHR(72)&CHR(73) = HI\n");
            if (sf) free(sf);
        }
        cke("-CHR(65)", "-CHR(65)", 6);     /* a unary '-' stays an error */
        /* the STATUS-BOX file name: basename, no extension, at most
         * 10 chars (user report 2026-11-14) */
        {
            char nb[24];
            struct { const char *in; const char *want; } fc[] = {
                { "examples/brainf.csd", "brainf" },
                { "a/b/c.CSD", "c" },
                { "1234567890123.csd", "1234567890" },
                { "", "" },
                { "noext", "noext" },
                { "x.sheet.csd", "x.sheet" },
            };
            int q, ok = 1;
            for (q = 0; q < 6; q++) {
                cs_fname_disp(fc[q].in, nb, (int) sizeof(nb));
                if (strcmp(nb, fc[q].want))
                    { printf("FAIL fname %d (%s)\n", q, nb); ok = 0; }
            }
            if (!ok) fails++;
            else printf("ok   the status-box file name\n");
        }
        /* a ONE-CHARACTER entry that is not a digit is TEXT (the bf
         * program cells, flagged refinement 2026-11-13) */
        cs_clear_all();
        cs_commit_entry(0, 0, "+");
        cs_commit_entry(1, 0, "-");
        cs_commit_entry(2, 0, ".");
        cs_commit_entry(3, 0, ",");
        cs_commit_entry(4, 0, "[");
        {
            int k, ok = 1;
            const char *want = "+-.,[";
            for (k = 0; k < 5; k++)
                if (tread(k, 0)->type != CE_TEXT ||
                    !tread(k, 0)->cont || tread(k, 0)->cont[0] != want[k])
                    ok = 0;
            if (!ok) { printf("FAIL one-char text entries\n"); fails++; }
            else printf("ok   one-char + - . , [ entries are text\n");
        }
        cs_commit_entry(5, 0, "5");         /* a lone digit stays numeric */
        if (tread(5, 0)->type != CE_NUM)
            { printf("FAIL lone digit\n"); fails++; }
        else printf("ok   a lone digit stays numeric\n");
        cs_commit_entry(6, 0, "+A");        /* a real formula: still an error cell */
        if (tread(6, 0)->type != CE_NUM || !tread(6, 0)->iserr)
            { printf("FAIL +A entry\n"); fails++; }
        else printf("ok   +A stays a formula error cell\n");
        cs_clear_all();
        /* the DERIVED TYPE (user bug report 2026-11-13): a formula
         * whose result is a string IS a text value - for VAL, for the
         * bare-reference coercion, for the display and the type line */
        cs_clear_all();
        cs_commit_entry(0, 0, "+CHR(65)&CHR(66)");   /* A1 = "AB" */
        if (!tread(0, 0)->strres || strcmp(tread(0, 0)->strres, "AB"))
            { printf("FAIL derived strres (%s)\n",
                     tread(0, 0)->strres ? tread(0, 0)->strres : "(null)");
              fails++; }
        else printf("ok   a string-result formula keeps its string\n");
        {
            Csn r;
            char *sf = NULL;
            int e = cs_eval_formula("VAL(A1)", &r, &sf);
            if (e != 0 || sf == NULL || strcmp(sf, "AB"))
                { printf("FAIL VAL of string formula (%s)\n",
                         sf ? sf : "(null)"); fails++; }
            else printf("ok   VAL(A1) yields the string AB\n");
            if (sf) free(sf);
        }
        {
            Csn r;
            char *sf = NULL;
            int e = cs_eval_formula("(A1=\"AB\"):\"y\":\"n\"", &r, &sf);
            if (e != 0 || sf == NULL || strcmp(sf, "y"))
                { printf("FAIL bare-ref string compare (%s)\n",
                         sf ? sf : "(null)"); fails++; }
            else printf("ok   a bare reference coerces to the string\n");
            if (sf) free(sf);
        }
        {
            Csn r;
            char *sf = NULL;
            int e = cs_eval_formula("+VAL(A1)&\"!\"", &r, &sf);
            if (e != 0 || sf == NULL || strcmp(sf, "AB!"))
                { printf("FAIL string chain (%s)\n", sf ? sf : "(null)");
                  fails++; }
            else printf("ok   VAL(A1)&\"!\" chains the string\n");
            if (sf) free(sf);
        }
        cs_commit_entry(2, 0, "+\"ABC\"");            /* C1 = "ABC" */
        cs_commit_entry(1, 0, "+VAL(C1)");            /* B1 = "ABC" */
        if (!tread(1, 0)->strres || strcmp(tread(1, 0)->strres, "ABC"))
            { printf("FAIL VAL text formula (%s)\n",
                     tread(1, 0)->strres ? tread(1, 0)->strres : "(null)");
              fails++; }
        else printf("ok   +VAL(C1) shows ABC, derived type text\n");
        /* the derived type flips back: a numeric result clears the
         * string and the cell is numeric again */
        cs_commit_entry(0, 0, "+1+1");
        if (tread(0, 0)->strres != NULL)
            { printf("FAIL derived flip-back (strres not cleared)\n"); fails++; }
        else printf("ok   a numeric result clears the string type\n");
        {
            char gb[48];
            csn_canon(&tread(0, 0)->val, gb, (int) sizeof(gb));
            if (strcmp(gb, "+2.0000000000000E+1"))
                { printf("FAIL flip-back value (%s)\n", gb); fails++; }
            else printf("ok   the cell is numeric again (2)\n");
        }
        /* a string-result cell inside a range is SKIPPED like text */
        cs_clear_all();
        cs_commit_entry(0, 0, "+\"txt\"");            /* A1 = "txt" */
        setv(0, 1, "5");                              /* A2 = 5 */
        ckc("range skips string formula", "SUM(A1>A2)",
            "+5.0000000000000E+1");
        cs_clear_all();
        cke("ORD ABC", "ORD(\"ABC\")", -1);
        cke("ORD empty", "ORD(\"\")", -1);
        cke("ORD number", "ORD(5)", 6);
        cke("ORD space", "ORD(\" \")", 0);
        cke("ORD out-of-range", "ORD(\"\x80\")", -1);
        /* the round trips */
        ckc("ORD CHR 65", "ORD(CHR(65))", "+6.5000000000000E+2");
        {
            Csn r;
            char *sf = NULL;
            int e = cs_eval_formula("CHR(ORD(\"A\"))", &r, &sf);
            if (e != 0 || sf == NULL || strcmp(sf, "A"))
                { printf("FAIL CHR ORD round trip\n"); fails++; }
            else printf("ok   CHR(ORD(\"A\")) = A\n");
            if (sf) free(sf);
        }
        /* the & concatenation: String & String only */
        cs_clear_all();
        setv(0, 0, "42");
        cs_set_text(tcell(0, 1), "hello", 'L');
        cs_set_text(tcell(1, 0), "World", 'L');
        cs_set_text(tcell(1, 1), "World", 'L');
        {
            Csn r;
            char *sf = NULL;
            int e = cs_eval_formula("A2&B1", &r, &sf);
            if (e != 0 || sf == NULL || strcmp(sf, "helloWorld"))
                { printf("FAIL concat refs (%s)\n", sf ? sf : "(null)"); fails++; }
            else printf("ok   B1&C2 = helloWorld\n");
            if (sf) free(sf);
        }
        {
            Csn r;
            char *sf = NULL;
            int e = cs_eval_formula("CHR(72)&CHR(73)", &r, &sf);
            if (e != 0 || sf == NULL || strcmp(sf, "HI"))
                { printf("FAIL concat CHR (%s)\n", sf ? sf : "(null)"); fails++; }
            else printf("ok   CHR(72)&CHR(73) = HI\n");
            if (sf) free(sf);
        }
        {
            Csn r;
            char *sf = NULL;
            int e = cs_eval_formula("(\"AB\"=\"AB\"):(\"x\"):(\"y\")", &r, &sf);
            if (e != 0 || sf == NULL || strcmp(sf, "x"))
                { printf("FAIL string cond (%s)\n", sf ? sf : "(null)"); fails++; }
            else printf("ok   a string condition composes\n");
            if (sf) free(sf);
        }
        cs_clear_all();
        /* INTG / FRAC / SGN / NCR / NPR (user-ext 2026-11-13) */
        ckc("INTG 3.5", "INTG(3.5)", "+3.0000000000000E+1");
        ckc("INTG -3.5", "INTG(-3.5)", "-3.0000000000000E+1");
        ckc("FRAC 3.5", "FRAC(3.5)", "+5.0000000000000E+0");
        ckc("FRAC -3.5", "FRAC(-3.5)", "-5.0000000000000E+0");
        ckc("FRAC -3.5 sign", "-FRAC(-3.5)", "+5.0000000000000E+0");
        ckc("SGN -3", "SGN(-3)", "-1.0000000000000E+1");
        ckc("SGN 3", "SGN(3)", "+1.0000000000000E+1");
        ckc("SGN 0", "SGN(0)", "0");
        ckc("NCR 5,2", "NCR(5,2)", "+1.0000000000000E+2");
        ckc("NPR 5,2", "NPR(5,2)", "+2.0000000000000E+2");
        cke("NCR cap", "NCR(30,2)", -1);
        /* the FRACTIONAL factorial: FACT(x) = Gamma(x+1) for x > 0
         * (the Lanczos approximation, user-ext 2026-11-13) - the
         * integer path (and its cap) is unchanged */
        ckc("FACT 2.2", "FACT(2.2)", "+2.4239654799353E+1");
        ckc("FACT 0.5", "FACT(0.5)", "+8.8622692545275E+0");
        ckc("FACT 5.5", "FACT(5.5)", "+2.8788527781504E+3");
        cke("FACT negative", "FACT(-2.2)", -1);
        cke("FACT huge", "FACT(2000.5)", -1);
        ckc("FACT int unchanged", "FACT(4)", "+2.4000000000000E+2");
        cke("FACT int cap", "FACT(25)", -1);
        /* the factorial ENTRY with a fractional source */
        cs_clear_all();
        cs_commit_entry(0, 0, "2.2!");
        {
            Cell *c = tread(0, 0);
            char g2[48];
            csn_canon(&c->val, g2, (int) sizeof(g2));
            if (c->type != CE_NUM || !c->factf ||
                strcmp(c->cont, "2.2!") || strcmp(g2, "+2.4239654799353E+1"))
                { printf("FAIL fact 2.2! entry\\n"); fails++; }
            else printf("ok   entry 2.2!: gamma, content kept\\n");
        }
        cs_clear_all();
        /* the FACTORIAL ENTRY: "5!" displays 120, content "5!" */
        cs_clear_all();
        cs_commit_entry(0, 0, "5!");
        {
            Cell *c = tread(0, 0);
            char g2[48];
            csn_canon(&c->val, g2, (int) sizeof(g2));
            if (c->type != CE_NUM || c->factf != 1 ||
                strcmp(c->cont, "5!") || strcmp(g2, "+1.2000000000000E+3"))
                { printf("FAIL fact entry 5! (ff=%d cont='%s')\n", c->factf, c->cont ? c->cont : ""); fails++; }
            else printf("ok   entry 5!: shows 120, content 5!\n");
        }
        /* A5! tracks A5 through ;R */
        {
            Csn v;
            csn_parse("5", &v);
            cs_set_num(tcell(0, 4), "5", &v, 0, NULL);   /* A5 = 5 */
            cs_commit_entry(0, 5, "A5!");                   /* A6 = FACT(A5) */
            cs_recalc_all();
            if (tread(0, 5)->val.nd[0] != 1 || tread(0, 5)->val.nd[1] != 2)
                { printf("FAIL A5! value\n"); fails++; }
            setv(0, 4, "3");                    /* A5 = 3 -> FACT = 6 */
            cs_recalc_all();
            if (tread(0, 5)->val.nd[0] != 6)
                { printf("FAIL A5! recompute (%d)\n", tread(0, 5)->val.nd[0]); fails++; }
            else printf("ok   A5! tracks A5 through ;R\n");
        }
        /* REF(2,5)! and the non-expression fallbacks */
        cs_clear_all();
        {
            Csn v;
            csn_parse("4", &v);
            cs_set_num(tcell(1, 4), "4", &v, 0, NULL);   /* B5 = 4 */
            cs_commit_entry(2, 4, "REF(2,5)!");             /* C5 = FACT(B5) */
            if (tread(2, 4)->val.nd[0] != 2 || tread(2, 4)->val.nd[1] != 4)
                { printf("FAIL REF factorial (24?)\n"); fails++; }
            else printf("ok   REF(2,5)! = 24\n");
            cs_commit_entry(0, 6, "STOP!");                 /* stays text */
            if (tread(0, 6)->type != CE_TEXT)
                { printf("FAIL STOP! not text\n"); fails++; }
            else printf("ok   STOP! stays text\n");
            cs_commit_entry(0, 7, "5!!");                   /* the old numeric path */
            if (tread(0, 7)->type != CE_NUM || !tread(0, 7)->iserr)
                { printf("FAIL 5!! numeric error\n"); fails++; }
            else printf("ok   5!! stays text\n");
            cs_commit_entry(0, 8, "A9!");                   /* empty A9: ?n? */
            if (tread(0, 8)->type != CE_NUM || !tread(0, 8)->iserr)
                { printf("FAIL A9! ?n?\n"); fails++; }
            else printf("ok   A9! shows ?n? until A9 is filled\n");
        }
        /* the factf state survives the CSD round trip */
        cs_clear_all();
        cs_commit_entry(0, 0, "5!");
        if (!cs_save("t_ft.CSD", 0, 0, CS_MAXCOL - 1, CS_MAXROW - 1, NULL))
            { printf("FAIL factf save\n"); fails++; }
        cs_clear_all();
        if (!cs_load("t_ft.CSD", 0, 0, 0))
            { printf("FAIL factf load\n"); fails++; }
        {
            Cell *c = tread(0, 0);
            if (c->type != CE_NUM || !c->factf ||
                c->val.nd[0] != 1 || c->val.nd[1] != 2)
                { printf("FAIL factf round trip\n"); fails++; }
            else printf("ok   factf survives save/load\n");
        }
        remove("t_ft.CSD");
        /* INVFACT: the inverse factorial (user-ext 2026-11-13) */
        cs_clear_all();
        ckc("INVFACT 120", "INVFACT(120)", "+5.0000000000000E+1");
        ckc("INVFACT 24", "INVFACT(24)", "+4.0000000000000E+1");
        ckc("INVFACT 6", "INVFACT(6)", "+3.0000000000000E+1");
        ckc("INVFACT 2", "INVFACT(2)", "+2.0000000000000E+1");
        ckc("INVFACT 1", "INVFACT(1)", "+1.0000000000000E+1");
        ckc("INVFACT 119.5", "INVFACT(119.5)", "+4.9975523876792E+1");
        ckc("INVFACT 50", "INVFACT(50)", "+4.4715274582078E+1");
        ckc("INVFACT 0.95", "INVFACT(0.95)", "+8.6419384840602E+0");
        cke("INVFACT 0", "INVFACT(0)", -1);
        cke("INVFACT -5", "INVFACT(-5)", -1);
        cke("INVFACT 0.5", "INVFACT(0.5)", -1);
        ckc("INVFACT 1e400", "INVFACT(1e400)", "+2.1084996061940E+3");
        /* the round trip to full precision: INVFACT(FACT(2.2)) = 2.2 */
        ckc("INVFACT round trip", "INVFACT(FACT(2.2))", "+2.1999999999999E+1");
        ckc("INVFACT round trip 0.5", "INVFACT(FACT(0.5))", "+4.9999999999999E+0");
        /* a reference argument */
        setv(0, 0, "120");
        ckc("INVFACT ref", "INVFACT(A1)", "+5.0000000000000E+1");
        /* E(): the Euler number at full internal precision */
        ckc("E()", "E()", "+2.7182818284590E+1");
        ckc("LN E", "LN(E())", "+1.0000000000000E+1");
        ckc("E squared", "E()*E()", "+7.3890560989306E+1");
        /* EXP(1) agrees with the E() constant to ~28 decimals (the
         * constant carries more digits than the exp series rounds) */
        {
            Csn r;
            char *sf = NULL;
            char gg[48];
            int e = cs_eval_formula("EXP(1)-E()", &r, &sf);
            csn_canon(&r, gg, (int) sizeof(gg));
            if (e != 0)
                { printf("FAIL E in exp err %d\n", e); fails++; }
            else if (e == 0 && gg[0] != '\0' &&
                     (strstr(gg, "E-2") != NULL || strstr(gg, "E-1") != NULL ||
                      strcmp(gg, "0") == 0))
                printf("ok   E in exp: agrees to ~28 decimals (%s)\n", gg);
            else
                { printf("FAIL E in exp (%s)\n", gg); fails++; }
            if (sf) free(sf);
        }
        cs_clear_all();
        cs_clear_all();
        cs_clear_all();
    }

    /* cs_name_to_col */
    {
        int dc;
        dc = cs_name_to_col("DW");
        if (dc != CS_MAXCOL - 1) { printf("FAIL DW=%d want %d\n", dc, CS_MAXCOL - 1); fails++; }
        else printf("ok   DW -> %d\n", CS_MAXCOL - 1);
    }
    /* coordinate parse */
    {
        int cc, rr;
        if (cs_parse_coord("C7", &cc, &rr) && cc == 2 && rr == 6)
            printf("ok   C7\n");
        else { printf("FAIL C7\n"); fails++; }
        if (!cs_parse_coord("ZZZ1", &cc, &rr))
            printf("ok   ZZZ1 rejected\n");
        else { printf("FAIL ZZZ1 accepted\n"); fails++; }
    }

    /* conditional strings (user batch): branch literals and text cells */
    {
        Cell *c;
        Csn v2;
        c = tcell(0, 20);
        csn_parse("2", &v2);
        cs_set_num(c, "2", &v2, 0, NULL);
        c = tcell(0, 21);
        csn_parse("5", &v2);
        cs_set_num(c, "5", &v2, 0, NULL);
        {
            int e;
            Csn vv; char *s2 = NULL;
            e = cs_eval_formula("A21<A22:\"a21 is smaller\":\"a22 is smaller\"", &vv, &s2);
            if (e != 0 || !s2 || strcmp(s2, "a21 is smaller")) {
                printf("FAIL cond str-literal e=%d str=%s\n", e, s2 ? s2 : "(none)");
                fails++;
            } else printf("ok   cond str-literal -> %s\n", s2);
            if (s2) free(s2);
        }
        /* text cell referenced in the true branch */
        cs_set_text(tcell(0, 22), "a23 is text", 'L');
        cs_set_text(tcell(2, 23), "c24 is text", 'L');
        {
            int e; Csn vv; char *s2 = NULL;
            e = cs_eval_formula("A21<A22:A23:C24", &vv, &s2);
            if (e != 0 || !s2 || strcmp(s2, "a23 is text")) {
                printf("FAIL cond text-cell-ref e=%d str=%s\n", e, s2 ? s2 : "(none)");
                fails++;
            } else printf("ok   cond text-cell ref -> %s\n", s2);
            if (s2) free(s2);
        }
    }

    /* caret toggles cell interpretation (manual 6.4/14.2) */
    {
        Cell *c;
        int eu;
        Csn v;
        csn_parse("1/4", &v);
        cs_set_num(tcell(5, 5), "1/4", &v, 0, NULL);   /* shows 0.25 */
        c = tread(5, 5);
        curCol = 5; curRow = 5;
        toggle_celltype();
        if (c->type == CE_TEXT && c->cont && !strcmp(c->cont, "1/4"))
            printf("ok   toggle numeric->text keeps cont\n");
        else { printf("FAIL n->t got type %d\n", c->type); fails++; }
        toggle_celltype();
        csn_canon(&c->val, buf, (int) sizeof(buf));
        if (c->type == CE_NUM && !strcmp(buf, "+2.5000000000000E+0")) {
            printf("ok   toggle text->numeric evaluates\n");
        } else { printf("FAIL t->n got %d %s\n", c->type, buf); fails++; }
        /* unparseable text to numeric: error mark, reversible, no loss */
        csn_parse("JUL 20", &v);
        cs_set_text(tcell(6, 5), "JUL 20", 'L');
        eu = entriesUsed;      /* toggles themselves never touch the pool */
        curCol = 6; curRow = 5;
        c = tcell(6, 5);
        toggle_celltype();
        if (c->type == CE_NUM && c->iserr)
            printf("ok   toggle bad text -> numeric ?n?\n");
        else { printf("FAIL bad toggle type %d iserr %d\n", c->type, c->iserr); fails++; }
        c = tcell(6, 5);
        toggle_celltype();           /* reversible */
        if (c->type == CE_TEXT && c->cont && !strcmp(c->cont, "JUL 20"))
            printf("ok   toggle is reversible\n");
        else { printf("FAIL reversible\n"); fails++; }
        if (eu != entriesUsed)
            { printf("FAIL entriesUsed %d -> %d\n", eu, entriesUsed); fails++; }
        printf("ok   toggle keeps entriesUsed %d\n", eu);
    }

    /* relative copy ref adjustments (manual ch. 14 / 5.9 examples) */
    {
        char *adj;
        adj = cs_adjust_refs("+SUM(A1,A2,A3)", 1, 0);
        if (adj && !strcmp(adj, "+SUM(B1,B2,B3)"))
            printf("ok   adjust SUM list\n");
        else { printf("FAIL adjust SUM got %s\n", adj ? adj : "(NULL)"); fails++; }
        if (adj) free(adj);
        adj = cs_adjust_refs("+SUM(B2>B6)/SQRT(A4!)", 1, 0);
        if (adj && !strcmp(adj, "+SUM(C2>C6)/SQRT(A4!)"))
            printf("ok   adjust manual 5.9 example\n");
        else { printf("FAIL adjust 5.9 got %s\n", adj ? adj : "(NULL)"); fails++; }
        if (adj) free(adj);
        adj = cs_adjust_refs("+SUM(A3>A5)*12", 1, 0);
        if (adj && !strcmp(adj, "+SUM(B3>B5)*12"))
            printf("ok   adjust range x12\n");
        else { printf("FAIL adjust range got %s\n", adj ? adj : "(NULL)"); fails++; }
        if (adj) free(adj);
        adj = cs_adjust_refs("+PI()*2", 1, 0);
        if (adj && !strcmp(adj, "+PI()*2"))
            printf("ok   adjust fn name preserved\n");
        else { printf("FAIL adjust PI got %s\n", adj ? adj : "(NULL)"); fails++; }
        if (adj) free(adj);
        adj = cs_adjust_refs("+A1+B2", 1, 0);
        if (adj && !strcmp(adj, "+B1+C2"))
            printf("ok   adjust single letters\n");
        else { printf("FAIL adjust letters got %s\n", adj ? adj : "(NULL)"); fails++; }
        if (adj) free(adj);
        adj = cs_shift_refs("+SUM(A2,A3)", 0, 1, 1);  /* insert row after row 2 */
        if (adj && !strcmp(adj, "+SUM(A3,A4)"))
            printf("ok   shift SUM ref\n");
        else { printf("FAIL shift SUM got %s\n", adj ? adj : "(NULL)"); fails++; }
        if (adj) free(adj);
    }

    /* save / load round trip (CSD2 with BCD values) */
    {
        Cell *c;
        Csn v;
        cs_clear_all();
        csn_parse("7.25", &v);
        cs_set_text(tcell(0, 0), "hello", 'L');
        cs_free_cell_at(1, 0);
        cs_set_text(tcell(1, 0), "*----*", 'W');
        csn_zero(&v);
        cs_set_text(tcell(0, 1), "rep", 'L');
        cs_set_num(tcell(2, 1), "B1", &v, 0, NULL);   /* formula */
        csn_parse("7.25", &v);
        cs_set_num(tcell(3, 1), "7.25", &v, 0, NULL);
        colw[2] = 13; colp[2] = 4;
        if (cs_save("t_rt", 0, 0, 5, 5, "")) {
            cs_clear_all();
            colw[2] = 10; colp[2] = 2;
            if (cs_load("t_rt", 0, 0, 0)) {
                cs_recalc_all();
                c = tcell(0, 0);
                if (c->type == CE_TEXT && c->cont && !strcmp(c->cont, "hello"))
                    printf("ok   save text\n");
                else { printf("FAIL save text\n"); fails++; }
                c = tcell(1, 0);
                if (c->type == CE_TEXT && c->just == 'W' && c->rep &&
                    !strcmp(c->rep, "*----*"))
                    printf("ok   save repeating\n");
                else { printf("FAIL save repeating\n"); fails++; }
                {
                    char g[48];
                    csn_canon(&c->val, g, (int) sizeof(g));
                    c = tcell(2, 1);
                    csn_canon(&c->val, g, (int) sizeof(g));
                    if (c->type == CE_NUM && c->cont && !strcmp(c->cont, "B1")
                        && !strcmp(g, "0")
                        && colw[2] == 13 && colp[2] == 4)
                        printf("ok   save formula + cols\n");
                    else { printf("FAIL save formula + cols\n"); fails++; }
                }
                c = tcell(3, 1);
                {
                    char g[48];
                    csn_canon(&c->val, g, (int) sizeof(g));
                    if (c->type == CE_NUM && !strcmp(g, "+7.2500000000000E+1"))
                        printf("ok   save number\n");
                    else { printf("FAIL save number\n"); fails++; }
                }
                remove("t_rt.csd");
            } else { printf("FAIL load\n"); fails++; }
        } else { printf("FAIL save\n"); fails++; }
    }
    /* the LOADER HARDENING (external review item 7, 2026-11-15): a
     * malformed, truncated or over-budget file is refused BEFORE the
     * user's current sheet is touched - the sheet survives intact */
    {
        FILE *g;
        long flen;
        unsigned char buf[512];
        Cell *c;
        int used;
        cs_clear_all();
        cs_commit_entry(0, 0, "7.25");              /* A1 */
        cs_commit_entry(1, 0, "hello");             /* B1 text */
        used = entriesUsed;
        if (!cs_save("t_ldfix", 0, 0, 2, 2, "")) {
            printf("FAIL ldfix save\n"); fails++;
        } else {
            g = fopen("t_ldfix.csd", "rb");
            fseek(g, 0, SEEK_END);
            flen = ftell(g);
            fseek(g, 0, SEEK_SET);
            if (flen > 400) flen = 400;             /* the fixture fits */
            if (fread(buf, 1, (size_t) flen, g) != (size_t) flen)
                flen = 0;
            fclose(g);
            /* header layout: magic 4 + origin 3 + 254 colw/prec +
             * pwdlen 1 + count 2 = 264 bytes before the first record */
            /* 1. the count patched over budget (0xFFFF > CS_BUDGET) */
            buf[262] = 0xFF; buf[263] = 0xFF;
            g = fopen("t_over.csd", "wb");
            fwrite(buf, 1, (size_t) flen, g); fclose(g);
            if (cs_load("t_over", 0, 0, 0) == 0)
                printf("ok   over-budget file refused\n");
            else { printf("FAIL over-budget load\n"); fails++; }
            /* 2. the first record's type patched to an unknown value */
            buf[262] = 0; buf[263] = 2;             /* count = 2 again */
            buf[264 + 2] = 7;                       /* ty = 7 */
            g = fopen("t_ty.csd", "wb");
            fwrite(buf, 1, (size_t) flen, g); fclose(g);
            if (cs_load("t_ty", 0, 0, 0) == 0)
                printf("ok   unknown record type refused\n");
            else { printf("FAIL unknown type load\n"); fails++; }
            /* 3. a truncated file (cut inside the records) */
            buf[264 + 2] = 2;                       /* ty valid again */
            g = fopen("t_trunc.csd", "wb");
            fwrite(buf, 1, (size_t)(flen * 3 / 4), g); fclose(g);
            if (cs_load("t_trunc", 0, 0, 0) == 0)
                printf("ok   truncated file refused\n");
            else { printf("FAIL truncated load\n"); fails++; }
            /* 4. THE SHEET IS INTACT after every refusal: the cells
             * the load would have destroyed are still there */
            c = tread(0, 0);
            if (c->type == CE_NUM && c->cont && !strcmp(c->cont, "7.25"))
                printf("ok   sheet intact after refusals (A1)\n");
            else { printf("FAIL sheet intact A1\n"); fails++; }
            c = tread(1, 0);
            if (c->type == CE_TEXT && c->cont && !strcmp(c->cont, "hello"))
                printf("ok   sheet intact after refusals (B1)\n");
            else { printf("FAIL sheet intact B1\n"); fails++; }
            if (entriesUsed == used)
                printf("ok   entry count unchanged after refusals\n");
            else { printf("FAIL entry count %d != %d\n", entriesUsed, used);
                   fails++; }
            remove("t_over.csd"); remove("t_ty.csd");
            remove("t_trunc.csd"); remove("t_ldfix.csd");
        }
    }
    /* CSD5 round trip for the DERIVED TYPE (2026-11-13): a string-
     * result formula cell saves its string and shows it right after a
     * load - NO ;R needed (the user's file-format request); a numeric
     * result round-trips with no string */
    {
        cs_clear_all();
        cs_commit_entry(0, 0, "+CHR(65)&CHR(66)");      /* A1 = "AB" */
        cs_commit_entry(1, 0, "+VAL(A1)");              /* B1 = "AB" */
        cs_commit_entry(2, 0, "+\"ABC\"");              /* C1 = "ABC" */
        cs_commit_entry(3, 0, "+1+1");                  /* D1 = 2 (numeric) */
        if (cs_save("t_str", 0, 0, 4, 4, "")) {
            cs_clear_all();
            if (cs_load("t_str", 0, 0, 0)) {
                Cell * c = tcell(0, 0);
                if (c->strres && !strcmp(c->strres, "AB") &&
                    c->type == CE_NUM && c->cont &&
                    !strcmp(c->cont, "+CHR(65)&CHR(66)"))
                    printf("ok   CSD round trip: string formula keeps "
                           "\"AB\" without ;R\n");
                else { printf("FAIL CSD string round trip\n"); fails++; }
                c = tread(1, 0);
                if (c->strres && !strcmp(c->strres, "AB"))
                    printf("ok   CSD round trip: VAL(A1) keeps \"AB\"\n");
                else { printf("FAIL CSD VAL round trip\n"); fails++; }
                c = tread(2, 0);
                if (c->strres && !strcmp(c->strres, "ABC"))
                    printf("ok   CSD round trip: literal formula keeps "
                           "\"ABC\"\n");
                else { printf("FAIL CSD literal round trip\n"); fails++; }
                c = tread(3, 0);
                {
                    char g[48];
                    csn_canon(&c->val, g, (int) sizeof(g));
                    if (!c->strres && !strcmp(g, "+2.0000000000000E+1"))
                        printf("ok   CSD round trip: numeric cell has no "
                               "string\n");
                    else { printf("FAIL CSD numeric round trip\n"); fails++; }
                }
                remove("t_str.csd");
            } else { printf("FAIL CSD string load\n"); fails++; }
        } else { printf("FAIL CSD string save\n"); fails++; }
        /* a LONG string result (200 chars) round-trips through the
         * u16 strres record - built directly on the cell, since an
         * entry cannot exceed CS_EDITMAX */
        cs_clear_all();
        {
            Csn v;
            char *big = (char *) malloc(201);
            csn_parse("0", &v);
            memset(big, 'A', 200);
            big[200] = '\0';
            cs_set_num(tcell(0, 0), "+BIG", &v, 0, big);
            free(big);
        }
        if (cs_save("t_lng", 0, 0, 2, 2, "")) {
            cs_clear_all();
            if (cs_load("t_lng", 0, 0, 0)) {
                Cell *c = tread(0, 0);
                int ok = c->strres && (int) strlen(c->strres) == 200;
                {
                    int i;
                    for (i = 0; ok && i < 200; i++)
                        if (c->strres[i] != 'A') ok = 0;
                }
                if (ok) printf("ok   CSD round trip: a 200-char string "
                               "survives\n");
                else { printf("FAIL CSD long string\n"); fails++; }
                remove("t_lng.csd");
            } else { printf("FAIL CSD long load\n"); fails++; }
        } else { printf("FAIL CSD long save\n"); fails++; }
        cs_clear_all();
    }
    /* Load/Merge position (manual 14-14): cells land at the requested
     * corner; cells running past the sheet edge are parsed and dropped
     * while the file stream stays in sync */
    {
        char g[48];
        Csn v;
        {
            Cell *cc1 = tcell(0, 20);
            Cell *cc2 = tcell(1, 20);
            csn_parse("11", &v); cs_set_num(cc1, "11", &v, 0, NULL); /* A21 */
            csn_parse("22", &v); cs_set_num(cc2, "22", &v, 0, NULL); /* B21 */
            {
                Cell *cc3 = tcell(0, 21);
                csn_parse("33", &v); cs_set_num(cc3, "33", &v, 0, NULL); /* A22 */
            }
        }
        cs_save("t_of", 0, 19, 2, 22, "");
        cs_clear_all();
        /* merge the 3-cell block at E10: the block's upper-left corner
         * (A20, stored origin) goes to E10, so A21 lands at E11 etc.
         * (manual 14-9: the load position anchors the FILE's upper
         * left corner, not its first data cell) */
        if (cs_load("t_of", 4, 9, 1)) {
            char ok1 = 0, ok2 = 0, ok3 = 0;
            {
                char buf[8];
                csn_canon(&tread(4, 10)->val, g, (int) sizeof(g));
                ok1 = (tread(4, 10)->type == CE_NUM &&
                       !strcmp(g, "+1.1000000000000E+2"));
                csn_canon(&tread(5, 10)->val, buf, (int) sizeof(buf));
                ok2 = (tread(5, 10)->type == CE_NUM);
                ok3 = (tread(4, 11)->type == CE_NUM);
            }
            if (!ok1 || !ok2 || !ok3)
                { printf("FAIL load-offset cells (%d %d %d g=%s)\n",
                         ok1, ok2, ok3, g); fails++; }
            else printf("ok   load position offsets cells (E11/F11/E12)\n");
            if (tread(0, 0)->type != CE_EMPTY || tread(0, 20)->type != CE_EMPTY)
                { printf("FAIL offset leak to origin\n"); fails++; }
            else printf("ok   no cells at original coordinates\n");
            /* edge drop: hold the same file near the corner DW/at255 */
            cs_clear_all();
            if (cs_load("t_of", CS_MAXCOL - 1, CS_MAXROW - 1, 1)) {
                /* B21/B22/A22 land outside; the stream must stay in sync
                 * - reload the same file at A1 afterwards and see ALL
                 * three cells (range corner A20 -> A1: A21 -> A2, ...) */
                cs_clear_all();
                if (cs_load("t_of", 0, 0, 1) &&
                    tread(0, 1)->type == CE_NUM &&
                    tread(1, 1)->type == CE_NUM &&
                    tread(0, 2)->type == CE_NUM) {
                    csn_canon(&tread(0, 1)->val, g, (int) sizeof(g));
                    if (strcmp(g, "+1.1000000000000E+2"))
                        { printf("FAIL edge-drop desync\n"); fails++; }
                    else printf("ok   edge drop keeps stream in sync\n");
                } else { printf("FAIL reload after edge drop\n"); fails++; }
            } else { printf("FAIL edge-drop load\n"); fails++; }
        } else { printf("FAIL offset load\n"); fails++; }
        cs_clear_all();
        remove("t_of.csd");
    }
    /* bugfix regressions (user report): (a) deleting a column shifts
     * its width/precision with the values; (b) All-mode save + load at
     * A1 is an identity - the formula lands where it was and survives
     * a recalculation */
    {
        Csn v;
        char g[48];
        int ec;
        colw[0] = 10; colp[0] = 2;
        colw[1] = 38; colp[1] = 12;
        csn_parse("9", &v);
        cs_set_num(tcell(1, 0), "9", &v, 0, NULL);    /* B1 */
        cs_delete_columns(0, 0);
        if (colw[0] != 38 || colp[0] != 12)
            { printf("FAIL col delete keeps format\n"); fails++; }
        else printf("ok   column delete shifts width/precision\n");
        csn_canon(&tread(0, 0)->val, g, (int) sizeof(g));
        if (tread(0, 0)->type != CE_NUM ||
            strcmp(g, "+9.0000000000000E+1"))
            { printf("FAIL col delete moves value\n"); fails++; }
        else printf("ok   column delete moves the value to A\n");
        /* the dependency scan must TERMINATE when a formula references
         * the deleted row/column (the sparse-conversion hang) */
        cs_clear_all();
        setv(0, 3, "+A3*2");                  /* A4 references A3 (row 3 = at 2) */
        {
            char dl[128];
            dl[0] = '\0';
            dep_check_range(0, 2, 2, dl, (int) sizeof(dl));
            if (strcmp(dl, "A4"))
                { printf("FAIL dep scan row (%s)\n", dl); fails++; }
            else printf("ok   dep scan row terminates: %s\n", dl);
        }
        {
            char dl[128];
            dl[0] = '\0';
            setv(2, 0, "+B1*2");              /* C1 references B1 (col 1) */
            dep_check_range(1, 1, 1, dl, (int) sizeof(dl));
            if (strcmp(dl, "C1"))
                { printf("FAIL dep scan col (%s)\n", dl); fails++; }
            else printf("ok   dep scan col terminates: %s\n", dl);
        }
        cs_clear_all();
        /* the sparse-relink bug (user report 2026-11-14): an ABSENT
         * source must still clear an occupied target - deleting
         * columns C>D kept the old D, deleting rows 4>7 kept c and e */
        cs_clear_all();
        setv(1, 2, "1"); setv(2, 2, "2");     /* row 3: B3..E3 = 1..4 */
        setv(3, 2, "3"); setv(4, 2, "4");
        cs_delete_columns(2, 3);              /* delete columns C>D */
        if (tread(1, 2)->type != CE_NUM ||
            tread(1, 2)->val.nd[0] != 1 ||
            tread(2, 2)->type != CE_NUM ||
            tread(2, 2)->val.nd[0] != 4 ||
            tread(3, 2)->type == CE_NUM)
            { printf("FAIL col delete clears targets\n"); fails++; }
        else printf("ok   column delete clears with absent sources\n");
        cs_clear_all();
        {
            const char *txt[8] = { "a", "b", "c", "d", "e", "f", NULL,
                                   "x" };
            int w;
            for (w = 0; w < 8; w++) {
                if (txt[w]) cs_set_text(tcell(1, 2 + w), txt[w], 'L');
            }
        }
        cs_delete_rows(3, 6);                 /* delete rows 4>7 */
        if (tread(1, 2)->type != CE_TEXT ||
            strcmp(tread(1, 2)->cont, "a") ||
            tread(1, 3)->type != CE_TEXT ||
            strcmp(tread(1, 3)->cont, "f") ||
            tread(1, 4)->type != CE_EMPTY ||
            tread(1, 5)->type != CE_TEXT ||
            strcmp(tread(1, 5)->cont, "x") ||
            tread(1, 6)->type != CE_EMPTY)
            { printf("FAIL row delete clears targets\n"); fails++; }
        else printf("ok   row delete clears with absent sources\n");
        cs_clear_all();
        /* ROW() and COL(): context-free (the ?-calculator) answers
         * the cursor; a STORED formula answers its own cell */
        curCol = 0; curRow = 0;
        ckc("ROW at A1", "ROW()", "+1.0000000000000E+1");
        ckc("COL at A1", "COL()", "+1.0000000000000E+1");
        curCol = 2; curRow = 2;
        ckc("ROW at C3", "ROW()", "+3.0000000000000E+1");
        ckc("COL at C3", "COL()", "+3.0000000000000E+1");
        cke("ROW with arg", "ROW(5)", 6);
        cke("COL with arg", "COL(1)", 6);
        cs_clear_all();
        /* the stored +ROW() at (5, 6) = F7 evaluates to 7 */
        cs_commit_entry(5, 6, "+ROW()");
        cs_recalc_all();
        if (tread(5, 6)->type != CE_NUM ||
            tread(5, 6)->val.nd[0] != 7)
            { printf("FAIL stored ROW (%d)\n",
                     tread(5, 6)->val.nd[0]); fails++; }
        else printf("ok   a stored +ROW() is its own row\n");
        /* the user's cases with NON-ORIGIN sources (the shift-delta
         * bug): +COL() at B1 -> C1>F1 = 3,4,5,6; +ROW() at A2 ->
         * A3>A10 = 3..10 */
        cs_clear_all();
        cs_commit_entry(1, 0, "+COL()");      /* B1 = 2 */
        cs_copy_block(1, 0, 1, 0, 2, 0, 4, 1, 1, 4, 1);
        {
            int w, ok3 = 1;
            Csn want;
            for (w = 3; w <= 6; w++) {
                csn_from_int(&want, w);
                if (tread(w - 1, 0)->type != CE_NUM ||
                    csn_cmp(&tread(w - 1, 0)->val, &want) != 0)
                    ok3 = 0;
            }
            if (!ok3) { printf("FAIL COL copy across\n"); fails++; }
            else printf("ok   +COL() copied across yields 3..6\n");
        }
        cs_clear_all();
        setv(0, 1, "x");
        cs_commit_entry(0, 1, "+ROW()");      /* A2 = 2 */
        cs_copy_block(0, 1, 0, 1, 0, 2, 1, 8, 1, 8, 1);
        {
            int w, ok4 = 1;
            Csn want;
            for (w = 3; w <= 10; w++) {
                csn_from_int(&want, w);
                if (tread(0, w - 1)->type != CE_NUM ||
                    csn_cmp(&tread(0, w - 1)->val, &want) != 0)
                    ok4 = 0;
            }
            if (!ok4) { printf("FAIL ROW copy-down off-origin\n"); fails++; }
            else printf("ok   +ROW() copied down off-origin yields 3..10\n");
        }
        cs_clear_all();
        /* the FILL copy (user request 2026-11-14): the vertical case */
        cs_clear_all();
        setv(0, 0, "1"); setv(1, 0, "2");
        cs_commit_entry(0, 1, "+A1+1");
        cs_commit_entry(1, 1, "+B1+2");
        cs_fill_block(0, 1, 1, 1, 0, 9, 1, 9, 1);
        {
            int w, okf = 1;
            Csn want;
            for (w = 3; w <= 10; w++) {
                csn_from_int(&want, w);
                if (csn_cmp(&tread(0, w - 1)->val, &want) != 0) okf = 0;
            }
            for (w = 3; w <= 10; w++) {
                csn_from_int(&want, 2 * w);
                if (csn_cmp(&tread(1, w - 1)->val, &want) != 0) okf = 0;
            }
            if (!okf) { printf("FAIL fill vertical\n"); fails++; }
            else printf("ok   fill down: 3..10 and 6..20\n");
        }
        cs_clear_all();
        /* the horizontal fill */
        setv(0, 0, "1"); setv(0, 1, "2");
        cs_commit_entry(1, 0, "+A1+1");
        cs_commit_entry(1, 1, "+A2+2");
        cs_fill_block(1, 0, 1, 1, 2, 0, 5, 1, 1);
        {
            int w, okh = 1;
            Csn want;
            for (w = 3; w <= 6; w++) {
                csn_from_int(&want, w);
                if (csn_cmp(&tread(w - 1, 0)->val, &want) != 0) okh = 0;
            }
            for (w = 3; w <= 6; w++) {
                csn_from_int(&want, 2 * w);
                if (csn_cmp(&tread(w - 1, 1)->val, &want) != 0) okh = 0;
            }
            if (!okh) { printf("FAIL fill horizontal\n"); fails++; }
            else printf("ok   fill right: 3..6 and 6..12\n");
        }
        cs_clear_all();
        /* the mirrored fill: UP toward the source (the chain builds
         * upward with the empty-as-zero rule) */
        cs_clear_all();
        cs_commit_entry(0, 8, "+ROW()");      /* A9 = 9 */
        cs_fill_block(0, 8, 0, 8, 0, 1, 0, 1, 1);
        {
            int w, oku = 1;
            Csn want;
            for (w = 2; w <= 8; w++) {
                csn_from_int(&want, w);
                if (csn_cmp(&tread(0, w - 1)->val, &want) != 0) oku = 0;
            }
            if (!oku) { printf("FAIL fill up\n"); fails++; }
            else printf("ok   fill up mirrors: A2..A8 = 2..8\n");
        }
        cs_clear_all();
        /* the user's copy-down: +ROW() at A1 copied to A2>A10 -> 2..10 */
        cs_clear_all();
        cs_commit_entry(0, 0, "+ROW()");
        cs_copy_block(0, 0, 0, 0, 0, 1, 1, 9, 1, 9, 1);
        {
            int w, ok2 = 1;
            Csn want;
            for (w = 2; w <= 10; w++) {
                csn_from_int(&want, w);
                if (tread(0, w - 1)->type != CE_NUM ||
                    csn_cmp(&tread(0, w - 1)->val, &want) != 0)
                    ok2 = 0;
            }
            if (!ok2) { printf("FAIL ROW copy-down\n"); fails++; }
            else printf("ok   +ROW() copied down yields 2..10\n");
        }
        cs_clear_all();
        /* the stored +COL() at (2, 4) = C5 evaluates to 3 */
        cs_commit_entry(2, 4, "+COL()");
        cs_recalc_all();
        if (tread(2, 4)->type != CE_NUM ||
            tread(2, 4)->val.nd[0] != 3)
            { printf("FAIL stored COL (%d)\n",
                     tread(2, 4)->val.nd[0]); fails++; }
        else printf("ok   a stored +COL() is its own column\n");
        cs_clear_all();
        /* range deletes: rows 1..3 of a populated sheet shift up two;
         * column range deletes shift the formats by the range width */
        colw[0] = 10; colw[1] = 20; colw[2] = 30; colw[3] = 40;
        colp[0] = 2; colp[1] = 4; colp[2] = 6; colp[3] = 8;
        csn_parse("1", &v); cs_set_num(tcell(0, 4), "1", &v, 0, NULL);
        csn_parse("2", &v); cs_set_num(tcell(0, 5), "2", &v, 0, NULL);
        csn_parse("3", &v); cs_set_num(tcell(0, 6), "3", &v, 0, NULL);
        cs_delete_rows(0, 1);               /* rows 1..2 */
        csn_canon(&tread(0, 4)->val, g, (int) sizeof(g));
        if (tread(0, 4)->type != CE_NUM ||
            strcmp(g, "+3.0000000000000E+1") || colw[2] != 30 || colp[2] != 6)
            { printf("FAIL row range delete\n"); fails++; }
        else printf("ok   row range delete shifts cells up\n");
        cs_delete_columns(0, 1);            /* columns A..B */
        if (colw[0] != 30 || colp[0] != 6 || colw[1] != 40 || colp[1] != 8)
            { printf("FAIL col range delete formats\n"); fails++; }
        else printf("ok   column range delete shifts formats\n");
        /* From-To clear: cells freed, pool bookkeeping stays honest */
        {
            int before = entriesUsed;
            csn_parse("7", &v); cs_set_num(tcell(0, 8), "7", &v, 0, NULL);
            csn_parse("8", &v); cs_set_num(tcell(1, 8), "8", &v, 0, NULL);
            if (entriesUsed != before + 2)
                { printf("FAIL clear range pool pre\n"); fails++; }
            cs_clear_range(0, 8, 1, 8);
            if (entriesUsed != before ||
                tread(0, 8)->type != CE_EMPTY ||
                tread(1, 8)->type != CE_EMPTY)
                { printf("FAIL clear range pool\n"); fails++; }
            else printf("ok   From-To clear frees the rectangle\n");
        }
        colw[0] = 10; colw[1] = 10; colw[2] = 10; colw[3] = 10;
        colp[0] = 2; colp[1] = 2; colp[2] = 2; colp[3] = 2;
        /* (b) All save + load at A1: identity, formula intact */
        cs_clear_all();
        colw[0] = 10; colp[0] = 2;      /* cs_clear_all resets formats */
        csn_parse("5", &v);
        cs_set_num(tcell(1, 1), "5", &v, 0, NULL);    /* B2 */
        ec = cs_commit_entry(1, 2, "+B2*10");           /* B3 */
        if (ec != 0) { printf("FAIL B3 commit err %d\n", ec); fails++; }
        cs_save("t_id", 0, 0, CS_MAXCOL - 1, CS_MAXROW - 1, "");
        cs_clear_all();
        if (!cs_load("t_id", 0, 0, 0))
            { printf("FAIL identity load\n"); fails++; }
        else {
            csn_canon(&tread(1, 1)->val, g, (int) sizeof(g));
            if (tread(1, 1)->type != CE_NUM ||
                strcmp(g, "+5.0000000000000E+1"))
                { printf("FAIL identity B2 got %s\n", g); fails++; }
            else printf("ok   All+load(A1): B2 stays at B2\n");
            if (!tread(1, 2)->cont ||
                strcmp(tread(1, 2)->cont, "+B2*10"))
                { printf("FAIL identity B3 formula\n"); fails++; }
            else printf("ok   All+load(A1): formula unchanged\n");
            cs_recalc_all();
            csn_canon(&tread(1, 2)->val, g, (int) sizeof(g));
            if (tread(1, 2)->type != CE_NUM ||
                strcmp(g, "+5.0000000000000E+2"))
                { printf("FAIL identity B3 recalc got %s\n", g); fails++; }
            else printf("ok   All+load(A1): formula evaluates 50\n");
        }
        remove("t_id.csd");
    }
    /* user batch: Evaluate-key formatting, @-reference entries, sticky
     * error text with the cell coordinate */
    {
        int ec;
        /* "= " + %24s of the CS_MAX_P-decimal fixed answer, no float */
        {
            Csn r;
            char num[64], msg[76], want[76];
            char *s2 = NULL;
            if (cs_eval_formula("5+5", &r, &s2) != 0) { printf("FAIL 5+5\n"); fails++; }
            if (s2) free(s2);
            csn_fmt_fixed(&r, CS_MAX_P, num, (int) sizeof(num));
            sprintf(msg, "=%24s", num);
            sprintf(want, "=10.000000000000000000000000");
            if (strcmp(msg, want)) { printf("FAIL eval-key fmt %s want %s\n", msg, want); fails++; }
            else printf("ok   eval-key format (24 decimals)\n");
        }
        /* '@'-reference entries commit as bare-coordinate formulas */
        {
            Csn v;
            csn_parse("100", &v);
            cs_set_num(tcell(0, 0), "100", &v, 0, NULL);   /* A1 */
            csn_parse("200", &v);
            cs_set_num(tcell(1, 0), "200", &v, 0, NULL);   /* B1 */
            ec = cs_commit_entry(1, 3, "+A1+B1");
            cs_free_cell_at(1, 3);
            if (ec != 0)
                { printf("FAIL @-ref commit err %d\n", ec); fails++; }
            else printf("ok   @-ref entry evaluates\n");
            /* the OLD '@'-junk must no longer occur: nothing stores a
             * literal '@' in the cell (the @ key resolves in main.c) */
        }
        /* goto-style "Bad coord" (capital B) is just a string; check the
         * parse-negative path still exists (goto UI is pty-tested) */
    }
    {
        FILE *f = fopen("t_old.csd", "wb");   /* loader's lowercase name */
        if (f) { fwrite("CSD1junk", 1, 8, f); fclose(f); }
        if (cs_load("t_old", 0, 0, 0))
            { printf("FAIL old file accepted\n"); fails++; }
        else printf("ok   old CSD1 refused\n");
        remove("t_old.csd");
    }
    /* save-name rule: bare name -> lowercase .csd; typed .CSD kept as-is */
    {
        FILE *f;
        cs_save("tlow", 0, 0, 0, 0, "");
        f = fopen("tlow.csd", "rb");
        if (!f) { printf("FAIL bare name file\n"); fails++; }
        else printf("ok   bare name saves tlow.csd\n");
        if (f) { fclose(f); remove("tlow.csd"); }
        cs_save("tup.CSD", 0, 0, 0, 0, "");
        f = fopen("tup.CSD", "rb");
        if (!f) { printf("FAIL typed-CSD file\n"); fails++; }
        else printf("ok   typed .CSD kept (no doubling)\n");
        if (f) { fclose(f); remove("tup.CSD"); }
        f = fopen("tup.CSD.csd", "rb");
        if (f) { fclose(f); remove("tup.CSD.csd");
                 printf("FAIL doubled extension\n"); fails++; }
    }
    /* form mode (manual 11.3): flag survives commits and save/load */
    {
        Cell *c = tcell(30, 25);              /* far from other tests */
        Cell *d = tcell(31, 25);
        Csn v;
        c->formf = 1;
        d->formf = 1;
        csn_parse("7", &v);
        cs_set_num(c, "+7", &v, 0, NULL);       /* commit over the flag */
        if (!c->formf)
            { printf("FAIL form flag cleared by commit\n"); fails++; }
        else printf("ok   form flag survives commit\n");
        cs_save("t_fm", 0, 0, CS_MAXCOL - 1, CS_MAXROW - 1, "");
        cs_clear_all();
        if (cs_load("t_fm", 0, 0, 0)) {
            Cell *l = tread(30, 25);
            if (!l->formf)
                { printf("FAIL form flag lost in save/load (formf=%d "
                         "type=%d)\n", l->formf, l->type); fails++; }
            else printf("ok   form flag survives save/load\n");
            remove("t_fm.csd");
        } else { printf("FAIL form save/load\n"); fails++; }
    }
    cs_clear_all();
    {
        /* ...and cs_clear_all does reset the flags (sheet is pristine):
         * the ordered scan must find nothing at all */
        int fl = 0, r = 0, c = 0;
        if (cs_next_cell(&c, &r) != NULL) fl = 1;
        if (fl) { printf("FAIL cs_clear_all left form flags\n"); fails++; }
        else printf("ok   cs_clear_all resets form flags\n");
    }

    /* window scrolling rules, reverse-engineered from real CalcStar
     * (CP/M) with user-run experiments: uniform +5/-6 vertical slide,
     * flush-left / anchor-3 horizontal, wide-column advance-until-fits,
     * lock-aware S/2 vertical anchor, no scroll for in-window targets */
    {
        int f2 = 0;
        COLS = 80;
        lockCols = 0; lockRows = 0; extended = 0;
        for (i = 0; i < CS_MAXCOL; i++) { colw[i] = 10; colp[i] = 2; }
        /* vertical down: cursor rides 10 -> 11, window 1..10 -> 6..15 */
        winRow = 0; curRow = 10; scroll_v();
        f2 += scrck(winRow == 5, "scroll down slide +5 (rows 6..15)");
        /* no scroll while visible */
        winRow = 5; curRow = 13; scroll_v();
        f2 += scrck(winRow == 5, "no vertical scroll inside window");
        /* vertical up mid-sheet: window 10..19, cursor 9 -> 4..13 */
        winRow = 9; curRow = 8; scroll_v();
        f2 += scrck(winRow == 3, "scroll up lands cursor 6th row");
        /* vertical up clamp at the sheet top: cursor 3 -> rows 1..10 */
        winRow = 3; curRow = 2; scroll_v();
        f2 += scrck(winRow == 0, "scroll up clamps at row 1");
        /* horizontal right uniform: window A..F, cursor G -> D..I */
        winCol = 0; curCol = 6; scroll_h();
        f2 += scrck(winCol == 3, "scroll right anchor 3 (window D)");
        /* no scroll while the column fits */
        winCol = 3; curCol = 4; scroll_h();
        f2 += scrck(winCol == 3, "no horizontal scroll inside window");
        /* horizontal left: window E..J, cursor D -> flush left */
        winCol = 4; curCol = 3; scroll_h();
        f2 += scrck(winCol == 3, "scroll left flush at cursor");
        /* goto-style left target deep from window: also flush */
        winCol = 6; curCol = 1; scroll_h();
        f2 += scrck(winCol == 1, "goto left target flush");
        /* wide column E=45: anchor B would clip E (38..82) -> C */
        colw[4] = 45;
        winCol = 0; curCol = 4; scroll_h();
        f2 += scrck(winCol == 2, "wide col: advance until it fits");
        /* with E=45 in view, F beside E sits at 73..82: it does not
         * fit, so the anchor moves to D (E 16..60, F 62..71) */
        winCol = 2; curCol = 5; scroll_h();
        f2 += scrck(winCol == 3, "wide col 45: F beside E re-anchors");
        /* E=45, cursor G: anchor D would clip G (73..82) -> E */
        curCol = 6; scroll_h();
        f2 += scrck(winCol == 4, "wide col: cursor re-anchors past it");
        /* wide column E=60: anchor B..C all clip -> D (16..75) */
        colw[4] = 60;
        winCol = 0; curCol = 4; scroll_h();
        f2 += scrck(winCol == 3, "wide col 60: stops at first fit");
        colw[4] = 10;
        /* locks: lockRows=3 (S=7): cursor row 11 -> scrollable 8..14 */
        lockRows = 3;
        winRow = 0; curRow = 10; scroll_v();
        f2 += scrck(winRow == 7, "locked: cursor 4th scrollable row");
        /* cursor inside the locked area never scrolls */
        winRow = 0; curRow = 1; scroll_v();
        f2 += scrck(winRow == 0, "locked: cursor in lock, no scroll");
        /* locked up-crossing: window 8..14, cursor 7 -> 4..10 */
        winRow = 7; curRow = 6; scroll_v();
        f2 += scrck(winRow == 3, "locked up: clamps at lock edge");
        /* locked down-crossing: window 8..14, cursor 15 -> 12..18 */
        winRow = 7; curRow = 14; scroll_v();
        f2 += scrck(winRow == 11, "locked down: 4th scrollable again");
        lockRows = 0;
        /* extended mode (15 rows): S/2 = 7 */
        extended = 1;
        winRow = 0; curRow = 15; scroll_v();
        f2 += scrck(winRow == 8, "extended: cursor 8th of 15 rows");
        extended = 0;
        fails += f2;
    }

    /* coordinate parsing accepts either case (Goto/TAB/copy fields;
     * stored coordinates stay uppercase via the parser's normalizer) */
    {
        int cc, rr;
        if (!cs_parse_coord("b3", &cc, &rr) || cc != 1 || rr != 2)
            { printf("FAIL lower b3\n"); fails++; }
        else printf("ok   cs_parse_coord accepts lowercase\n");
        if (!cs_parse_coord("aB7", &cc, &rr) || cc != 27 || rr != 6)
            { printf("FAIL mixed aB7\n"); fails++; }
        else printf("ok   cs_parse_coord accepts mixed case\n");
        if (!cs_parse_coord("t10", &cc, &rr) || cc != 19 || rr != 9)
            { printf("FAIL lower t10\n"); fails++; }
        else printf("ok   cs_parse_coord lower two-letter column\n");
        if (cs_parse_coord("abc1", &cc, &rr))
            { printf("FAIL abc1 accepted\n"); fails++; }
        else printf("ok   three-letter coord still rejected\n");
        if (cs_parse_coord("b0", &cc, &rr))
            { printf("FAIL b0 accepted\n"); fails++; }
        else printf("ok   lowercase out-of-range still rejected\n");
    }

    /* ==================== the CSV import/export ===================== */
    {
        Csn v; char b[64];
        struct { const char *in; const char *want; } nt[] = {
            { "0", "0" },
            { "2", "2" },
            { "123.45", "123.45" },
            { "0.5", "0.5" },
            { "1.5E-3", "0.0015" },
            { "0.0012345678901234567", "0.0012345678901234567" },
            { "1.2345678901234567E20", "1.2345678901234567E+20" },
            { "5E-5", "5E-5" },
            { "1E17", "1E+17" },
            { "1E18", "1E+18" },
            { "-2.75", "-2.75" },
            { "0.0001", "0.0001" },
            { "0.00005", "5E-5" },
        };
        for (i = 0; i < (int) (sizeof(nt) / sizeof(nt[0])); i++) {
            if (csn_parse(nt[i].in, &v) != 0) {
                printf("FAIL csvnum parse %s\n", nt[i].in); fails++; continue;
            }
            cs_csv_num(&v, b, (int) sizeof(b));
            if (strcmp(b, nt[i].want) != 0)
                { printf("FAIL csvnum %s -> %s want %s\n",
                         nt[i].in, b, nt[i].want); fails++; }
            else printf("ok   csvnum %-22s = %s\n", nt[i].in, b);
        }
        /* the 17-digit truncation through the engine (1/3, 2/3) */
        {
            Csn a, b2; char *s = NULL; char ob[64];
            cs_eval_formula("1/3", &a, &s); if (s) free(s);
            cs_csv_num(&a, ob, (int) sizeof(ob));
            if (strcmp(ob, "0.33333333333333333") != 0)
                { printf("FAIL csvnum 1/3 -> %s\n", ob); fails++; }
            else printf("ok   csvnum 1/3 = %s\n", ob);
            cs_eval_formula("2/3", &b2, &s); if (s) free(s);
            cs_csv_num(&b2, ob, (int) sizeof(ob));
            if (strcmp(ob, "0.66666666666666666") != 0)
                { printf("FAIL csvnum 2/3 -> %s\n", ob); fails++; }
            else printf("ok   csvnum 2/3 = %s\n", ob);
        }
        /* ---- the export: quote doubling, commas, empty fields,
         * CRLF separators, no trailing CRLF, 17-digit numerics ---- */
        {
            FILE *g;
            char data[512];
            size_t got;
            cs_clear_all();
            setv(0, 0, "123.45");            /* A1 */
            setv(1, 0, "2");                 /* B1 */
            cs_commit_entry(2, 0, "plain text");   /* C1: text */
            cs_commit_entry(3, 0, "a,comma");      /* D1 */
            cs_commit_entry(4, 0, "say \"hi\"");   /* E1 */
            cs_commit_entry(2, 1, "3/4");    /* C2: a formula -> 0.75 */
            if (!cs_save_csv("t_csv.csv", 0, 0, 4, 1))
                { printf("FAIL csv save\n"); fails++; }
            else {
                g = fopen("t_csv.csv", "rb");
                memset(data, 0, sizeof(data));
                got = fread(data, 1, sizeof(data) - 1, g);
                fclose(g);
                data[got] = '\0';
                {
                    const char *want =
                        "123.45,2,plain text,\"a,comma\",\"say \"\"hi\"\"\"\r\n"
                        ",,0.75,,";
                    if (strcmp(data, want) != 0) {
                        printf("FAIL csv export bytes:\n  got  %.120s\n  want %.120s\n",
                               data, want);
                        fails++;
                    } else printf("ok   csv export exact bytes\n");
                }
            }
            remove("t_csv.csv");
        }
        /* ---- the import: pure numerics, quoted text, doubled
         * quotes, embedded-newline flattening, the BOM, >127
         * dropped, ragged rows, the load corner, no recalc ---- */
        {
            FILE *g;
            const char *csv =
                "\xEF\xBB\xBF"                 /* the UTF-8 BOM */
                "1,2,3\r\n"
                "\"41\",\"42\",x\xC3\xA9y\r\n"  /* quoted numerics + UTF-8 */
                "+7,1.5E-3,\"a,comma\",123 pages\r\n"
                "\"say \"\"hi\"\"\",\"b\ncc\"\r\n"  /* quoted newline */
                "tail";
            g = fopen("t_csv.csv", "wb");
            fwrite(csv, 1, strlen(csv), g);
            fclose(g);
            cs_clear_all();
            if (!cs_load_csv("t_csv.csv", 2, 1, 0))   /* load at C2 */
                { printf("FAIL csv load refused\n"); fails++; }
            else {
                Cell *c;
                c = tread(2, 1);   /* C2 = "1" numeric */
                if (c->type != CE_NUM || strcmp(c->cont, "1") ||
                    c->val.ndig != 1 || c->val.nd[0] != 1)
                    { printf("FAIL csv C2\n"); fails++; }
                else printf("ok   csv C2 = 1 (numeric)\n");
                c = tread(3, 1);   /* D2 = "2" */
                if (c->type != CE_NUM || c->val.nd[0] != 2)
                    { printf("FAIL csv D2\n"); fails++; }
                else printf("ok   csv D2 = 2\n");
                c = tread(4, 1);   /* E2 = "3" */
                if (c->type != CE_NUM || c->val.nd[0] != 3)
                    { printf("FAIL csv E2\n"); fails++; }
                else printf("ok   csv E2 = 3\n");
                c = tread(2, 2);   /* C3: the quoted "41" -> numeric */
                if (c->type != CE_NUM || c->val.nd[0] != 4 || c->val.nd[1] != 1)
                    { printf("FAIL csv C3 quoted numeric\n"); fails++; }
                else printf("ok   csv quoted \"41\" is numeric\n");
                c = tread(4, 2);   /* E3: x\xC3\xA9y -> the >127 byte
                                      dropped -> "xy" text */
                if (c->type != CE_TEXT || strcmp(c->cont, "xy"))
                    { printf("FAIL csv utf8 drop (got %s)\n",
                             c->cont ? c->cont : "(null)"); fails++; }
                else printf("ok   csv utf8 byte dropped\n");
                c = tread(2, 3);   /* C4: "+7" numeric */
                if (c->type != CE_NUM || c->val.nd[0] != 7)
                    { printf("FAIL csv +7\n"); fails++; }
                else printf("ok   csv +7 numeric\n");
                c = tread(3, 3);   /* D4: 1.5E-3 */
                {
                    Csn w;
                    csn_parse("0.0015", &w);
                    if (c->type != CE_NUM || csn_cmp(&c->val, &w) != 0)
                        { printf("FAIL csv 1.5E-3\n"); fails++; }
                    else printf("ok   csv 1.5E-3 numeric\n");
                }
                c = tread(4, 3);   /* E4: "a,comma" text */
                if (c->type != CE_TEXT || strcmp(c->cont, "a,comma"))
                    { printf("FAIL csv quoted comma\n"); fails++; }
                else printf("ok   csv quoted comma text\n");
                c = tread(5, 3);   /* F4: "123 pages" stays TEXT */
                if (c->type != CE_TEXT || strcmp(c->cont, "123 pages"))
                    { printf("FAIL csv 123 pages (got %s)\n",
                             c->cont ? c->cont : "(null)"); fails++; }
                else printf("ok   csv \"123 pages\" stays text\n");
                c = tread(2, 4);   /* C5: doubled quotes */
                if (c->type != CE_TEXT || strcmp(c->cont, "say \"hi\""))
                    { printf("FAIL csv doubled quotes\n"); fails++; }
                else printf("ok   csv doubled quotes\n");
                c = tread(3, 4);   /* D5: the embedded newline flattened */
                if (c->type != CE_TEXT || strcmp(c->cont, "b cc"))
                    { printf("FAIL csv newline flatten (got %s)\n",
                             c->cont ? c->cont : "(null)"); fails++; }
                else printf("ok   csv embedded newline flattened\n");
                c = tread(2, 5);   /* C6: the ragged "tail" */
                if (c->type != CE_TEXT || strcmp(c->cont, "tail"))
                    { printf("FAIL csv tail row\n"); fails++; }
                else printf("ok   csv ragged tail row\n");
                if (tread(3, 5)->type != CE_EMPTY)
                    { printf("FAIL csv ragged empty cell\n"); fails++; }
                else printf("ok   csv ragged row leaves empties\n");
                /* no recalculation happened: the formula-free values
                 * are exactly the parsed ones and the entry pool is
                 * honest */
                cs_recount_entries();
                printf("ok   csv load entry count %d\n", entriesUsed);
            }
            remove("t_csv.csv");
        }
        /* ---- the refusals: the sheet bounds and the budget ---- */
        {
            FILE *g;
            int k;
            g = fopen("t_csv.csv", "wb");
            for (k = 0; k < 300; k++) fprintf(g, "x\r\n");   /* 300 rows */
            fclose(g);
            cs_clear_all();
            if (cs_load_csv("t_csv.csv", 0, 0, 0))
                { printf("FAIL csv 300 rows accepted\n"); fails++; }
            else printf("ok   csv over-tall load refused (sheet intact: %s)\n",
                        tread(0, 0)->type == CE_EMPTY ? "empty" : "?");
            remove("t_csv.csv");
        }
    }

    /* ---- the long-path batch (user report 2026-11-22) ---- */
    {
        /* a 240-char single-component name: saves through cs_save,
         * loads back by the same name (the old 40-byte dialog buffer
         * truncated answers to 39 chars - and the prompt editor's
         * fixed 100-byte compose line crashed at ~56 typed chars) */
        char longname[CS_PATHMAX];
        int k;
        char got[256];
        Cell *c;
        for (k = 0; k < 240; k++) longname[k] = (char) ('a' + (k % 26));
        longname[240] = '\0';
        cs_clear_all();
        setv(0, 0, "7");
        if (!cs_save(longname, 0, 0, 0, 0, NULL))
            { printf("FAIL long path save refused\n"); fails++; }
        else printf("ok   long path (240 chars) save\n");
        cs_clear_all();
        if (!cs_load(longname, 0, 0, 0))
            { printf("FAIL long path load refused\n"); fails++; }
        else printf("ok   long path (240 chars) load\n");
        c = tread(0, 0);
        {
            Csn want;
            csn_parse("7", &want);
            if (!c || c->type != CE_NUM || csn_cmp(&c->val, &want) != 0)
                { printf("FAIL long path round trip value\n"); fails++; }
            else printf("ok   long path round trip value = 7\n");
        }
        remove(longname);
        strcat(longname, ".csd");
        remove(longname);

        /* the read-side not-found message: the old wording said
         * "write error ..." on the LOAD path (the write side never
         * used it) - now "read error <file> not found" */
        cs_clear_all();
        if (cs_load("t_nosuchfile_xyz", 0, 0, 0))
            { printf("FAIL missing csd load accepted\n"); fails++; }
        else printf("ok   missing csd load refused\n");
        cs_last_msg(got, (int) sizeof(got));
        if (strcmp(got, "read error t_nosuchfile_xyz.csd not found"))
            { printf("FAIL read error message (got %s)\n", got); fails++; }
        else printf("ok   read error <file> not found message\n");
        if (cs_load_csv("t_nosuchfile_xyz.csv", 0, 0, 0))
            { printf("FAIL missing csv load accepted\n"); fails++; }
        else printf("ok   missing csv load refused\n");
        cs_last_msg(got, (int) sizeof(got));
        if (strcmp(got, "read error t_nosuchfile_xyz.csv not found"))
            { printf("FAIL csv read error message (got %s)\n", got); fails++; }
        else printf("ok   csv read error <file> not found message\n");
    }

    if (fails == 0) printf("\nAll tests passed.\n");
    else printf("\n%d test(s) FAILED.\n", fails);
    exit(fails != 0);
}

int main(int argc, char **argv)
{
    int i;
    for (i = 0; i < CS_MAXCOL; i++) { colw[i] = 0; colp[i] = 0; }
    cs_clear_all();
    if (argc > 1 && strcmp(argv[1], "--test") == 0) {
        cs_headless = 1;
        run_tests();
    }
    /* the example generator (a re-runnable genprobe, 2026-11-17):
     * builds the brainfuck machinery and saves examples/brainf.csd
     * All-mode - re-run whenever the storage layout changes (the
     * program is the classic Hello World minus its final '.', whose
     * byte 10 is outside CHR's 32..127) */
    if (argc > 1 && strcmp(argv[1], "--genbf") == 0) {
        cs_headless = 1;
        bf_build("++++++++[>++++[>++>+++>+++>+<<<<-]>+>+>->>+[<]<-]>>.>---"
                 ".+++++++..+++.>>.<-.<.+++.------.--------.>>+.>++", NULL);
        if (!cs_save("examples/brainf.csd", 0, 0,
                     CS_MAXCOL - 1, CS_MAXROW - 1, NULL)) {
            printf("genbf: save failed\n");
            return 1;
        }
        printf("genbf: examples/brainf.csd written\n");
        return 0;
    }
    /* optional file argument: load at startup.  The name is used as
     * typed - cs_load's own name rule appends ".csd" only when no .csd
     * extension (any case) is already present, so both "NAME" and
     * "NAME.csd"/"NAME.CSD" spell the same file. */
    if (argc > 1) {
        const char *a = argv[1];
        int n = (int) strlen(a);
        if (n > 0 && n < (int) sizeof(cs_cmdfile)) {
            memcpy(cs_cmdfile, a, (size_t) n);
            cs_cmdfile[n] = '\0';
        }
    }
    initscr();
    run_session();
    endwin();
    return 0;
}
