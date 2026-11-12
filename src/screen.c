/* screen.c - CalcStar screen mask rendering and prompt engine.
 *
 * Layout is a faithful clone of a genuine 80x24 CalcStar screen:
 *
 *   lines 1-5   command menu (printed verbatim, no frame)
 *               hidden in extended mode (15-row window)
 *   line 6      "Col>" + column bars at x=4,15,26,... letters at x=5,16,...
 *   line 7      "Row+" + dashes across the data window
 *   lines 8-17  data rows "  1|", " 10|" + cell contents; no column bars,
 *               '>' / '<' markers on the separator positions of the
 *               current cell
 *   line 18     "   +" + dashes (bottom of the data window)
 *   line 19     "[filename ]  cursor:   B3    current:   B3    L-R"
 *   line 20     blank
 *   line 21     "current||       type: numeric"
 *   line 22     "data   ||   contents: 33.00"
 *   line 23     "                edit: " (the entry / prompt / message line)
 *
 * Terminology (screen x coordinates, 0-based):
 *   - column c's cell contents occupy xs[c] .. xs[c]+colw[c]-1
 *     (xs[0]=5, xs[c+1]=xs[c]+colw[c]+1; default 10-wide => bars at 4,15,26..)
 *     a column is drawn only when it fits entirely (no clipped slivers)
 *   - the '|' of a data row label sits at x=3
 *   - everything is clamped to 80 columns even on wide terminals
 */
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <curses.h>
#include "cs.h"

static char lastmsg[80];
static char prompttxt[80];
static int  promptvis = 0;

static void place_cursor(void);

/* the edit / prompt / message line (last layout row) */
#define EDITLINE_X 16     /* "edit: " label column                     */
#define EDITTEXT_X 22     /* where typed text / prompt values start    */

/* data window height in rows: shrinks/grows with the terminal.
 * On a classic 80x24 screen: 10 rows (15 in extended mode).  Taller
 * terminals show more worksheet rows: fixed chrome is 7 rows at the top
 * (menu, column header, row header) + 7 rows at the bottom (border,
 * cursor line, blank, type, contents, edit, prompt) in normal mode, and
 * 2 top rows in extended mode. */
int cs_win_rows(void)
{
    int n;
    if (extended)
        n = (LINES > 24) ? LINES - 9 : 15;
    else
        n = (LINES > 24) ? LINES - 14 : 10;
    return n;
}

/* drawing width: full terminal (COLS).  Bigger terminals simply show
 * more columns; 80x24 is the minimum classic layout. */
int cs_scrw(void)
{
    return COLS;
}

/* geometry (line numbers on screen) */
static int line_colhdr;
static int line_rowhdr;
static int line_rows;
static int line_box;
static int line_cursor;
static int line_type;
static int line_cont;
static int line_edit;
static int line_prompt;   /* last row: command prompts and messages */

void cs_compute_layout(void)
{
    if (extended) {
        line_colhdr = 0;
        line_rowhdr = 1;
    } else {
        line_colhdr = 5;
        line_rowhdr = 6;
    }
    line_rows = line_rowhdr + 1;
    line_box = line_rows + cs_win_rows();
    line_cursor = line_box + 1;
    line_type = line_cursor + 2;   /* one blank line between */
    line_cont = line_type + 1;
    line_edit = line_cont + 1;
    line_prompt = line_edit + 1;   /* very last row of the screen */
}

/* attribute-capable single character (the inverted cell-cursor marks) */
static void cchra(int y, int x, char ch, chtype attr)
{
    if (y >= 0 && y < LINES && x >= 0 && x < cs_scrw())
        mvaddch(y, x, (chtype) (unsigned char) ch | attr);
}

static void cchr(int y, int x, char ch)
{
    cchra(y, x, ch, A_NORMAL);
}

static void cputa(int y, int x, const char *s, chtype attr)
{
    if (y < 0 || y >= LINES) return;
    while (*s && x < cs_scrw()) {
        mvaddch(y, x, (chtype) (unsigned char) *s | attr);
        s++; x++;
    }
}

static void cput(int y, int x, const char *s)
{
    cputa(y, x, s, A_NORMAL);
}

static void cputn(int y, int x, const char *s, int n)
{
    int w = cs_scrw();
    if (y < 0 || y >= LINES) return;
    while (n-- > 0 && x < w) {
        char ch = *s ? *s++ : ' ';
        mvaddch(y, x, (chtype) (unsigned char) ch);
        x++;
    }
}

/* dashes under/over the data window: x=4 .. scrw-3 (x=77 on 80 cols) */
static void dashline(int y, int x)
{
    int end = cs_scrw() - 3;
    for (; x <= end; x++) cchr(y, x, '-');
}

/* left edge of column col's cell contents */
static int cell_x(int col)
{
    int x = 5, i;
    int lim = (col < lockCols) ? col : lockCols;
    for (i = 0; i < lim; i++) x += colw[i] + 1;
    if (col >= lockCols) {
        int fc = winCol >= lockCols ? winCol : lockCols;
        i = fc;
        while (i < col) { x += colw[i] + 1; i++; }
    }
    return x;
}

/* exported for main.c / command.c window scrolling */
int cs_cell_x(int col)
{
    return cell_x(col);
}

/* render one cell into buffer of exactly `width` chars */
static void cell_render(int col, int row, char *out, int width)
{
    Cell *c = cs_cell(col, row);
    out[0] = '\0';
    if (!c || c->type == CE_EMPTY || c->type == CE_ALLOC) return;
    if (c->type == CE_TEXT && !showForm) {
        if (c->just == 'W') {
            int i, j = 0, rl;
            const char *rep = (c->rep && c->rep[0]) ? c->rep : "=";
            rl = (int) strlen(rep);
            for (i = 0; i < width; i++) out[i] = rep[j++ % rl];
            out[width] = '\0';
            return;
        }
        {
            const char *s = c->cont ? c->cont : "";
            int len = (int) strlen(s), pad = 0, k;
            if (len > width) {
                memcpy(out, s, (size_t) width);
                out[width] = '\0';
                return;
            }
            if (c->just == 'R') pad = width - len;
            if (c->just == 'C') pad = (width - len) / 2;
            for (k = 0; k < pad; k++) out[k] = ' ';
            strcpy(out + pad, s);
        }
        return;
    }
    /* numeric */
    if (c->iserr) {
        sprintf(out, "%*s", width, "?n?");
        return;
    }
    if (c->strres) {
        const char *s = c->strres;
        int len = (int) strlen(s);
        if (len > width) { memcpy(out, s, (size_t) width); out[width] = '\0'; }
        else sprintf(out, "%*s", width, s);
        return;
    }
    if (cs_is_inputf(c)) {
        sprintf(out, "%*s", width, "In!");
        return;
    }
    {
        char buf[64];
        int prec = (c->prec >= 0) ? c->prec : colp[col];
        if (showForm && c->cont) {
            const char *s = c->cont;
            int len = (int) strlen(s);
            if (len > width) { memcpy(out, s, (size_t) width); out[width] = '\0'; }
            else sprintf(out, "%*s", width, s);
        } else {
            cs_fmt_num(&c->val, width, prec, 0, buf, (int) sizeof(buf));
            sprintf(out, "%*s", width, buf);
        }
    }
}

/* screen line for data row `row`, or -1 if not visible */
static int row_line(int row)
{
    int lr = lockRows;
    int fr = winRow >= lr ? winRow : lr;
    int y = line_rows;
    if (row < lr) return line_rows + row;
    if (row < fr) return -1;
    y = line_rows + lr + (row - fr);
    return (y - line_rows < cs_win_rows()) ? y : -1;
}

/* top menu: five lines, verbatim from the CalcStar screen mask */
static const char *menu5[] = {
    " -Cursor Movement- |        -Commands-  ; followed by             | -Misc-",
    "  <CR> Right       |A Auto   F Format  M Merge  R Recalc * Extend |@ Curs Pos",
    " ^S Left  ^D Right |C Copy   H Help    O Order  S Save   = Lock   |? Evaluate",
    " ^E Up    ^X Down  |D Delete I Insert  P Print  W What   ? Space  |^ Data Togl",
    " ^Z Col A next row |E Edge   L Load    Q Quit   G or <TAB> Goto   |<ESC>Cancel ",
};

static void draw_menu(void)
{
    int i;
    if (extended) return;
    for (i = 0; i < 5; i++)
        cputn(i, 0, menu5[i], cs_scrw());
}

static void draw_headers(void)
{
    char nambuf[4];
    int c, x;
    int fc = winCol >= lockCols ? winCol : lockCols;
    cput(line_colhdr, 0, "Col>");
    cputn(line_rowhdr, 0, "Row+", 4);
    dashline(line_rowhdr, 4);
    for (c = 0; c < CS_MAXCOL; c++) {
        if (c >= lockCols && c < fc) continue;   /* left of window */
        x = cell_x(c);
        if (x - 1 >= cs_scrw()) break;
        cchr(line_colhdr, x - 1, c < lockCols ? '*' : '|');
        if (x + colw[c] > cs_scrw()) break;   /* no clipped names */
        cs_col_name(c, nambuf);
        cputn(line_colhdr, x, nambuf, 2);
    }
}

static void draw_rows(void)
{
    int i;
    int w = cs_scrw();
    for (i = 0; i < cs_win_rows(); i++) {
        int row, y, c;
        if (i < lockRows) row = i;
        else {
            int fr = winRow >= lockRows ? winRow : lockRows;
            row = fr + (i - lockRows);
            if (row >= CS_MAXROW) break;   /* past the last row: stop */
        }
        y = row_line(row);
        if (y < 0 || y >= LINES) continue;
        /* row label: "  1|", " 10|"; locked rows marked with '*' */
        {
            char lab[8];
            sprintf(lab, "%3d%c", row + 1, row < lockRows ? '*' : '|');
            cputn(y, 0, lab, 4);
        }
        /* cell contents: no column bars on data rows; a column is
         * drawn only when it fits entirely (real CalcStar shows no
         * clipped slivers at the right edge).  Columns LEFT of the
         * window must be SKIPPED: cell_x() positions everything at the
         * screen margin, so rendering them would smear the wide
         * columns' digits into the separator gaps (ghost digits) */
        {
            int fc = winCol >= lockCols ? winCol : lockCols;
            for (c = 0; c < CS_MAXCOL; c++) {
                char rend[CS_MAX_W + 2];
                int x;
                if (c >= lockCols && c < fc) continue;
                x = cell_x(c);
                if (x >= w) break;
                if (x + colw[c] > w) break;
                cell_render(c, row, rend, colw[c]);
                cputn(y, x, rend, colw[c]);
            }
        }
        /* current-cell markers on the separator positions; the original
         * paints them INVERTED so they stay visible over cell contents */
        if (row == curRow) {
            int cx = cell_x(curCol);
            if (cx - 1 >= 4 && cx - 1 < w) cchra(y, cx - 1, '>', A_REVERSE);
            if (cx + colw[curCol] < w) cchra(y, cx + colw[curCol], '<', A_REVERSE);
        }
    }
}

/* --------------------------------------------------------------- */
static const char *justname(char j)
{
    switch (j) {
    case 'R': return "text: right justified";
    case 'C': return "text: center justified";
    case 'W': return "text: repeating";
    default:  return "text: left justified";
    }
}

static void draw_cursorline(void)
{
    char lbuf[96];
    char cb[8], curcb[8];
    char cn[4];
    int w = cs_scrw();

    /* bottom border of the data window */
    cputn(line_box, 0, "   +", 4);
    dashline(line_box, 4);

    cs_col_name(curCol, cn);
    sprintf(cb, "%s%d", cn, curRow + 1);
    if (g_editstate == EDIT_EDITING) {
        cs_col_name(g_entrycol, cn);
        sprintf(curcb, "%s%d", cn, g_entryrow + 1);
    } else
        strcpy(curcb, cb);
    /* filename box + cursor/current + entry direction */
    sprintf(lbuf, "[%-10s]", fname[0] ? fname : "");
    cputn(line_cursor, 0, lbuf, 12);
    sprintf(lbuf, "  cursor:%6s      current:%6s     %s",
            cb, curcb, dirTB ? "T-B" : "L-R");
    cputn(line_cursor, 12, lbuf, w - 12);
}

/* is the typed contents a plain numeric literal (optionally signed,
 * digits with at most one '.')?  Those cells display their formatted
 * value in the contents field; anything else (formulas, input
 * formulas) is shown as typed. */
static int cont_is_number(const char *s)
{
    int i = 0, dig = 0, dot = 0;
    if (!s) return 0;
    if (s[0] == '+' || s[0] == '-') i = 1;
    for (; s[i]; i++) {
        if (s[i] >= '0' && s[i] <= '9') dig = 1;
        else if (s[i] == '.' && !dot) dot = 1;
        else return 0;
    }
    return dig;
}

static void draw_typecont(void)
{
    Cell *c = cs_cell(curCol, curRow);

    cputn(line_type, 0, "current||", 9);
    cputn(line_cont, 0, "data   ||", 9);
    /* type: during an entry the LIVE interpretation wins over the
     * committed cell state (manual: the user sees what the cell will
     * become - especially with the ^/" toggles mid-entry) */
    cput(line_type, 16, "type:");
    if (g_editstate == EDIT_EDITING)
        cput(line_type, 22, g_entry_num ? "numeric" : "text: left justified");
    else if (!c || c->type == CE_EMPTY)
        ; /* nothing */
    else if (c->type == CE_ALLOC)
        cput(line_type, 22, "(empty, but allocated)");
    else if (c->type == CE_TEXT)
        cput(line_type, 22, justname(c->just));
    else
        cput(line_type, 22, "numeric");
    if (c && c->formf) {
        /* manual 11.3: form-mode stop cells show "(form mode set)"
         * after the type text */
        int x = 22;
        if (g_editstate == EDIT_EDITING)
            x = 22 + (g_entry_num ? 7 : 20);
        else if (!c || c->type == CE_EMPTY)
            x = 22;
        else if (c->type == CE_ALLOC)
            x = 22 + 22;
        else if (c->type == CE_TEXT) {
            const char *jn = justname(c->just);
            x = 22 + (int) strlen(jn);
        } else
            x = 22 + 7;
        cput(line_type, x + 1, "(form mode set)");
    }
    /* contents: shows the cell as typed; plain numbers formatted */
    cput(line_cont, 12, "contents:");
    if (c && !c->iserr && c->type == CE_NUM && !showForm &&
        !csn_is_zero(&c->val) && !c->strres &&
        cont_is_number(c->cont)) {
        char buf[64];
        int prec = (c->prec >= 0) ? c->prec : colp[curCol];
        cs_fmt_num(&c->val, 20, prec, 0, buf, (int) sizeof(buf));
        cput(line_cont, 22, buf);
    } else if (c && c->cont) {
        cput(line_cont, 22, c->cont);   /* formula / input formula as typed */
    }
}

/* idle state of the edit line: "                edit: " */
/* idle state of the edit line: "                edit: " */
static void draw_editline_idle(void)
{
    cput(line_edit, 16, "edit: ");
    if (g_editstate == EDIT_EDITING && g_editlen > 0)
        cput(line_edit, EDITTEXT_X, g_edit);
}

/* draw an interactive prompt line (bottom row, starting at column 0)
 * with optional progress dots */
void cs_draw_prompt_text(const char *txt, const char *dots)
{
    cs_compute_layout();
    promptvis = 1;
    move(line_prompt, 0);
    clrtoeol();
    cput(line_prompt, 0, txt);
    if (dots && dots[0])
        cput(line_prompt, (int) strlen(txt) + 1, dots);
    move(line_prompt, (int) strlen(txt) +
                      ((dots && dots[0]) ? 1 + (int) strlen(dots) : 0));
    if (cs_scrw() < 4) move(line_prompt, 0);
    refresh();
}

void cs_prompt_hide(void)
{
    promptvis = 0;
    lastmsg[0] = '\0';
    cs_compute_layout();
    move(line_prompt, 0);
    clrtoeol();
    move(line_edit, 0);
    clrtoeol();
    draw_editline_idle();
    move(line_edit, EDITTEXT_X);   /* idle cursor rests on the edit field */
    refresh();
}

/* passive low-memory warning (manual: "MEMORY GETTING LOW" is displayed
 * on the right side of the screen) - redrawn with every full repaint */
static void draw_memwarn(void)
{
    const char *msg = "MEMORY GETTING LOW";
    int left = CS_BUDGET - entriesUsed;
    int x;
    if (left > CS_BUDGET / 10) return;     /* only when nearly full */
    x = cs_scrw() - (int) strlen(msg);
    if (x < 60) return;
    cput(extended ? 0 : 4, x, msg);
}

void cs_draw(void)
{
    cs_compute_layout();
    clear();
    draw_menu();
    draw_headers();
    draw_memwarn();
    draw_rows();
    draw_cursorline();
    draw_typecont();
    draw_editline_idle();
    /* the last row is cleared by clear(); messages are transient: they
     * are painted by cs_msg() and vanish as soon as the next key causes
     * any repaint */
    promptvis = 0;   /* a repaint always ends prompt mode */
    place_cursor();
    refresh();
}

static void cs_msg_text(const char *s, chtype attr);

void cs_msg(const char *fmt, ...)
{
    va_list ap;
    promptvis = 0;
    va_start(ap, fmt);
    vsprintf(lastmsg, fmt, ap);
    va_end(ap);
    lastmsg[75] = '\0';
    cs_msg_text(lastmsg, A_REVERSE);
}

void cs_msg_plain(const char *fmt, ...)
{
    va_list ap;
    promptvis = 0;
    va_start(ap, fmt);
    vsprintf(lastmsg, fmt, ap);
    va_end(ap);
    lastmsg[75] = '\0';
    cs_msg_text(lastmsg, A_NORMAL);
}

/* shared paint: system messages are inverse (CalcStar behavior),
 * Evaluate answers are NOT: CalcStar shows the result plainly. The
 * terminal cursor never rests in the status bar: it belongs on the
 * edit line (idle position, or after typed text while entering) */
static void cs_msg_text(const char *s, chtype attr)
{
    if (cs_headless) return;   /* --test mode: no screen */
    cs_compute_layout();
    move(line_prompt, 0);
    clrtoeol();
    cputa(line_prompt, 0, s, attr);
    place_cursor();
    refresh();
}

int cs_get_key(void)
{
    return getch();
}

/* KEY_RESIZE helper: the terminal was resized while waiting for a key.
 * Repaints everything at the new size and returns 1 so the caller can
 * ignore the pseudo-key and wait again.  Callers in prompt loops redraw
 * their prompt themselves (or via the next loop iteration). */
int cs_was_resize(int ch)
{
#ifdef KEY_RESIZE
    if (ch != KEY_RESIZE) return 0;
    if (promptvis) {
        promptvis = 0;
        cs_draw();
        promptvis = 1;   /* caller redraws/restores the prompt */
    } else {
        cs_draw();
    }
    return 1;
#else
    (void) ch;
    return 0;
#endif
}

int cs_prompt_char(const char *pr)
{
    int ch;
    strncpy(prompttxt, pr, 76);
    prompttxt[76] = '\0';
    promptvis = 1;
    for (;;) {
        cs_draw_prompt_text(prompttxt, "");
        ch = getch();
        if (cs_was_resize(ch)) continue;
        break;
    }
    promptvis = 0;
    return ch;
}

int cs_prompt_yn(const char *pr)
{
    int ch;
    for (;;) {
        ch = cs_prompt_char(pr);
        if (ch == 27) return -1;
        if (ch == 'y' || ch == 'Y') return 1;
        if (ch == 'n' || ch == 'N') return 0;
    }
}

/* the plain prompt editor: dflt shows while nothing is typed, the
 * first printable keystroke replaces it.  With keepgt != 0 a '>' as
 * the FIRST keystroke instead seeds the buffer with dflt + '>' - the
 * delete-range prompts use it so the prefilled row/column survives a
 * range answer ("2>" + "5"). */
static int prompt_str_core(const char *pr, const char *dflt, char *out,
                           int maxlen, int keepgt)
{
    char buf[80];
    int n, ch;
    n = 0;
    buf[0] = '\0';
    for (;;) {
        char line[100];
        sprintf(line, "%s %s", pr, n ? buf : (dflt ? dflt : ""));
        cs_draw_prompt_text(line, "");
        ch = getch();
        if (cs_was_resize(ch)) continue;
        if (ch == 27) { promptvis = 0; return -1; }
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            promptvis = 0;
            if (n == 0 && dflt && dflt[0]) {
                strncpy(out, dflt, maxlen - 1);
                out[maxlen - 1] = '\0';
                return (int) strlen(out);
            }
            strncpy(out, buf, maxlen - 1);
            out[maxlen - 1] = '\0';
            return (int) strlen(out);
        }
        if (ch == KEY_BACKSPACE || ch == 8 || ch == 127) {
            if (n > 0) buf[--n] = '\0';
            continue;
        }
        if (ch == '>' && keepgt && n == 0 && dflt && dflt[0]) {
            /* keep the prefilled row/column as the left side */
            int i;
            for (i = 0; dflt[i] && n < (int) sizeof(buf) - 2; i++)
                buf[n++] = dflt[i];
            if (n < (int) sizeof(buf) - 2) buf[n++] = '>';
            buf[n] = '\0';
            continue;
        }
        if (ch >= 32 && ch < 127 && n < (int) sizeof(buf) - 2) {
            buf[n++] = (char) ch;
            buf[n] = '\0';
        }
    }
}

int cs_prompt_str(const char *pr, const char *dflt, char *out, int maxlen)
{
    return prompt_str_core(pr, dflt, out, maxlen, 0);
}

/* the delete-range prompt: like cs_prompt_str but a leading '>' keeps
 * the prefilled row/column (delete_cmd's R/C fields) */
int cs_prompt_range(const char *pr, const char *dflt, char *out, int maxlen)
{
    return prompt_str_core(pr, dflt, out, maxlen, 1);
}

/* helper to paint the prompt cursor position (shared by the coord and
 * plain prompt editors) */
static void prompt_edit_paint(const char *pr, const char *shown)
{
    char line[100];
    sprintf(line, "%s %s", pr, shown);
    cs_draw_prompt_text(line, "");
}

/* coordinate prompt with the "@" reference: "@" inserts the CURRENT
 * cell-cursor coordinate (the metric the referenced cell-cursor keeps
 * its place), and the cursor KEYS move the sheet cursor while the
 * prompt is up.  setmode: 1 = the field holds ONE coordinate (@
 * replaces the input), 0 = append it (range fields: "B3" then ">" then
 * @), 2 = like 0 plus a '>' as the FIRST keystroke seeds the buffer
 * with dflt + ">" (the delete From-To prompt keeps its prefilled
 * coordinate as the left side).  Everything else behaves exactly like
 * cs_prompt_str. */
int cs_prompt_coord(const char *pr, const char *dflt, char *out,
                    int maxlen, int setmode)
{
    char buf[80];
    int n, ch;
    n = 0;
    buf[0] = '\0';
    for (;;) {
        ch = -1;
        if (n)
            prompt_edit_paint(pr, buf);
        else
            prompt_edit_paint(pr, dflt ? dflt : "");
        ch = getch();
        if (cs_was_resize(ch)) continue;
        if (ch == 27) { promptvis = 0; return -1; }
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            promptvis = 0;
            if (n == 0 && dflt && dflt[0]) {
                strncpy(out, dflt, maxlen - 1);
                out[maxlen - 1] = '\0';
                return (int) strlen(out);
            }
            strncpy(out, buf, maxlen - 1);
            out[maxlen - 1] = '\0';
            return (int) strlen(out);
        }
        if (ch == '@') {
            /* insert the current cursor coordinate; the cell cursor
             * stays where it is */
            char cn[4];
            char coord[10];
            cs_col_name(curCol, cn);
            sprintf(coord, "%s%d", cn, curRow + 1);
            if (setmode) { buf[0] = '\0'; n = 0; }
            {
                int i;
                for (i = 0; coord[i]; i++)
                    if (n < (int) sizeof(buf) - 2) buf[n++] = coord[i];
                buf[n] = '\0';
            }
            continue;
        }
#ifdef KEY_UP
        if (ch == KEY_UP || ch == KEY_DOWN ||
            ch == KEY_LEFT || ch == KEY_RIGHT) {
            if (ch == KEY_UP) cs_move_cursor(0, -1);
            else if (ch == KEY_DOWN) cs_move_cursor(0, 1);
            else if (ch == KEY_LEFT) cs_move_cursor(-1, 0);
            else cs_move_cursor(1, 0);
            cs_draw();
            continue;
        }
#endif
        if (ch == 5) { cs_move_cursor(0, -1); cs_draw(); continue; }
        if (ch == 24) { cs_move_cursor(0, 1); cs_draw(); continue; }
        if (ch == 19) { cs_move_cursor(-1, 0); cs_draw(); continue; }
        if (ch == 4) { cs_move_cursor(1, 0); cs_draw(); continue; }
        if (ch == KEY_BACKSPACE || ch == 8 || ch == 127) {
            if (n > 0) buf[--n] = '\0';
            continue;
        }
        if (ch == '>' && setmode == 2 && n == 0 && dflt && dflt[0]) {
            /* keep the prefilled coordinate as the left side */
            int i;
            for (i = 0; dflt[i] && n < (int) sizeof(buf) - 2; i++)
                buf[n++] = dflt[i];
            if (n < (int) sizeof(buf) - 2) buf[n++] = '>';
            buf[n] = '\0';
            continue;
        }
        if (ch >= 32 && ch < 127 && n < (int) sizeof(buf) - 2) {
            buf[n++] = (char) ch;
            buf[n] = '\0';
        }
    }
}

/* the wanted input mode while nested code runs (the recalculation
 * driver switches the keyboard to non-blocking for its ESC polls and
 * restores the previous state afterwards; cs_poll_escape must restore
 * THE WANTED MODE, not blocking - it runs inside the driver's passes) */
static int cs_kb_nodelay;       /* 0 = blocking (default), 1 = non-blocking */

void cs_kb_nodelay_set(int on)
{
    cs_kb_nodelay = on;
    nodelay(stdscr, on);
}

/* poll for an ESC keystroke: non-blocking read, 1 = ESC seen, else 0.
 * Used by the side-effect statement machinery for the user interrupt. */
int cs_poll_escape(void)
{
    int ch;
    nodelay(stdscr, TRUE);
    ch = getch();
    nodelay(stdscr, cs_kb_nodelay);     /* restore the wanted mode */
    return (ch == 27);
}

/* position the physical (blinking) terminal cursor.  It always rests at
 * the bottom of the screen: while typing, just after the entry text; on
 * the "edit: " field when idle -- never inside the cell area. */
static void place_cursor(void)
{
    int x;

    if (promptvis) return;   /* prompt routines position it themselves */
    if (g_editstate == EDIT_EDITING) {
        x = EDITTEXT_X + g_editlen;
        if (x > cs_scrw() - 1) x = cs_scrw() - 1;
        move(line_edit, x);
        return;
    }
    move(line_edit, EDITTEXT_X);
}
