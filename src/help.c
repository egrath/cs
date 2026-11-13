/* help.c - the two CalcStar help pages */
#include <string.h>
#include <curses.h>
#include "cs.h"

/* the ORIGINAL CalcStar help screens (user-supplied verbatim,
 * 2026-11-13): our extensions are documented in README.md /
 * AGENTS.md, not in the in-app help.  The \t characters reproduce
 * the original capture's tab indentation (curses expands them). */
static const char *page1[] = {
"\t---- Help page 1 ----   hit <RETURN> to continue",
"________________Main entry mode_____________________________________________",
"",
"Cursor is controlled with keys E, D, X, and S with the <CNRTL> key held down.",
"  Direction is up, right, down, and left in that order.  (Control Diamond)",
"  <RETURN> moves the cursor right if no data has just been typed.",
"  <CNTRL>Z moves the cursor to the next row, first column.",
"",
"Data entry: digits 0-9, +, -, signal numeric entry.",
"\t    alphabetic characters or blank signal text entry.",
"\t    /, followed by R, C, or L justifies text right, centered or left.",
"\t    ^ is used to switch between text and numeric if needed.",
"\t    /= duplicates entry across column. /P causes page break for print.",
"\t    ! sets special entry mode for arithmetic modification of input. ",
"",
"Equations may contain numeric data, cell references, and functions.",
"\t    four-function arithmetic (+,-,*,/) may be used with any of these.",
"\t    @ refers to current cell location.  Type one character and then",
"\t\tmove cursor to collect cell references without typing them.",
"\t    ! following a cell reference holds ref constant in relative copy.",
"\t    functions include: sum, avg, cnt, abs, exp, log, ln, min, max,",
"\t\tregr, slope, proj, depd.",
"",
""};

static const char *page2[] = {
"\t---- Help page 2 -----   Hit <RETURN> to continue",
"________________Extended commands_______________________________________",
"   ",
"   these commands are accessed by hitting the ';' key",
"      Auto        enter Automatic entry mode.  <ESC> will abort.",
"      Copy        Copy a (range) entry to another (range) entry",
"      Delete      Delete a row, column, entry or the whole array",
"      Edge        set the window top left corner to the cursor position",
"      Format      change column size or precision under cursor",
"      Goto        move cursor to a specific column and row",
"      Help        display this information",
"      Insert      Insert a row or column into the array",
"      Load        Load a file into the array",
"      Merge       overlay file onto array",
"      Order       Change the evaluation (column/row) order of the array",
"      Print       Print a report",
"      Quit        exit the report generator",
"      Recalculate recompute entry at cursor or whole array in current order",
"      Save        Save the array to a file",
"      What        if text, prints 1st column & row entries of cursor position",
"      ?           print approximate storage space in terms of entries",
"      =           lock column A and row 1 onto display",
"      *\t\t  Switch between 10 (with menu) and 15 row screen display",
"",
""};

#define P1N (int) (sizeof(page1) / sizeof(page1[0]))
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
