/* cs.h - CalcStar (MicroPro 1981) clone - shared definitions */
#ifndef CS_H
#define CS_H

#include <stdio.h>
#include "num.h"

#define CS_MAXROW 255          /* rows 1..255 */
#define CS_MAXCOL 127          /* cols A..DW  */
#define CS_DEF_W 10
#define CS_MIN_W 3
#define CS_MAX_W 63
#define CS_DEF_P 2
#define CS_MAX_P 24   /* visible decimals; engineering extension (manual: 12) */
#define CS_EDITMAX 120
#define CS_BUDGET 2000         /* emulated entry pool for ';?' */

typedef enum { CE_EMPTY = 0, CE_ALLOC = 1, CE_TEXT = 2, CE_NUM = 3 } CellType;

typedef struct {
    CellType type;
    char just;      /* 'L','R','C' justification; 'W' = repeating */
    char *rep;      /* repeat pattern when just=='W' */
    char *cont;     /* contents as typed (plain text, or formula) */
    Csn val;        /* cached numeric value (BCD decimal, no FP)  */
    int iserr;      /* 1 => cell shows ?n? */
    char *strres;   /* string result of a formula, or NULL */
    int formf;      /* form mode set (for ;A Auto) */
    int prec;       /* per-entry precision override, -1 = none */
    int notation;   /* display notation (flagged ext 2026-11): 0=auto,
                     * 1=scientific (too narrow: "!n!"), 2=decimal
                     * (too narrow: head kept, trailing '*') */
} Cell;

#define EDIT_IDLE 0
#define EDIT_EDITING 1
#define EDIT_COMMITTED 2

extern Cell cells[CS_MAXROW][CS_MAXCOL];   /* cells[row][col], 0-based */
extern int  colw[CS_MAXCOL];
extern int  colp[CS_MAXCOL];

extern int  curCol, curRow;     /* cursor (0-based) */
extern int  winCol, winRow;     /* window origin (0-based) */
extern int  dirTB;              /* 0 = L-R, 1 = T-B */
extern int  extended;           /* 1 = 15-row window (menu hidden) */
extern int  lockCols, lockRows; /* locked (titled) rows/cols counts */
extern int  showForm;           /* ^B data toggle: show formulas instead of values */
extern char fname[40];          /* current file name (no extension) */
extern char cs_cmdfile[40];     /* .CSD name given on the command line */
void   cs_stay_visible(void);   /* re-anchor window around the cursor */
void   cs_move_cursor(int dc, int dr);     /* exported cursor step */
void   cs_delete_rows(int r1, int r2);   /* shift cells+refs+formats */
void   cs_delete_columns(int c1, int c2);
void   cs_clear_range(int c1, int r1, int c2, int r2);  /* ;D F clear */
int    cs_prompt_coord(const char *pr, const char *dflt, char *out,
                       int maxlen, int setmode);  /* @-aware coord prompt */
int    cs_prompt_range(const char *pr, const char *dflt, char *out,
                       int maxlen);   /* leading '>' keeps the prefill */

extern int  entriesUsed;
extern int  orderTB;            /* ;O recalculation order */
extern int  cs_headless;        /* 1 = --test mode: no curses screen */
extern int  g_reg_ok;
extern Csn   g_reg_slope, g_reg_icept, g_reg_meany;   /* BCD values */

/* entry engine state */
extern char g_edit[CS_EDITMAX];
extern int  g_editlen;
extern int  g_editstate;        /* EDIT_* */
extern int  g_entry_num;        /* pending entry is numeric */
extern int  g_entryrow, g_entrycol; /* 'current' location of pending entry */

/* cells.c */
Cell  *cs_cell(int col, int rw);
char  *cs_strdup(const char *s);
int    cs_name_to_col(const char *s);
void   cs_col_name(int col, char *buf);
int    cs_parse_coord(const char *s, int *col, int *row);
int    cs_parse_range(const char *s, int *c1, int *r1, int *c2, int *r2);
int    cs_coord_text(const char *s, int *c1, int *r1, int *c2, int *r2);
char  *cs_sort_refs(const char *s, int c1, int r1, int c2, int r2,
                    const int *perm);
void   cs_range_norm(int *c1, int *r1, int *c2, int *r2);
void   cs_set_text(Cell *c, const char *cont, char just);
void   cs_set_num(Cell *c, const char *cont, const Csn *val, int iserr, const char *strres);
void   cs_free_cell(Cell *c);
void   cs_clear_all(void);
int    cs_scan_refs(const char *s, void (*cb)(int col, int row, void *), void *ctx);
char  *cs_adjust_refs(const char *s, int dc, int dr);
char  *cs_shift_refs(const char *s, int iscol, int at, int insert);
char  *cs_shift_refs_n(const char *s, int iscol, int at, int delta);
/* side-effect functions + iteration (user-extended 2026-11, flagged) */
#define KW_ABORT 13   /* internal "user interrupt" code */
int    cs_recalc_entry(int col, int rw);   /* returns eval error code */
void   cs_recalc_all(void);
void   cs_run_recalc_passes(int always_one); /* side-effect driver */
void   cs_mark_sheet_change(void);  /* eval.c: a stored value changed */
extern int cs_recalc_requested;     /* RECALC() seen during evaluation */
int    cs_recalc_limit_hit(void);   /* 1 = the last driver run overran */
void   cs_recalc_limit_reset(void); /* clear before a new driver context */
extern int cs_recalc_last_passes;   /* passes of the last driver run */

/* error-cell queue (filled by the last Recalculate, in recalc order):
 * main.c replays the sticky "Error ... hit space [coord]" messages one
 * after another, confirmed with Space */
struct CsErrRec { int col, row, code; };
extern struct CsErrRec cs_errlist[256];
extern int cs_nerrlist;
void   cs_sticky_err(int code, const char *expr, int col, int rw);
void   cs_set_errpend(int on);             /* main.c: sticky-message walker */

void   cs_reg_reset(void);   /* zero the REGR() state */
int    cs_commit_entry(int col, int rw, const char *entry);
void   cs_fmt_num(const Csn *v, int width, int prec, int iserr, int notation,
                  char *buf, int bufn);
int    cs_ensure_room(void);
void   cs_recount_entries(void);
int    cs_is_inputf(const Cell *c);

/* parser.c  (returns 0 on success; nonzero = CalcStar evaluation error
   code; -1 = CalcStar "Error 0" value-range error such as ?n?) */
int    cs_eval_formula(const char *s, Csn *val, char **str);
int    cs_eval_range(const char *s, int *c1, int *r1, int *c2, int *r2);
int    cs_eval_keycol(const char *s, int *err);
void   cs_sort_range(int c1, int r1, int c2, int r2, int keycol, int desc);

/* screen.c */
void   cs_compute_layout(void);
int    cs_scrw(void);          /* drawing width: full terminal (COLS) */
int    cs_cell_x(int col);
int    cs_win_rows(void);
void   cs_draw(void);
void   cs_draw_prompt_text(const char *txt, const char *dots);
void   cs_msg_plain(const char *fmt, ...);
int    cs_poll_escape(void);   /* screen.c: non-blocking ESC poll */
void   cs_kb_nodelay_set(int on);  /* screen.c: wanted input mode */
void   cs_msg(const char *fmt, ...);
int    cs_get_key(void);
int    cs_was_resize(int ch);           /* KEY_RESIZE -> full repaint */
int    cs_prompt_char(const char *prompt);
int    cs_prompt_yn(const char *prompt);
int    cs_prompt_str(const char *prompt, const char *dflt, char *out, int maxlen);
void   cs_prompt_hide(void);

/* command.c */
void   cs_command_menu(void);
void   cs_lock_cmd(void);
void   cs_goto_cmd(void);

/* fileio.c */
int    cs_save(const char *name, int c1, int r1, int c2, int r2, const char *pw);
int    cs_load(const char *name, int c0, int r0, int merge);
int    cs_save_dialog(void);
int    cs_load_dialog(int merge);
int    cs_file_password(const char *name, char *pw, int max);
int    cs_prompt_hidden(const char *pr, char *out, int max);

/* print.c */
void   cs_print_cmd(void);

#endif /* CS_H */
