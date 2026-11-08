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
}
