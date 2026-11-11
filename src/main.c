/* main.c - CalcStar main loop: curses setup, WordStar-style keys,
   entry engine (@-references, " toggle, ESC cancel, trailing ? calc),
   and a --test self-test mode for the evaluator */
#include <stdlib.h>
#include <string.h>
#include <curses.h>
#include "cs.h"

static void redraw(void) { cs_draw(); }

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
    char cn[4];
    char ref[10];
    cs_col_name(curCol, cn);
    sprintf(ref, "%s%d", cn, curRow + 1);
    g_entryrow = curRow;
    g_entrycol = curCol;
    g_entry_num = 1;
    g_editstate = EDIT_EDITING;
    strcpy(g_edit, ref);
    g_editlen = (int) strlen(ref);
    redraw();
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
    redraw();
}

static void entry_append(int ch)
{
    if (g_editlen < CS_EDITMAX - 1) {
        g_edit[g_editlen++] = (char) ch;
        g_edit[g_editlen] = '\0';
    }
    redraw();
}

/* finish a cell reference entered with '@': uses the cursor position
 * at the moment of resolution (the user may have moved the cursor in
 * "@" mode), e.g. moving to E3 and pressing <CR> or an operator
 * inserts "E3" */
static int at_resolve(void)
{
    char cn[4];
    char ref[10];
    cs_col_name(curCol, cn);
    sprintf(ref, "%s%d", cn, curRow + 1);
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
 * "=         10.000000000000" ('=' plus a %24s right-aligned field of
 * the 12-decimal fixed form) and the edit field clears.  Manual 4.1:
 * every answer has 12 decimal places; no floating point anywhere. */
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
    redraw();
    if (err) {
        cs_set_errpend(1);      /* single message, no queue */
        cs_sticky_err(err, saved, g_entrycol, g_entryrow);
        return;
    }
    if (str) {
        cs_msg_plain("%s", str);
        free(str);
        return;
    }
    csn_fmt_fixed(&val, 12, num, (int) sizeof(num));
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
    if (g_editlen == 0) { edit_clear(); redraw(); return; }
    g_edit[g_editlen] = '\0';
    strcpy(saved, g_edit);
    ec = cs_commit_entry(col, row, g_edit);   /* eval error code, 0 = none */
    c = cs_cell(col, row);
    edit_clear();
    curCol = col;               /* commit lands on the entry cell */
    curRow = row;
    cs_nerrlist = 0;            /* commit errors are a one-message set */
    errq_i = 0;
    if (c && c->type == CE_NUM && c->iserr) {
        errpend = 1;
        redraw();                 /* cells first ... */
        cs_sticky_err(ec, saved, col, row);   /* "... hit space [cell]" */
        return;
    }
    redraw();
}

/* manual 6.4/14.2: caret toggles the interpretation of a cell between
 * text and numeric; `cont` stays exactly as typed, so the toggle is
 * reversible (numeric "1/4" shows as 1/4 text; text "1/4" shows 0.25) */
static void toggle_celltype(void)
{
    Cell *c = cs_cell(curCol, curRow);
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
    redraw();
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
    /* load the file given on the command line, if any */
    if (cs_cmdfile[0]) {
        if (cs_load(cs_cmdfile, 0, 0, 0))
            redraw();            /* repaint the loaded sheet */
        /* on failure cs_msg put the error on the status row; leave it */
    }
    while (running) {
        int ch = getch();
        if (cs_was_resize(ch)) continue;   /* terminal resized */
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
                              cs_cell(cs_errlist[errq_i].col,
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
            move_cursor(0, -1); redraw(); break;
        case 24:                       /* ^X down */
            move_cursor(0, 1); redraw(); break;
        case 19:                       /* ^S left */
            move_cursor(-1, 0); redraw(); break;
        case 4:                        /* ^D right */
            move_cursor(1, 0); redraw(); break;
#ifdef KEY_UP
        case KEY_UP:
            move_cursor(0, -1); redraw(); break;
        case KEY_DOWN:
            move_cursor(0, 1); redraw(); break;
        case KEY_LEFT:
            move_cursor(-1, 0); redraw(); break;
        case KEY_RIGHT:
            move_cursor(1, 0); redraw(); break;
#endif
        case 26:                       /* ^Z col A next row (L-R mode)
                                          or row A next col (T-B mode) */
            if (dirTB) {
                curRow = 0;
                move_cursor(1, 0);
                winRow = 0;
            } else {
                curCol = 0;
                move_cursor(0, 1);
                winCol = 0;
            }
            redraw(); break;
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
                if (dirTB) move_cursor(0, 1);
                else if (curCol >= CS_MAXCOL - 1) { curCol = 0; move_cursor(0, 1); }
                else move_cursor(1, 0);
                redraw();
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
            redraw(); break;
        case 27:                       /* ESC */
            edit_clear();
            cs_prompt_hide();
            redraw(); break;
        case KEY_BACKSPACE: case 8: case 127:
            if (g_editstate == EDIT_EDITING && g_editlen > 0) {
                g_edit[--g_editlen] = '\0';
                if (g_editlen == 0) g_editstate = EDIT_IDLE;
                redraw();
            }
            break;
        case '"':
            /* '"': text-literal quote.  While entering it is just a
             * character (needed for conditional strings); the pending
             * TEXT/NUMERIC toggle lives on '^' (manual 6.4) */
            if (g_editstate == EDIT_EDITING)
                entry_append('"');
            else
                entry_begin('"');
            break;
        case 94:                       /* '^' TEXT/NUMERIC DATA TOGLE (manual 6.4) */
            if (g_editstate == EDIT_EDITING) {
                g_entry_num = !g_entry_num;   /* pending toggle, live on the
                                                 type line via redraw */
                redraw();
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
                if (at_resolve()) redraw();
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
static void setv(int col, int row, const char *txt)
{
    Csn v;
    csn_parse(txt, &v);
    cs_free_cell(cs_cell(col, row));
    cs_set_num(cs_cell(col, row), txt, &v, 0, NULL);
}

/* window-scroll check: 1 on failure (added to `fails` by the caller) */
static int scrck(int cond, const char *what)
{
    if (cond) { printf("ok   %s\n", what); return 0; }
    printf("FAIL %s\n", what);
    return 1;
}

static void run_tests(void)
{
    char buf[64];
    int i;

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
        char t[8];
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

    /* Appendix evaluation-error codes (2nd manual table): the parser's
     * internal codes ARE the manual's; cs_commit_entry returns them for
     * the sticky "Error n-> ..." line (<=0 folds to 0 at the message) */
    cke("err unknown function", "+XYZ(A1)", 4);
    cke("err missing paren", "(1+2", 3);
    cke("err illegal chars end", "1+2)", 7);
    cke("err range not row/col", "SUM(A1>B2)", 2);
    cke("err term expr illegal", "+A", 6);
    cke("err illegal coordinate", "+A99", 1);
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
        cs_free_cell(cs_cell(2, 100));
    }
    /* !n!: "column is not wide enough" (both manuals' entry-error token) */
    {
        Csn w;
        char b[128];
        csn_parse("1234567890.12", &w);
        cs_fmt_num(&w, 3, 2, 0, b, (int) sizeof(b));
        if (strcmp(b, "!n!")) { printf("FAIL !n! display got %s\n", b); fails++; }
        else printf("ok   narrow column displays !n!\n");
    }

    /* conditionals */
    ckc("cond true", "1<2:5:7", "+5.0000000000000E+1");
    ckc("cond false", "1>2:5:7", "+7.0000000000000E+1");
    ckc("empty true-branch", "1<2::9", "0");
    ckc("missing else", "1>2:5", "0");

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

    /* trigonometry (degrees, in and out) */
    ckc("SIN(30)", "SIN(30)", "+5.0000000000000E+0");
    ckc("COS(60)", "COS(60)", "+5.0000000000000E+0");
    ckc("TAN(45)", "TAN(45)", "+1.0000000000000E+1");
    ckc("SIN(90)", "SIN(90)", "+1.0000000000000E+1");
    ckc("COS(90)", "COS(90)", "0");
    ckc("SIN(-30)", "SIN(-30)", "-5.0000000000000E+0");
    ckc("COS(120)", "COS(120)", "-5.0000000000000E+0");
    ckc("SIN(200)", "SIN(200)", "-3.4202014332566E+0");
    ckc("ASIN(0.5)", "ASIN(0.5)", "+3.0000000000000E+2");
    ckc("ACOS(0.5)", "ACOS(0.5)", "+6.0000000000000E+2");
    ckc("ATAN(1)", "ATAN(1)", "+4.5000000000000E+2");
    ckc("ASIN(1)", "ASIN(1)", "+9.0000000000000E+2");
    /* inverse-trig accuracy: the atan-halving reduction and the acos
     * half-angle identity keep the 14-digit floor (round-trip chains
     * produce 9.000000003..., NOT 6.25 as before the fix) */
    ckc("ACOS(-0.5)", "ACOS(-0.5)", "+1.2000000000000E+3");   /* 120 */
    ckc("ACOS(-1)", "ACOS(-1)", "+1.8000000000000E+3");       /* 180 */
    ckc("ACOS(0)", "ACOS(0)", "+9.0000000000000E+2");         /* 90 */
    ckc("ACOS(1)", "ACOS(1)", "0");
    ckc("ACOS(-0.9999)", "ACOS(-0.9999)", "+1.7918970856293E+3");
    /* true value 0.81029143706569954...: the old want (...6570) carried
     * the old round15 carry into digit 14; round30 keeps the true
     * truncation (...6569) */
    ckc("ACOS(0.9999)", "ACOS(0.9999)", "+8.1029143706569E+0");
    ckc("ACOS(0.9999976)", "ACOS(0.9999976)",
        "+1.2552878866239E+0");
    {
        Csn r;
        char g[48];
        char *s2 = NULL;
        int e = cs_eval_formula(
            "+ASIN(ACOS(ATAN(TAN(COS(SIN(9))))))", &r, &s2);
        if (s2) free(s2);
        csn_canon(&r, g, (int) sizeof(g));
        /* 40-digit mantissa + round30: the chain lands on 9 through
         * 23 decimals (9.0000000000000000000000028611); the canon
         * floor is clean.  The pre-raise engine stopped at
         * 9.0000000036813 (round15) */
        if (e != 0 || strcmp(g, "+9.0000000000000E+1")) {
            printf("FAIL trig round-trip chain e=%d got %s\n", e, g);
            fails++;
        } else printf("ok   trig round-trip chain -> 9.0000000000000\n");
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
        if (e != 0 || strcmp(g, "9.000000000000000000000002"))
            { printf("FAIL chain fixed-24 e=%d got %s\n", e, g); fails++; }
        else printf("ok   chain fixed-24 keeps 23 digits\n");
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
        csn_parse("-60", &t);
        csn_cos(&r, &t);
        csn_canon(&r, g, (int) sizeof(g));
        if (strcmp(g, "+5.0000000000000E+0"))
            { printf("FAIL cos(-60) got %s\n", g); fails++; }
        else printf("ok   cos(-60) = 0.5 (even function)\n");
        csn_parse("-0.9999", &t);
        csn_cos(&r, &t);
        csn_fmt_fixed(&r, 30, g, (int) sizeof(g));
        if (strcmp(g, "0.999847725615063856701212110583"))
            { printf("FAIL cos(-0.9999) got %s\n", g); fails++; }
        else printf("ok   cos(-0.9999) sign + 30 digits\n");
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
    ckc("FACT(5)", "FACT(5)", "+1.2000000000000E+3");
    ckc("FACT(0)", "FACT(0)", "+1.0000000000000E+1");
    { Csn r; char *sf = NULL;
      int e = cs_eval_formula("FACT(1.5)", &r, &sf);
      if (e != -1) { printf("FAIL FACT(1.5) err %d want -1\n", e); fails++; }
      else printf("ok   FACT(1.5) -> Error 0 (?n?)\n");
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
        cs_fmt_num(&v, 10, 2, 0, buf, (int) sizeof(buf));
        ckstr("fmt truncate", buf, "1.99");
        csn_parse("0", &v);
        cs_fmt_num(&v, 10, 2, 1, buf, (int) sizeof(buf));
        ckstr("?n?", buf, "?n?");
        /* narrow column: exponential fallback (manual's example style) */
        csn_parse("1234567.89", &v);
        cs_fmt_num(&v, 8, 2, 0, buf, (int) sizeof(buf));
        ckstr("exp form narrow", buf, "1.23E+6");
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
        c = cs_cell(0, 20);   /* A21 = 2 */
        csn_parse("2", &v2);
        cs_set_num(c, "2", &v2, 0, NULL);
        c = cs_cell(0, 21);   /* A22 = 5 */
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
        cs_set_text(cs_cell(0, 22), "a23 is text", 'L');
        cs_set_text(cs_cell(2, 23), "c24 is text", 'L');
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
        cs_set_num(cs_cell(5, 5), "1/4", &v, 0, NULL);   /* shows 0.25 */
        c = cs_cell(5, 5);
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
        cs_set_text(cs_cell(6, 5), "JUL 20", 'L');
        eu = entriesUsed;      /* toggles themselves never touch the pool */
        curCol = 6; curRow = 5;
        c = cs_cell(6, 5);
        toggle_celltype();
        if (c->type == CE_NUM && c->iserr)
            printf("ok   toggle bad text -> numeric ?n?\n");
        else { printf("FAIL bad toggle type %d iserr %d\n", c->type, c->iserr); fails++; }
        c = cs_cell(6, 5);
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
        cs_set_text(cs_cell(0, 0), "hello", 'L');
        cs_free_cell(cs_cell(1, 0));
        cs_set_text(cs_cell(1, 0), "*----*", 'W');
        csn_zero(&v);
        cs_set_text(cs_cell(0, 1), "rep", 'L');
        cs_set_num(cs_cell(2, 1), "B1", &v, 0, NULL);   /* formula */
        csn_parse("7.25", &v);
        cs_set_num(cs_cell(3, 1), "7.25", &v, 0, NULL);
        colw[2] = 13; colp[2] = 4;
        if (cs_save("t_rt", 0, 0, 5, 5, "")) {
            cs_clear_all();
            colw[2] = 10; colp[2] = 2;
            if (cs_load("t_rt", 0, 0, 0)) {
                cs_recalc_all();
                c = cs_cell(0, 0);
                if (c->type == CE_TEXT && c->cont && !strcmp(c->cont, "hello"))
                    printf("ok   save text\n");
                else { printf("FAIL save text\n"); fails++; }
                c = cs_cell(1, 0);
                if (c->type == CE_TEXT && c->just == 'W' && c->rep &&
                    !strcmp(c->rep, "*----*"))
                    printf("ok   save repeating\n");
                else { printf("FAIL save repeating\n"); fails++; }
                {
                    char g[48];
                    csn_canon(&c->val, g, (int) sizeof(g));
                    c = cs_cell(2, 1);
                    csn_canon(&c->val, g, (int) sizeof(g));
                    if (c->type == CE_NUM && c->cont && !strcmp(c->cont, "B1")
                        && !strcmp(g, "0")
                        && colw[2] == 13 && colp[2] == 4)
                        printf("ok   save formula + cols\n");
                    else { printf("FAIL save formula + cols\n"); fails++; }
                }
                c = cs_cell(3, 1);
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
    /* Load/Merge position (manual 14-14): cells land at the requested
     * corner; cells running past the sheet edge are parsed and dropped
     * while the file stream stays in sync */
    {
        char g[48];
        Csn v;
        {
            Cell *cc1 = cs_cell(0, 20);
            Cell *cc2 = cs_cell(1, 20);
            csn_parse("11", &v); cs_set_num(cc1, "11", &v, 0, NULL); /* A21 */
            csn_parse("22", &v); cs_set_num(cc2, "22", &v, 0, NULL); /* B21 */
            {
                Cell *cc3 = cs_cell(0, 21);
                csn_parse("33", &v); cs_set_num(cc3, "33", &v, 0, NULL); /* A22 */
            }
        }
        cs_save("t_of", 0, 19, 2, 22, "");
        cs_clear_all();
        /* merge the 3-cell block at E10 */
        if (cs_load("t_of", 4, 9, 1)) {
            char ok1 = 0, ok2 = 0, ok3 = 0;
            {
                char buf[8];
                csn_canon(&cs_cell(4, 9)->val, g, (int) sizeof(g));
                ok1 = (cs_cell(4, 9)->type == CE_NUM &&
                       !strcmp(g, "+1.1000000000000E+2"));
                csn_canon(&cs_cell(5, 9)->val, buf, (int) sizeof(buf));
                ok2 = (cs_cell(5, 9)->type == CE_NUM);
                ok3 = (cs_cell(4, 10)->type == CE_NUM);
            }
            if (!ok1 || !ok2 || !ok3)
                { printf("FAIL load-offset cells\n"); fails++; }
            else printf("ok   load position offsets cells (E10/F10/E11)\n");
            if (cs_cell(0, 0)->type != CE_EMPTY || cs_cell(0, 20)->type != CE_EMPTY)
                { printf("FAIL offset leak to origin\n"); fails++; }
            else printf("ok   no cells at original coordinates\n");
            /* edge drop: hold the same file near the corner DW/at255 */
            cs_clear_all();
            if (cs_load("t_of", CS_MAXCOL - 1, CS_MAXROW - 1, 1)) {
                /* B21/B22/A22 land outside; the stream must stay in sync
                 * - reload the same file at A1 afterwards and see ALL
                 * three cells */
                cs_clear_all();
                if (cs_load("t_of", 0, 0, 1) &&
                    cs_cell(0, 0)->type == CE_NUM &&
                    cs_cell(1, 0)->type == CE_NUM &&
                    cs_cell(0, 1)->type == CE_NUM) {
                    csn_canon(&cs_cell(0, 0)->val, g, (int) sizeof(g));
                    if (strcmp(g, "+1.1000000000000E+2"))
                        { printf("FAIL edge-drop desync\n"); fails++; }
                    else printf("ok   edge drop keeps stream in sync\n");
                } else { printf("FAIL reload after edge drop\n"); fails++; }
            } else { printf("FAIL edge-drop load\n"); fails++; }
        } else { printf("FAIL offset load\n"); fails++; }
        cs_clear_all();
        remove("t_of.csd");
    }
    /* user batch: Evaluate-key formatting, @-reference entries, sticky
     * error text with the cell coordinate */
    {
        int ec;
        /* "= " + %24s of the 12-decimal fixed answer, no float */
        {
            Csn r;
            char num[64], msg[76], want[76];
            char *s2 = NULL;
            if (cs_eval_formula("5+5", &r, &s2) != 0) { printf("FAIL 5+5\n"); fails++; }
            if (s2) free(s2);
            csn_fmt_fixed(&r, 12, num, (int) sizeof(num));
            sprintf(msg, "=%24s", num);
            sprintf(want, "=         10.000000000000");
            if (strcmp(msg, want)) { printf("FAIL eval-key fmt %s want %s\n", msg, want); fails++; }
            else printf("ok   eval-key format\n");
        }
        /* '@'-reference entries commit as bare-coordinate formulas */
        {
            Csn v;
            csn_parse("100", &v);
            cs_set_num(cs_cell(0, 0), "100", &v, 0, NULL);   /* A1 */
            csn_parse("200", &v);
            cs_set_num(cs_cell(1, 0), "200", &v, 0, NULL);   /* B1 */
            ec = cs_commit_entry(1, 3, "+A1+B1");
            cs_free_cell(cs_cell(1, 3));
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
        Cell *c = cs_cell(30, 25);              /* far from other tests */
        Cell *d = cs_cell(31, 25);
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
            Cell *l = cs_cell(30, 25);
            if (!l->formf)
                { printf("FAIL form flag lost in save/load\n"); fails++; }
            else printf("ok   form flag survives save/load\n");
            remove("t_fm.csd");
        } else { printf("FAIL form save/load\n"); fails++; }
    }
    cs_clear_all();
    {
        /* ...and cs_clear_all does reset the flags (sheet is pristine) */
        int fl = 0, r, c;
        for (r = 0; r < CS_MAXROW; r++)
            for (c = 0; c < CS_MAXCOL; c++)
                if (cells[r][c].formf) fl = 1;
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
