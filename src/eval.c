/* eval.c - recalculation, entry commitment (BCD arithmetic) */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "cs.h"

int cs_is_inputf(const Cell *c);

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

int cs_recalc_entry(int col, int rw)
{
    Cell *c = cs_cell(col, rw);
    Csn val;
    char *str = NULL;
    int err;
    if (!c || c->type != CE_NUM || !c->cont) return 0;
    if (cs_is_inputf(c)) return 0;      /* recalculate does not apply */
    err = cs_eval_formula(c->cont, &val, &str);
    if (c->strres) { free(c->strres); c->strres = NULL; }
    csn_zero(&c->val);
    if (err) {
        c->iserr = 1;
        errq_add(col, rw, err);         /* queue for ";R" replay */
    } else {
        csn_copy(&c->val, &val);
        c->iserr = 0;
        if (str) c->strres = str;
    }
    return err;
}

void cs_recalc_all(void)
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

/* does the cell hold an '!expr!' input formula? */
int cs_is_inputf(const Cell *c)
{
    int n = (int) strlen(c->cont);
    return n > 2 && c->cont[0] == '!' && c->cont[n - 1] == '!';
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
        if (err) cs_msg("Error %d-> %s", err <= 0 ? 0 : err, tmp);
        else if (str) {
            cs_msg("%s", str);
            free(str);
        } else {
            /* format with up to 10 decimals, then strip trailing zeros
               for the calculator display (string work only) */
            /* manual 4.1/4.2: every answer contains 12 decimal places */
            csn_fmt_fixed(&val, 12, out, (int) sizeof(out));
            cs_msg("%s", out);
        }
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
        if (err) {
            csn_zero(&val);
            cs_set_num(c, buf, &val, 1, NULL);
            /* message with hit-space behaviour handled by main loop */
        } else {
            cs_set_num(c, buf, &val, 0, str);
            if (str) free(str);
        }
        return err;
    }
    if (!cs_ensure_room()) { cs_msg("MEMORY IS TOO LOW"); return 0; }
    cs_set_text(c, buf, 'L');
    return 0;
}
