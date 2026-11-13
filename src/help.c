/* help.c - the two CalcStar help pages */
#include <string.h>
#include <curses.h>
#include "cs.h"

static const char *page1[] = {
"*        Help page 1 -- hit <RETURN> to continue                          *",
"*              Main entry mode                                           *",
"* Cursor is controlled with keys E, D, X, and S with the <CTRL> key held  *",
"* down. Direction is up, right, down, and left in that order.             *",
"* (Control Diamond)                                                       *",
"* <RETURN> moves the cursor right if no data has just been typed.         *",
"* <CTRL> Z moves the cursor to the next row, first column.                *",
"*                                                                         *",
"* Data Entry: digits 0-9, +, -, (, and . signal numeric entry.            *",
"*             alphabetic characters or blank signal text entry.           *",
"*             /, followed by R, C, or L justifies text right, centered,   *",
"*             or left.                                                    *",
"*             \" is used to switch between text and numeric if needed.     *",
"*             /= duplicates entry across column. /p causes page break     *",
"*             for print. /R, /C and /L with nothing after re-justify an   *",
"*             existing text entry.                                        *",
"*                                                                         *",
"*   Equations may contain numeric data, cell references, and functions.   *",
"*   four-function arithmetic (+,-,*,/) may be used with any of these.     *",
"*   @ refers to current cell location. Type one character and then        *",
"*       move cursor to put in cell references without typing them.        *",
"*   ! following a cell reference holds it constant in relative copy       *",
"*   functions include: sum, avg, cnt, abs, exp, log, ln, min, max,        *",
"*       sqrt, regr, slope, proj, depd.                                    *",
"****************************************************************************"
};
#define P1N (int) (sizeof(page1) / sizeof(page1[0]))

static const char *page2[] = {
"*______________Extended -- Help ___________________________________________*",
"*        Help page 2 -- hit <RETURN> to continue                           *",
"*                                                                          *",
"* these commands are accessed by hitting the ';' key                       *",
"*     Auto        enter Automatic entry mode. <ESC> will abort.            *",
"*     Copy        Copy a (range) entry to another (range) entry            *",
"*     Delete      Delete a row, column, entry or the whole array           *",
"*     Edge        set the window top left corner to the cursor position    *",
"*     Format      change column size or precision under cursor             *",
"*     Goto        move cursor to a specific column and row                 *",
"*     Help        display this information                                 *",
"*     Insert      Insert a row or column into the array                    *",
"*     Load        Load a file into the array                               *",
"*     Merge       overlay file onto array                                  *",
"*     Order       Change the evaluation (column/row) order of the array    *",
"*     Print       Print a report                                           *",
"*     Quit        exit the report generator                                *",
"*     Recalculate recompute entry at cursor or whole array in current order*",
"*     Save        Save the array to a file                                 *",
"*     What        if text, prints 1st column & row entries of cursor pos   *",
"*     ?           print approximate storage space in terms of entries      *",
"*       =         lock column A and row 1 onto display                     *",
"*       *         switch between 10 (with menu) and 15 row screen display  *",
"*                                                                          *",
"****************************************************************************"
};
#define P2N (int) (sizeof(page2) / sizeof(page2[0]))

static void draw_page(const char **pg, int n)
{
    int i;
    clear();
    for (i = 0; i < n && i < LINES; i++)
        mvaddstr(i, 0, pg[i]);
}

/* page 3 (flagged extension 2026-11, branch ref-val): the expression
 * language extensions - references, VAL/REF, dynamic ranges, SET/
 * RECALC with conditionals */
static const char *page3[] = {
"*       Help page 3 -- extensions (hit <RETURN> to end)                  *",
"*                                                                        *",
"* EXPRESSION EXTENSIONS (not in the 1982 manual)                         *",
"* A bare cell name is a REFERENCE:  A1  B5  AA20                         *",
"* VAL(cell)    the cell's numeric value:  VAL(A1)+VAL(B1)                *",
"*              SQRT(VAL(A1))   VAL(REF(2,5))                             *",
"*              VAL of an empty/text/error cell -> Error 1                *",
"* REF(col,row) one-based coordinates:  REF(1,1) is A1  REF(2,5) is B5    *",
"*              dynamic:  REF(1+1,2+3)  REF(VAL(B1)+1,VAL(B2))            *",
"*              limits: columns 1..127, rows 1..255                       *",
"* COLIDX(x)    the column index:  COLIDX(AB)  COLIDX(\"AB\")             *",
"*              COLIDX(REF(2,3))  ->  2   (row ignored)                  *",
"* RANGES      ref > ref builds a range (numbers compare as before):      *",
"*              A1>A5   REF(1,1)>REF(1,5)   A1>REF(1,5)                   *",
"*              reversed ranges normalize; diagonals are Error 2          *",
"*              SUM(A1>A5)  AVG(REF(2,1)>REF(2,10))  CNT(A1>REF(1,10))    *",
"*              SUM(A1>A5,B1>B3)   REGR(A1>A10,B2)                        *",
"*              value comparisons use VAL:  VAL(A1)>VAL(A5)               *",
"* SET(cell,value) in conditionals:                                       *",
"*              (VAL(A1)>0):SET(A1,VAL(A1)-1);RECALC();0:\"finished\"       *",
"*              (1):SET(REF(C1,C2),VAL(B7));0:1  dynamic target           *",
"*              SET(A2,VAL(B1)*2)  SET(A1,B2) copies number or text       *",
"*              SET(A1,\"\\rtext\") right-justifies                      *",
"*              RECALC() / RECALC(ref) request recalculation passes       *",
"*              idempotent SETs settle; 1000-pass limit; ESC interrupts   *",
"* DIALOGS     dynamic ranges in prompts:  ;D R  REF(1,C2)>REF(1,C3)      *",
"*              Goto  REF(2,4)  (static coords still work first)          *",
};

#define P3N (int) (sizeof(page3) / sizeof(page3[0]))

void cs_help_cmd(void)
{
    draw_page(page1, P1N);
    refresh();
    for (;;) {
        int ch = getch();
#ifdef KEY_RESIZE
        if (ch == KEY_RESIZE) { draw_page(page1, P1N); refresh(); continue; }
#endif
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER || ch == 27) break;
    }
    draw_page(page2, P2N);
    refresh();
    for (;;) {
        int ch = getch();
#ifdef KEY_RESIZE
        if (ch == KEY_RESIZE) { draw_page(page2, P2N); refresh(); continue; }
#endif
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER || ch == 27) break;
    }
    draw_page(page3, P3N);
    refresh();
    for (;;) {
        int ch = getch();
#ifdef KEY_RESIZE
        if (ch == KEY_RESIZE) { draw_page(page3, P3N); refresh(); continue; }
#endif
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER || ch == 27) break;
    }
}
