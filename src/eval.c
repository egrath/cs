/* eval.c - recalculation, entry commitment (BCD arithmetic), the
 * side-effect execution driver (SET / RECALC / iterated passes) */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <curses.h>
#include "cs.h"

int cs_is_inputf(const Cell *c);

/* side-effect state: cs_recalc_requested is set by RECALC() /
 * RECALC(cell) during a formula evaluation; cs_sheet_changed is set
 * whenever a stored cell value changes (the stability criterion) */
int cs_recalc_requested = 0;
int cs_recalc_last_passes = 0;      /* passes of the last driver run */
static int cs_sheet_changed;
static int cs_eval_aborted;         /* KW_ABORT seen during a pass */

void cs_mark_sheet_change(void)
{
    cs_sheet_changed = 1;
}

/* the safeguard limits and the interruption message (flagged strings) */
#define CS_MAX_PASSES 1000

/* re-evaluate a single numeric cell (input-formula cells excluded) */
/* error-cell queue: filled by the last Recalculate, in the same order
 * the recalculation ran; main.c replays the "?n?" cells one after
 * another (sticky "Error ... hit space [coord]", confirmed with Space) */
struct CsErrRec cs_errlist[256];
int cs_nerrlist = 0;

static void errq_add(int col, int rw, int code)
{
    if (cs_nerrlist < (int) (sizeof(cs_errlist) / sizeof(cs_errlist[0]))) {
        cs_errlist[cs_nerrlist].col = col;
        cs_errlist[cs_nerrlist].row = rw;
        cs_errlist[cs_nerrlist].code = code;
        cs_nerrlist++;
    }
}

/* paint the sticky evaluation-error message: the original shows the
 * error class for value-range problems and the CALCellL coordinate
 * afterwards: "Value range ERROR 0-> +A1/0? hit space [B2]" / 1981
 * manual: "Error 7-> <expr>   hit space [B4]" (both confirm the
 * trailing [cell] annotation).  Caller redraws BEFORE calling so the
 * message survives the repaint. */
void cs_sticky_err(int code, const char *expr, int col, int rw)
{
    char cn[4], coord[10];
    cs_col_name(col, cn);
    sprintf(coord, "%s%d", cn, rw + 1);
    expr = expr ? expr : "";
    if (code <= 0)
        cs_msg("Value range ERROR 0-> %s? hit space [%s]", expr, coord);
    else
        cs_msg("Error %d-> %s? hit space [%s]", code, expr, coord);
}

/* the FACTORIAL ENTRY recompute (user-ext 2026-11-13): evaluate the
 * prefix (the content minus the trailing '!') and store FACT of it.
 * Marks the sheet changed only on a real difference. */
static int factf_recompute(int col, int rw, Cell *c)
{
    char pfx[CS_EDITMAX + 1];
    Csn src, fv;
    int n = (int) strlen(c->cont), e, fe;

    memcpy(pfx, c->cont, (size_t) n - 1);
    pfx[n - 1] = '\0';
    e = cs_eval_fact_source(pfx, &src);
    fe = 0;
    if (!e) fe = csn_fact(&fv, &src);
    if (e == KW_ABORT) { cs_eval_aborted = 1; return KW_ABORT; }
    if (e || fe) {
        if (!c->iserr) cs_sheet_changed = 1;
        c->iserr = 1;
        csn_zero(&c->val);
        errq_add(col, rw, e ? e : 0);
        return e ? e : 0;
    }
    if (c->iserr || csn_cmp(&fv, &c->val) != 0) cs_sheet_changed = 1;
    csn_copy(&c->val, &fv);
    c->iserr = 0;
    return 0;
}

int cs_recalc_entry(int col, int rw)
{
    Cell *c = cs_cell(col, rw);
    Csn val, old;
    char *str = NULL, *oldstr;
    int err, changed;
    if (!c || c->type != CE_NUM || !c->cont) return 0;
    if (cs_is_inputf(c)) return 0;      /* the value holder: not a formula */
    if (c->factf) return factf_recompute(col, rw, c);
    csn_copy(&old, &c->val);
    oldstr = c->strres;                 /* kept for the change check */
    err = cs_eval_formula(c->cont, &val, &str);
    if (err == KW_ABORT) {
        /* user interrupt: the cell keeps its previous value, the
         * message is already painted by the abort site */
        cs_eval_aborted = 1;
        return KW_ABORT;
    }
    if (err) {
        if (oldstr) free(oldstr);
        c->strres = NULL;
        if (!c->iserr) cs_sheet_changed = 1;    /* an error is a change */
        c->iserr = 1;
        errq_add(col, rw, err);         /* queue for ";R" replay */
        return err;
    }
    /* stability: the NEW stored value (or string result) differing
     * from the OLD one marks the pass dirty */
    changed = c->iserr || csn_cmp(&val, &old) != 0;
    if (!changed && ((oldstr != NULL) != (str != NULL)))
        changed = 1;                    /* string result appeared/vanished */
    else if (!changed && oldstr && str && strcmp(oldstr, str) != 0)
        changed = 1;                    /* the string result changed */
    if (oldstr) free(oldstr);
    if (changed) cs_sheet_changed = 1;
    csn_copy(&c->val, &val);
    c->iserr = 0;
    c->strres = NULL;
    if (str) c->strres = str;
    return 0;
}

/* the one-pass body of the recalculation (locked rows first) */
static void recalc_pass(void)
{
    int r, c;
    cs_nerrlist = 0;                    /* fresh queue per recalculation */
    if (dirTB) {
        for (c = 0; c < CS_MAXCOL; c++)
            for (r = 0; r < CS_MAXROW; r++)
                cs_recalc_entry(c, r);
    } else {
        for (r = 0; r < CS_MAXROW; r++)
            for (c = 0; c < CS_MAXCOL; c++)
                cs_recalc_entry(c, r);
    }
}

/* 1 = the last driver run exhausted the pass limit without stabilizing
 * (the callers re-paint the message AFTER their final repaint, which
 * would otherwise wipe it) */
static int cs_limit_hit;

int cs_recalc_limit_hit(void)
{
    return cs_limit_hit;
}

void cs_recalc_limit_reset(void)
{
    cs_limit_hit = 0;
}

/* the side-effect execution driver: RECALC() during a formula marks a
 * request; the CALLING context runs whole-sheet passes until the
 * spreadsheet stabilizes (a pass changes no stored value), the pass
 * limit is reached or the user interrupts with ESC.  always_one: run
 * the first pass unconditionally (the ;R entry points) or only when a
 * RECALC was requested (the entry-commit / calculator contexts).
 * Status: an inverse "Calculating ... (pass N)" line while iterating
 * (flagged); the window is repainted after every changed pass so the
 * user sees the current cell values update live (user request
 * 2026-11).  The keyboard is non-blocking for the driver's duration
 * (its own ESC poll and the per-statement cs_poll_escape share the
 * mode; a hard-coded blocking restore once made every pass wait for
 * an ENTER keypress).  An overrun sets cs_limit_hit for the callers. */
void cs_run_recalc_passes(int always_one)
{
    int pass, iterating = 0;
    cs_limit_hit = 0;
    cs_recalc_last_passes = 0;
    if (!always_one && !cs_recalc_requested) return;
    cs_recalc_requested = 0;
    cs_eval_aborted = 0;
    cs_kb_nodelay_set(1);               /* non-blocking ESC polls */
    for (pass = 1; pass <= CS_MAX_PASSES; pass++) {
        int ch;
        if (iterating)
            cs_msg("Calculating ... (pass %d)", pass);
        cs_sheet_changed = 0;
        recalc_pass();
        if (cs_recalc_requested) cs_recalc_requested = 0;
        if (cs_eval_aborted) {          /* interrupt inside a formula */
            cs_eval_aborted = 0;
            break;
        }
        if (!iterating && !cs_sheet_changed) break;   /* plain ;R: one pass */
        iterating = 1;
        if (!cs_sheet_changed) break;                 /* stabilized */
        /* the user watches the iteration: repaint the window with the
         * current values, then put the pass status back (cs_draw
         * wipes the status rows) */
        cs_draw();
        cs_msg("Calculating ... (pass %d)", pass);
        ch = getch();                   /* non-blocking: ERR when idle */
        if (ch == 27) {
            cs_msg("User interrupted execution");
            break;
        }
    }
    cs_kb_nodelay_set(0);
    cs_recalc_last_passes = pass;       /* diagnostics / tests */
    if (iterating && pass > CS_MAX_PASSES) {
        cs_limit_hit = 1;
        cs_msg("Recalculation limit reached");
    }
}

void cs_recalc_all(void)
{
    cs_run_recalc_passes(1);
}
/* does the cell hold an '!expr!' input formula? */
int cs_is_inputf(const Cell *c)
{
    int n = (int) strlen(c->cont);
    return n > 2 && c->cont[0] == '!' && c->cont[n - 1] == '!';
}

/* does the cell hold a FACTORIAL ENTRY ("5!", "A5!", "REF(2,5)!")?
 * (user-ext 2026-11-13): displays FACT of its source and recomputes
 * on ;R — unlike the '!' value holder, the window shows the value,
 * not "In!" */
int cs_is_factf(const Cell *c)
{
    return c->factf;
}

/* is this char one that may start a numeric entry */
static int numeric_start(int ch)
{
    return (ch >= '0' && ch <= '9') || ch == '+' || ch == '-' ||
           ch == '(' || ch == '.' || ch == '!';
}

/* commit an entry into a cell, exactly like typing it and hitting RETURN.
 * Returns the evaluation error code (0 = no error / text commit); callers
 * paint the sticky "Error n-> ..." line with it (<=0 folds to 0: the
 * Appendix "value range error" code costs the -1 sentinel used inside). */
int cs_commit_entry(int col, int rw, const char *entry)
{
    Cell *c;
    char buf[CS_EDITMAX + 1];
    int n;

    if (!entry || !*entry) return 0;
    strncpy(buf, entry, CS_EDITMAX);
    buf[CS_EDITMAX] = '\0';
    n = (int) strlen(buf);
    c = cs_cell(col, rw);
    if (!c) return 0;

    /* the FACTORIAL ENTRY (user-ext 2026-11-13): "5!", "A5!",
     * "REF(2,5)!" - the content keeps the entry verbatim, the window
     * shows FACT of the prefix's value, and ;R recomputes.  The rule
     * is a PARSE check (decision (b)): the prefix must be a valid
     * expression - a source that is not a valid number yet shows ?n?
     * until it is; non-expression prefixes (STOP!, 5!!) stay text.
     * The manual's '!...'!' value-holder form is untouched. */
    if (n >= 2 && buf[n - 1] == '!' && buf[0] != '!') {
        char pfx[CS_EDITMAX + 1];
        Csn fv, src;
        int e, fe;
        memcpy(pfx, buf, (size_t) n - 1);
        pfx[n - 1] = '\0';
        if (cs_parse_only(pfx) == 0) {
            csn_zero(&fv);
            e = cs_eval_fact_source(pfx, &src);
            fe = 0;
            if (!e) fe = csn_fact(&fv, &src);
            if (!cs_ensure_room()) { cs_msg("MEMORY IS TOO LOW"); return 0; }
            cs_set_num(c, buf, &fv, (e || fe) ? 1 : 0, NULL);
            c->factf = 1;
            cs_recount_entries();
            return 0;
        }
    }

    /* ---- calculator: trailing '?' evaluates without storing ---- */
    if (buf[n - 1] == '?' && buf[0] != '!') {
        char tmp[CS_EDITMAX + 1];
        Csn val;
        char *str = NULL;
        char out[128];
        int err;
        strncpy(tmp, buf, (size_t) n - 1);
        tmp[n - 1] = '\0';
        err = cs_eval_formula(tmp, &val, &str);
        if (err == KW_ABORT) return 0;      /* interrupt already reported */
        if (err) cs_msg("Error %d-> %s", err <= 0 ? 0 : err, tmp);
        else if (str) {
            cs_msg_plain("%s", str);
            free(str);
        } else {
            /* format with up to 10 decimals, then strip trailing zeros
               for the calculator display (string work only) */
            /* manual 4.1/4.2 pinned the calculator at 12 decimals; the
               visible precision is user-extended to CS_MAX_P (24) to
               match the cells */
            csn_fmt_fixed(&val, CS_MAX_P, out, (int) sizeof(out));
            cs_msg_plain("=%24s", out);   /* answers are non-inverse */
        }
        /* the calculator is a driver context too (RECALC in the pending
         * text iterates; flagged) */
        cs_run_recalc_passes(0);
        if (cs_recalc_limit_hit())
            cs_msg("Recalculation limit reached");
        return err;
    }

    /* ---- input-formula: !110%! ---- */
    if (buf[0] == '!' && buf[n - 1] == '!' && n > 2) {
        if (!cs_ensure_room()) { cs_msg("MEMORY IS TOO LOW"); return 0; }
        cs_free_cell(c);
        c->type = CE_NUM;
        c->just = 'R';
        c->cont = cs_strdup(buf);
        csn_zero(&c->val);
        c->iserr = 0;
        entriesUsed++;
        return 0;
    }

    /* ---- justification prefix ---- */
    if (buf[0] == '/') {
        int ch = toupper((unsigned char) buf[1]);
        if (ch == 'R' || ch == 'C' || ch == 'L') {
            char just = (char) ch;
            if (buf[2] == '\0') {
                /* re-justify the existing entry */
                if (c->type == CE_TEXT) c->just = just;
                return 0;
            }
            if (!cs_ensure_room()) { cs_msg("MEMORY IS TOO LOW"); return 0; }
            cs_set_text(c, buf + 2, just);
            return 0;
        }
        if (buf[1] == '=') {
            cs_set_text(c, buf + 2, 'W');
            return 0;
        }
        /* '/page' etc: plain text stored as typed */
        if (!cs_ensure_room()) { cs_msg("MEMORY IS TOO LOW"); return 0; }
        cs_set_text(c, buf, 'L');
        return 0;
    }

    /* ---- numeric or text, per pending toggle ---- */
    if (g_entry_num && numeric_start((unsigned char) buf[0])) {
        Csn val;
        char *str = NULL;
        int err;
        if (!cs_ensure_room()) { cs_msg("MEMORY IS TOO LOW"); return 0; }
        err = cs_eval_formula(buf, &val, &str);
        if (err == KW_ABORT) return 0;      /* interrupt already reported */
        if (err) {
            csn_zero(&val);
            cs_set_num(c, buf, &val, 1, NULL);
            /* message with hit-space behaviour handled by main loop */
        } else {
            cs_set_num(c, buf, &val, 0, str);
            if (str) free(str);
        }
        cs_run_recalc_passes(0);
        return err;
    }
    if (!cs_ensure_room()) { cs_msg("MEMORY IS TOO LOW"); return 0; }
    cs_set_text(c, buf, 'L');
    return 0;
}
