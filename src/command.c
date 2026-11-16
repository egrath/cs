/* command.c - the ';' command set: Auto Copy Delete Edge Format Goto Help
               Insert Load Merge Order Print Quit Recalc Save What ? = *   */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <curses.h>
#include "cs.h"

/* helpers defined in other modules */
extern void cs_help_cmd(void);      /* help.c */
extern void cs_print_cmd(void);     /* print.c */
extern int  cs_save_dialog(void);   /* fileio.c */
extern int  cs_load_dialog(int merge); /* fileio.c */
extern int  cs_file_password(const char *path, char *pw, int max);
extern int  cs_prompt_hidden(const char *pr, char *out, int max);

static void redraw(void);
void cs_command_menu(void);   /* forward: auto entry loop needs it */

static const char *collab(const char *w, int col, const char *rest);

void cs_goto_cmd(void)
{
    char buf[32];
    int c, r, gc2, gr2;
    if (cs_prompt_coord("Goto>", "A1", buf, (int) sizeof(buf), 1) < 0) return;
    if (!cs_coord_text(buf, &c, &r, &gc2, &gr2)) { cs_msg("Bad coord"); return; }
    curCol = c; curRow = r;
    cs_stay_visible();   /* scrolls only when the target is off-window
                            (real CalcStar verified: in-window Goto
                            target leaves the window untouched) */
    redraw();
}

/* ============================ helpers ============================== */
/* the snapshot: one CHUNK PER ROW (each at most 127 cells ≈ 14 KB —
 * the no-allocation-over-64-KiB rule; the old single block could
 * reach ~400 KB for a full-sheet sort).  Row i holds columns
 * c1..c2; the strings are cloned. */
/* the SPARSE SNAPSHOT (the user's memory report, 2026-11-16): the
 * old snapshot was a full rectangular matrix of Cell structs
 * (255 x 127 = 32,385 structs, ~3.6 MB - unsuitable for the XT
 * target); the snapshot now holds only the POPULATED cells with
 * their relative coordinates, bounded by the pool budget by
 * design.  Used only for OVERLAPPING copies; the fill needs no
 * snapshot at all (its writes never touch the source block). */
struct snap_ent {
    unsigned char rc, rr;       /* the relative col/row */
    Cell c;                     /* the data (the strings strdup'd) */
};

static struct snap_ent *clone_snap(int c1, int r1, int c2, int r2,
                                   int *nret)
{
    int sh = c2 - c1 + 1, sv = r2 - r1 + 1;
    int i, j, cap, n = 0, fail_i = -1, fail_j = -1;
    struct snap_ent *snap;
    /* the caller's signals: n = -1 -> the allocation failed;
     * n = 0 with NULL -> a WHOLLY BLANK source (a legitimate empty
     * snapshot: the copy then clears the overlapping targets) */
    *nret = -1;
    /* the counting pass, then the EXACT allocation */
    cap = 0;
    for (i = 0; i < sv; i++)
        for (j = 0; j < sh; j++)
            if (cs_getcell(c1 + j, r1 + i)) cap++;
    if (cap == 0) { *nret = 0; return NULL; }
    snap = (struct snap_ent *) malloc((size_t) cap *
                                      sizeof(struct snap_ent));
    if (!snap) { *nret = -1; return NULL; }
    for (i = 0; i < sv; i++) {
        for (j = 0; j < sh; j++) {
            Cell *cl = cs_getcell(c1 + j, r1 + i);
            struct snap_ent *se;
            if (!cl) continue;
            se = &snap[n];
            se->rc = (unsigned char) j;
            se->rr = (unsigned char) i;
            se->c = *cl;                        /* the data copy */
            se->c.cont = NULL; se->c.rep = NULL; se->c.strres = NULL;
            n++;
            if (cl->cont) {
                se->c.cont = cs_strdup(cl->cont);
                if (!se->c.cont) { fail_i = i; fail_j = j; goto fail; }
            }
            if (cl->rep) {
                se->c.rep = cs_strdup(cl->rep);
                if (!se->c.rep) { fail_i = i; fail_j = j; goto fail; }
            }
            if (cl->strres) {
                se->c.strres = cs_strdup(cl->strres);
                if (!se->c.strres) { fail_i = i; fail_j = j; goto fail; }
            }
        }
    }
    *nret = n;
    return snap;
fail:
    /* the unwind frees EXACTLY what was allocated: the complete
     * entries before (fail_i, fail_j), and the CURRENT entry's
     * already-duplicated strings (the fields are NULLed before the
     * assignment, so a non-NULL field is ours to free) - the old
     * dense version leaked the current cell (the reviewer's find) */
    for (i = 0; i < n; i++) {
        if (i == fail_i * 0) break;     /* (placeholder, replaced below) */
    }
    for (i = 0; i < n; i++) {
        int ecol = (int) snap[i].rc, erow = (int) snap[i].rr;
        int cur = (erow == fail_i && ecol == fail_j);
        int prior = (erow < fail_i) ||
                    (erow == fail_i && ecol < fail_j);
        if (fail_i < 0 || prior || cur) {
            free(snap[i].c.cont); free(snap[i].c.rep);
            free(snap[i].c.strres);
        }
        if (cur) break;
    }
    free(snap);
    return NULL;
}

/* the sparse lookup: the snapshot's cell at (rc, rr), or NULL when
 * the source position was blank */
static Cell *snap_lookup(struct snap_ent *snap, int n, int rc, int rr)
{
    /* the snapshot is row-major: binary search */
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (snap[mid].rr < rr ||
            (snap[mid].rr == rr && snap[mid].rc < rc))
            lo = mid + 1;
        else if (snap[mid].rr > rr ||
                 (snap[mid].rr == rr && snap[mid].rc > rc))
            hi = mid - 1;
        else return &snap[mid].c;
    }
    return NULL;
}

static void free_snap(struct snap_ent *snap, int n)
{
    int i;
    if (!snap) return;
    for (i = 0; i < n; i++) {
        free(snap[i].c.cont); free(snap[i].c.rep);
        free(snap[i].c.strres);
    }
    free(snap);
}

static void copy_one(Cell *dst, const Cell *src, int rel, int dc, int dr)
{
    Csn val;
    char *str = NULL;
    char *use;
    int err;
    if (src->type == CE_NUM && src->cont) {
        if (cs_is_inputf(src)) {
            /* the !...! value holder: verbatim - evaluating it as an
             * expression was the old bug (an error cell) */
            char *use = cs_strdup(src->cont);
            if (!use) return;
            cs_free_cell(dst);
            dst->type = CE_NUM;
            dst->just = src->just;
            dst->volatf = src->volatf;
            dst->cont = use;
            csn_copy(&dst->val, &src->val);  /* the stored value */
            dst->iserr = src->iserr;
            dst->prec = src->prec;
            dst->notation = src->notation;
            dst->formf = src->formf;
            dst->factf = 0;
            dst->strres = NULL;
            return;
        }
        if (src->factf) {
            /* the factorial entry: the prefix (with re-shifted
             * references) re-evaluates, FACT of it, the flag carries
             * - the old code evaluated "5!" as an ordinary formula
             * and produced an error cell */
            char pfx[CS_EDITMAX + 1];
            size_t ul;
            Csn val, srcval, zero;
            char *use = rel ? cs_adjust_refs(src->cont, dc, dr)
                            : cs_strdup(src->cont);
            int e, fe;
            if (!use) return;
            ul = strlen(use);
            /* the BUFFER GUARD (the user's report, 2026-11-16): a
             * LOADED factf cell can carry content longer than the
             * editor types - the memcpy would overflow; an unusable
             * prefix copies as an error cell */
            if (ul < 2 || ul > CS_EDITMAX + 1 || use[ul - 1] != '!') {
                free(use);
                cs_free_cell(dst);
                dst->type = CE_NUM;
                dst->iserr = 1;
                csn_zero(&dst->val);
                dst->cont = NULL;
                dst->factf = 1;
                return;
            }
            memcpy(pfx, use, ul - 1);
            pfx[ul - 1] = '\0';
            e = cs_eval_fact_source(pfx, &srcval);
            fe = 0;
            if (!e) fe = csn_fact(&val, &srcval);
            cs_free_cell(dst);
            dst->type = CE_NUM;
            dst->just = src->just;
            dst->volatf = src->volatf;
            dst->cont = use;
            csn_zero(&zero);
            csn_copy(&dst->val, (e || fe) ? &zero : &val);
            dst->iserr = (e || fe) ? 1 : 0;
            dst->prec = src->prec;
            dst->notation = src->notation;
            dst->formf = src->formf;
            dst->factf = 1;
            dst->strres = NULL;
            return;
        }
        if (rel && !cs_is_inputf(src))
            use = cs_adjust_refs(src->cont, dc, dr);
        else
            use = cs_strdup(src->cont);
        if (!use) return;
        err = cs_eval_formula(use, &val, &str);
        cs_free_cell(dst);
        dst->type = CE_NUM;
        dst->just = src->just;    /* the alignment carries (the string
                                   * result's /r /l /c) */
        dst->volatf = src->volatf;
        dst->cont = use;
        csn_copy(&dst->val, &val);       /* zeroed by eval on error */
        dst->iserr = err ? 1 : 0;
        dst->prec = src->prec;
        dst->notation = src->notation;
        dst->formf = src->formf;
        dst->factf = 0;
        dst->strres = str;
    } else if (src->type == CE_TEXT) {
        /* the allocate-first discipline (review item 6): a failed
         * strdup leaves the destination UNCHANGED */
        char *nc = cs_strdup(src->cont ? src->cont : "");
        char *nr = (src->just == 'W' && src->rep) ? cs_strdup(src->rep)
                                                  : NULL;
        if (!nc || ((src->just == 'W' && src->rep) && !nr)) {
            free(nc);
            free(nr);
            return;
        }
        cs_free_cell(dst);
        dst->type = CE_TEXT;
        dst->just = src->just;
        dst->volatf = src->volatf;
        dst->cont = nc;
        dst->rep = nr;
        dst->prec = src->prec;
        dst->notation = src->notation;
        dst->formf = src->formf;
        dst->factf = 0;
        csn_zero(&dst->val); dst->iserr = 0;
        dst->strres = NULL;
    } else {
        /* the ALLOCATED-EMPTY state (a real form-mode flag): the
         * complete flag set carries, no content */
        cs_free_cell(dst);
        dst->type = CE_ALLOC;
        dst->just = src->just;
        dst->volatf = 0;
        dst->cont = NULL;
        dst->prec = src->prec;
        dst->notation = src->notation;
        dst->formf = src->formf;
        dst->factf = 0;
        csn_zero(&dst->val); dst->iserr = 0;
        dst->strres = NULL;
    }
}

/* ============================= Copy ================================ */
static void copy_cmd(void)
{
    char from[40], to[40];
    int c1, r1, c2, r2, d1, e1, d2, e2, sw, sh, dw, dh, rel, rep, nfill;
    int i;

    if (cs_prompt_coord("from coord (>coord)", "", from, (int) sizeof(from), 0) < 0) return;
    if (cs_prompt_coord("to coord (>coord)", "", to, (int) sizeof(to), 0) < 0) return;

    /* rep: 1x1 source repeated across a destination range (manual 5.9
     * "from one cell into a range of cells") */
    rep = 0; nfill = 0;
    if (cs_coord_text(from, &c1, &r1, &c2, &r2) && (c2 != c1 || r2 != r1)) {
        sw = c2 - c1 + 1; sh = r2 - r1 + 1;
        if (strchr(to, '>')) {
            if (!cs_coord_text(to, &d1, &e1, &d2, &e2)) { cs_msg("bad range coord"); return; }
            dw = d2 - d1 + 1; dh = e2 - e1 + 1;
            if (sw == 1 && sh == 1) {
                rep = 1;
                nfill = dw * dh;
            } else if (dw == sw && dh == sh) {
                nfill = dw * dh;
            } else if (dw * dh == sw * sh) {
                /* equal counts but different geometry: the copy is not
                 * in a straight vertical or straight horizontal line */
                cs_msg("not along row/column"); return;
            } else {
                cs_msg("sizes don't agree");
                return;
            }
        } else {
            if (!cs_parse_coord(to, &d1, &e1)) { cs_msg("bad coord"); return; }
            d2 = d1 + sw - 1; e2 = e1 + sh - 1;
            nfill = sw * sh;
        }
    } else if (cs_coord_text(from, &c1, &r1, &c2, &r2) && c2 == c1 && r2 == r1) {
        c2 = c1; r2 = r1; sw = 1; sh = 1;
        if (strchr(to, '>')) {
            if (!cs_coord_text(to, &d1, &e1, &d2, &e2)) { cs_msg("bad range coord"); return; }
            rep = 1;
            nfill = (d2 - d1 + 1) * (e2 - e1 + 1);
        } else {
            if (!cs_coord_text(to, &d1, &e1, &d2, &e2)) { cs_msg("bad coord"); return; }
            d2 = d1; e2 = e1; nfill = 1;
        }
    } else { cs_msg("bad coord"); return; }

    if (rep) { dw = d2 - d1 + 1; dh = e2 - e1 + 1; }
    else     { dw = d2 - d1 + 1; dh = e2 - e1 + 1; }
    if (d2 >= CS_MAXCOL || e2 >= CS_MAXROW || d1 < 0 || e1 < 0) {
        cs_msg("bad coord"); return;
    }
    if (!rep && nfill <= 0) nfill = 1;
    if (d2 >= CS_MAXCOL || e2 >= CS_MAXROW || d1 < 0 || e1 < 0) {
        cs_msg("bad coord"); return;
    }
    rel = 0;
    for (i = 0; i < (rep ? 1 : sw * sh); i++) {
        Cell *s = cs_getcell(c1 + i % sw, r1 + i / sh);
        if (s && s->type == CE_NUM) {
            int ch = cs_prompt_char("R)elative or N)o adjustment");
            redraw();
            if (ch == 27) return;
            rel = (ch == 'R' || ch == 'r');
            break;
        }
    }
    /* the straight range-to-range copy (the same column span or the
     * same row span): the last question is fill vs single (user
     * request 2026-11-14) - a fill chains the source through every
     * cell between the source and the destination */
    if (sw * sh > 1 && ((c1 == d1 && c2 == d2) || (r1 == e1 && r2 == e2))) {
        int fill = 0;
        for (;;) {
            int ch2 = cs_prompt_char("F)ill or D)estination");
            redraw();
            if (ch2 == 27) return;
            if (ch2 == 'F' || ch2 == 'f') { fill = 1; break; }
            if (ch2 == 'D' || ch2 == 'd') { fill = 0; break; }
        }
        if (fill) {
            cs_fill_block(c1, r1, c2, r2, d1, e1, d2, e2, rel);
            redraw();
            return;
        }
    }
    cs_copy_block(c1, r1, c2, r2, d1, e1, dw, dh, rep, nfill, rel);
    redraw();
}

/* the copy core (shared with the --test pins): the source block
 * (c1,r1)-(c2,r2), the destination's top-left (d1,e1) with the tile
 * dimensions dw/dh, rep/nfill and the relative mode already resolved */
void cs_copy_block(int c1, int r1, int c2, int r2, int d1, int e1,
                   int dw, int dh, int rep, int nfill, int rel)
{
    int sw = c2 - c1 + 1, i, n;
    int d2c = d1 + dw - 1, e2r = e1 + dh - 1;
    int overlap = (d1 <= c2 && d2c >= c1 && e1 <= r2 && e2r >= r1);
    struct snap_ent *tmp = NULL;
    /* the snapshot exists ONLY for an overlapping copy (the write of
     * a target would destroy a not-yet-copied source cell); a
     * non-overlapping copy reads the source cells LIVE.  The
     * snapshot is SPARSE: the populated cells with their relative
     * coordinates - the old full rectangle cost 3.4 MB, empties
     * included (the user's memory report, 2026-11-16) */
    if (overlap) {
        tmp = clone_snap(c1, r1, c2, r2, &n);
        if (!tmp && n < 0) { cs_msg("MEMORY IS TOO LOW"); return; }
        /* n == 0: a wholly blank source - the lookups return NULL and
         * the blank policy clears the overlapping targets */
    } else
        n = (r2 - r1 + 1) * sw;
    /* the BUDGET PRE-CHECK (the user's memory report, item E): the
     * net growth = the populated sources - the populated
     * destinations; the copy refuses UPFRONT instead of dying
     * halfway through (a partial copy left both ranges mangled) */
    {
        int psn = 0, pdn = 0, r2c, c2c, r, c;
        if (rep)
            psn = cs_getcell(c1, r1) ? 1 : 0;
        else
            for (r = r1; r <= r2; r++)
                for (c = c1; c <= c2; c++)
                    if (cs_getcell(c, r)) psn++;
        r2c = e1 + dh - 1; c2c = d1 + dw - 1;
        for (r = e1; r <= r2c; r++)
            for (c = d1; c <= c2c; c++)
                if (cs_getcell(c, r)) pdn++;
        if (entriesUsed - pdn + psn > CS_BUDGET) {
            cs_msg("MEMORY IS TOO LOW"); return;
        }
    }
    for (i = 0; i < (rep ? nfill : n); i++) {
        int sc = rep ? c1 : (c1 + i % sw);
        int sr = rep ? r1 : (r1 + i / sw);
        int dc = d1 + i % dw, dr = e1 + i / dw;
        Cell *src, esrc;
        Cell *dst;
        if (tmp)
            src = snap_lookup(tmp, n, rep ? 0 : i % sw,
                              rep ? 0 : i / sw);
        else {
            src = cs_getcell(sc, sr);
            if (!src) { memset(&esrc, 0, sizeof(esrc));
                        src = &esrc; }
        }
        /* a BLANK source: the destination ceases to exist - the
         * complete state of "nothing" is nothing (the sparse
         * structure mirrors the source; no budget consumed) */
        if (!src || src->type == CE_EMPTY) {
            cs_free_cell_at(dc, dr);
            continue;
        }
        dst = cs_alloc_cell(dc, dr);
        if (!dst) { cs_msg("MEMORY IS TOO LOW"); break; }
        {
            /* the DESTINATION is the evaluation context (ROW()/COL()):
             * the dc/dr of copy_one are the relative SHIFT deltas */
            int so = cs_eval_ctx_on, sc2 = cs_eval_ctx_col,
                sr2 = cs_eval_ctx_row;
            cs_eval_ctx_on = 1;
            cs_eval_ctx_col = dc;
            cs_eval_ctx_row = dr;
            copy_one(dst, src, rel, dc - sc, dr - sr);
            cs_eval_ctx_on = so; cs_eval_ctx_col = sc2;
            cs_eval_ctx_row = sr2;
        }
    }
    if (tmp) free_snap(tmp, n);
    cs_recount_entries();
}

/* the chained fill (user request 2026-11-14): the source TILED and
 * re-shifted at every intermediate distance, evaluated NEAREST TO THE
 * SOURCE FIRST so relative chains build exactly like a sequence of
 * single-step copies (+A1+1 down a column: 3, 4, 5, ...).  The region
 * is the span between the source and the destination plus the
 * destination block; mirrored for the upward/leftward destinations.
 * The tiling rule is the spreadsheet's: the target takes the source
 * cell at (distance mod the source's own dimension) with the shift =
 * the distance from that source cell. */
static int fill_one(int sw, int sh,
                    int c1, int r1, int cc, int rr, int rel,
                    int vertical)
{
    int sr, sc, dc, dr;
    Cell *dst, *src;
    int so, sc2, sr2;
    if (vertical) {
        sr = r1 + (((rr - r1) % sh) + sh) % sh;   /* the tiled row */
        sc = cc;
        dc = 0;
        dr = rr - sr;
    } else {
        sc = c1 + (((cc - c1) % sw) + sw) % sw;   /* the tiled column */
        sr = rr;
        dc = cc - sc;
        dr = 0;
    }
    /* the LIVE source (the user's memory report, 2026-11-16): the
     * fill's writes NEVER touch the source block (nearest-to-source
     * ordering), so the snapshot was pure waste */
    src = cs_getcell(sc, sr);
    /* a BLANK source: the destination ceases to exist */
    if (!src || src->type == CE_EMPTY) {
        cs_free_cell_at(cc, rr);
        return 1;
    }
    dst = cs_alloc_cell(cc, rr);
    if (!dst) { cs_msg("MEMORY IS TOO LOW"); return 0; }
    so = cs_eval_ctx_on; sc2 = cs_eval_ctx_col; sr2 = cs_eval_ctx_row;
    cs_eval_ctx_on = 1;
    cs_eval_ctx_col = cc;
    cs_eval_ctx_row = rr;
    copy_one(dst, src, rel, dc, dr);
    cs_eval_ctx_on = so; cs_eval_ctx_col = sc2; cs_eval_ctx_row = sr2;
    return 1;
}

void cs_fill_block(int c1, int r1, int c2, int r2, int d1, int e1,
                   int d2, int e2, int rel)
{
    int sw = c2 - c1 + 1, sh = r2 - r1 + 1;
    int vertical = (c1 == d1 && c2 == d2);
    int horizontal = (r1 == e1 && r2 == e2);
    if (vertical == horizontal) return;     /* not straight: no fill */
    /* NO snapshot (the user's memory report, 2026-11-16): the fill's
     * writes never touch the source block, so the sources are read
     * live */
    if (vertical) {
        int rr, cc;
        if (e1 > r2) {                      /* down: the rows after the source */
            for (rr = r2 + 1; rr <= e2; rr++)
                for (cc = c1; cc <= c2; cc++)
                    if (!fill_one(sw, sh, c1, r1, cc, rr, rel, 1))
                        return;
        } else {                            /* up: toward the source */
            for (rr = r1 - 1; rr >= e2; rr--)
                for (cc = c1; cc <= c2; cc++)
                    if (!fill_one(sw, sh, c1, r1, cc, rr, rel, 1))
                        return;
        }
    } else {
        int rr, cc;
        if (d1 > c2) {                      /* right: the columns after the source */
            for (cc = c2 + 1; cc <= d2; cc++)
                for (rr = r1; rr <= r2; rr++)
                    if (!fill_one(sw, sh, c1, r1, cc, rr, rel, 0)) {
                        return;
                    }
        } else {                            /* left: toward the source */
            for (cc = c1 - 1; cc >= d2; cc--)
                for (rr = r1; rr <= r2; rr++)
                    if (!fill_one(sw, sh, c1, r1, cc, rr, rel, 0)) {
                        return;
                    }
        }
    }
    cs_recount_entries();
}

/* ====================== Insert row/column ========================== */
static void shift_all_refs(int iscol, int at, int insert)
{
    int r = 0, c = 0;
    Cell *cl;
    while ((cl = cs_next_cell(&c, &r)) != NULL) {
        char *nu;
        if (!cl->cont) { c = cl->col + 1; continue; }
        nu = cs_shift_refs(cl->cont, iscol, at, insert);
        if (nu) {
            free(cl->cont);
            cl->cont = nu;
        }
        c = cl->col + 1;
    }
}

static void blank_cell_rowcol(int r, int c)
{
    cs_free_cell_at(c, r);
}

static void insert_cmd(void)
{
    int ch, at, r, c;
    ch = cs_prompt_char("Insert: R)ow C)olumn");
    redraw();
    if (ch == 27) return;
    if (ch == 'R' || ch == 'r') {
        at = curRow;
        shift_all_refs(0, at, 1);
        for (r = CS_MAXROW - 1; r > at; r--)
            for (c = 0; c < CS_MAXCOL; c++)
                if (cs_relink_cell(c, r - 1, c, r)) {
                    cs_msg("MEMORY IS TOO LOW"); return;
                }
        for (c = 0; c < CS_MAXCOL; c++) blank_cell_rowcol(at, c);
    } else if (ch == 'C' || ch == 'c') {
        at = curCol;
        shift_all_refs(1, at, 1);
        for (r = 0; r < CS_MAXROW; r++)
            for (c = CS_MAXCOL - 1; c > at; c--)
                if (cs_relink_cell(c - 1, r, c, r)) {
                    cs_msg("MEMORY IS TOO LOW"); return;
                }
        for (r = 0; r < CS_MAXROW; r++) blank_cell_rowcol(r, at);
    } else return;
    redraw();
}

/* ====================== Delete row/column/all ====================== */
static int dep_check(int iscol, int at, char *list, int listmax)
{
    int r = 0, c = 0, found = 0;
    Cell *cl;
    list[0] = '\0';
    while ((cl = cs_next_cell(&c, &r)) != NULL) {
            char buf[10];
            if (iscol ? c == at : r == at) { c = cl->col + 1; continue; }
            if (!cl->cont || cl->type != CE_NUM) { c = cl->col + 1; continue; }
            {
                const char *s = cl->cont;
                int hit = 0, cc, rr, inq = 0;
                while (*s && !hit) {
                    if (*s == '"') { inq = !inq; s++; continue; }
                    if (*s == '\\' && !inq) break;   /* comment: no refs */
                    if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) {
                        const char *st = s;
                        int nl = 0;
                        while ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) { s++; nl++; }
                        if (nl <= 2 && *s >= '0' && *s <= '9') {
                            while (*s >= '0' && *s <= '9') s++;
                            if (*s == '!' || *s == '>' || *s == ',' || *s == ')' ||
                                *s == ' ' || *s == ':' || *s == '+' || *s == '-' ||
                                *s == '*' || *s == '/' || *s == '%' || *s == '\0' ||
                                *s == '<' || *s == '=' || *s == '&') {
                                char tmp[8];
                                int i;
                                for (i = 0; i < (s - st); i++)
                                    tmp[i] = (char) ((st[i] >= 'a' && st[i] <= 'z') ? st[i] - 32 : st[i]);
                                tmp[i] = '\0';
                                if (cs_parse_coord(tmp, &cc, &rr) &&
                                    (iscol ? cc == at : rr == at))
                                    hit = 1;
                            }
                            continue;
                        }
                        s = st + 1;
                        continue;
                    }
                    s++;
                }
                if (hit) {
                    char cn[4];
                    if (list[0] && (int) strlen(list) + 2 < listmax)
                        strcat(list, ", ");
                    cs_col_name(c, cn);
                    sprintf(buf, "%s%d", cn, r + 1);
                    if ((int) strlen(list) + (int) strlen(buf) < listmax - 2)
                        strcat(list, buf);
                    found = 1;
                }
            }
            /* the scan advance: WITHOUT this the loop revisits the same
             * cell forever whenever a formula references the deleted
             * row/column (the hit path) - the reported hang */
            c = cl->col + 1;
        }
    return found;
}

/* delete rows r1..r2 (inclusive): refs below the range shift up by the
 * range height; the window/lock/cursor edges follow the gap */
void cs_delete_rows(int r1, int r2)
{
    int n = r2 - r1 + 1, rr, c;
    if (n < 1) return;
    {
        int r = 0, cc = 0;
        Cell *cl;
        while ((cl = cs_next_cell(&cc, &r)) != NULL) {
            char *nu;
            if (cl->cont) {
                nu = cs_shift_refs_n(cl->cont, 0, r2, -n);
                if (nu) { free(cl->cont); cl->cont = nu; }
            }
            cc = cl->col + 1;
        }
    }
    for (rr = r1; rr < CS_MAXROW - n; rr++)
        for (c = 0; c < CS_MAXCOL; c++)
            if (cs_relink_cell(c, rr + n, c, rr)) {
                cs_msg("MEMORY IS TOO LOW"); return;
            }
    if (winRow > r2) winRow -= n;
    else if (winRow > r1) winRow = r1;
    if (lockRows > r2) lockRows -= n;
    else if (lockRows > r1) lockRows = r1;
    if (curRow > r2) curRow -= n;
    else if (curRow > r1) curRow = r1;
}

/* delete columns c1..c2 (inclusive): the columns' width/precision
 * shift with them (the tail resets to defaults) */
void cs_delete_columns(int c1, int c2)
{
    int n = c2 - c1 + 1, r, cc;
    if (n < 1) return;
    {
        int r = 0, c0 = 0;
        Cell *cl;
        while ((cl = cs_next_cell(&c0, &r)) != NULL) {
            char *nu;
            if (cl->cont) {
                nu = cs_shift_refs_n(cl->cont, 1, c2, -n);
                if (nu) { free(cl->cont); cl->cont = nu; }
            }
            c0 = cl->col + 1;
        }
    }
    for (r = 0; r < CS_MAXROW; r++)
        for (cc = c1; cc < CS_MAXCOL - n; cc++)
            if (cs_relink_cell(cc + n, r, cc, r)) {
                cs_msg("MEMORY IS TOO LOW"); return;
            }
    for (cc = c1; cc < CS_MAXCOL - n; cc++) {
        colw[cc] = colw[cc + n];
        colp[cc] = colp[cc + n];
    }
    for (cc = CS_MAXCOL - n; cc < CS_MAXCOL; cc++) {
        colw[cc] = CS_DEF_W;
        colp[cc] = CS_DEF_P;
    }
    if (winCol > c2) winCol -= n;
    else if (winCol > c1) winCol = c1;
    if (lockCols > c2) lockCols -= n;
    else if (lockCols > c1) lockCols = c1;
    if (curCol > c2) curCol -= n;
    else if (curCol > c1) curCol = c1;
}

/* clear (delete entry) every cell in the rectangle, like ;D E per
 * cell: no dependency check, no ref adjustments */
void cs_clear_range(int c1, int r1, int c2, int r2)
{
    int r, c;
    for (r = r1; r <= r2; r++)
        for (c = c1; c <= c2; c++)
            cs_free_cell_at(c, r);
}

/* one side of a delete-range answer.  want = 1 (rows): a bare row
 * number ("5") or a coordinate ("C3" - the row is taken).  want = 0
 * (columns): a column letter ("B") or a coordinate ("B7" - the column
 * is taken).  The non-wanted part is ignored.  Returns 1 on success
 * with the col/row pointers set (the non-wanted one is -1). */
static int parse_del_side(const char *s, int want, int *col, int *row)
{
    int l = 0, d = 0, i;
    const char *p = s;
    if (!s || !*s) return 0;
    while (((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) && l < 3) { p++; l++; }
    while (*p >= '0' && *p <= '9') { p++; d++; }
    if (*p) return 0;                      /* trailing junk */
    if (l == 0) {                          /* a bare number: a row */
        int r = 0;
        if (!want) return 0;               /* columns need letters */
        for (i = 0; i < d; i++) r = r * 10 + (s[i] - '0');
        if (r < 1 || r > CS_MAXROW) return 0;
        *row = r - 1;
        *col = -1;
        return 1;
    }
    if (l > 2 || d == 0) {                 /* letters only: a column */
        int c = 0;
        if (want) return 0;                /* rows need numbers/coords */
        for (i = 0; i < l; i++) {
            char ch = s[i];
            c = (i == 0) ? ((ch >= 'a' && ch <= 'z') ? ch - 'a' : ch - 'A')
                         : 26 + c * 26 + ((ch >= 'a' && ch <= 'z') ? ch - 'a' : ch - 'A');
        }
        if (c >= CS_MAXCOL) return 0;
        *col = c;
        *row = -1;
        return 1;
    }
    /* letters + digits: a full coordinate - take the wanted part */
    {
        char word[8];
        for (i = 0; i < l; i++)
            word[i] = (char) ((s[i] >= 'a' && s[i] <= 'z') ? s[i] - 32 : s[i]);
        for (i = 0; i < d; i++) word[l + i] = s[l + i];
        word[l + d] = '\0';
        if (!cs_parse_coord(word, col, row)) return 0;
        return 1;
    }
}

/* a delete-range answer for the R)ow / C)olumn prompts: one side or a
 * full "x>y" range; each side yields the WANTED part (rows: a number
 * or a coordinate's row; columns: a letter or a coordinate's column),
 * normalized so a1 <= a2.  On failure the expression evaluator is
 * tried (user-ext 2026-11): a Reference/Range value provides the
 * wanted part.  Returns 1 on success. */
static int parse_del_range(const char *s, int want, int *a1, int *a2);

static int del_range_text(const char *s, int want, int *a1, int *a2)
{
    int c1, r1, c2, r2;
    if (parse_del_range(s, want, a1, a2)) return 1;
    if (cs_eval_range(s, &c1, &r1, &c2, &r2) == 0) {
        *a1 = want ? r1 : c1;
        *a2 = want ? r2 : c2;
        return 1;
    }
    return 0;
}

static int parse_del_range(const char *s, int want, int *a1, int *a2)
{
    int v1, v2, c, r;
    const char *gt;
    if (!s || !*s) return 0;
    gt = strchr(s, '>');
    if (!gt) {
        if (!parse_del_side(s, want, &c, &r)) return 0;
        *a1 = *a2 = want ? r : c;
        return 1;
    }
    {
        char left[40];
        int ln = (int) (gt - s);
        if (ln <= 0 || ln >= (int) sizeof(left)) return 0;
        memcpy(left, s, (size_t) ln);
        left[ln] = '\0';
        if (!parse_del_side(left, want, &c, &r)) return 0;
        v1 = want ? r : c;
        if (!parse_del_side(gt + 1, want, &c, &r)) return 0;
        v2 = want ? r : c;
    }
    if (v1 <= v2) { *a1 = v1; *a2 = v2; }
    else { *a1 = v2; *a2 = v1; }
    return 1;
}

/* the dependency refusal: per manual 14-4, a row/column is not
 * deleted when it contains information used by other cells.  The
 * range check walks every line and merges the coordinate list; the
 * message path is the one the single-line delete always used
 * (user 2026-11: keep this behavior unchanged) */
void dep_check_range(int iscol, int a1, int a2, char *list, int listmax)
{
    char one[128];
    int a;
    list[0] = '\0';
    for (a = a1; a <= a2; a++) {
        if (!dep_check(iscol, a, one, (int) sizeof(one))) continue;
        if (list[0] && (int) strlen(list) + 2 < listmax)
            strcat(list, ", ");
        if ((int) strlen(list) + (int) strlen(one) < listmax - 1)
            strcat(list, one);
    }
}

static void delete_cmd(void)
{
    char list[256], dflt[8], inp[80];
    int ch, c1, r1, c2, r2;
    ch = cs_prompt_char("Delete: A)ll R)ow C)olumn F)rom-To E)ntry");
    redraw();
    if (ch == 27) return;
    if (ch == 'E' || ch == 'e') {
        cs_free_cell_at(curCol, curRow);
        redraw();
        return;
    }
    if (ch == 'R' || ch == 'r') {
        int hit;
        sprintf(dflt, "%d", curRow + 1);
        if (cs_prompt_range("Delete rows (From>To)", dflt, inp, (int) sizeof(inp)) < 0) return;
        redraw();
        if (!del_range_text(inp, 1, &r1, &r2))
            { cs_msg("bad range coord"); return; }
        dep_check_range(0, r1, r2, list, (int) sizeof(list));
        hit = (list[0] != '\0');
        if (hit)
            { cs_msg("ERROR: would delete ref(s) at %s", list); return; }
        ch = cs_prompt_yn("verify Y/N - ");
        redraw();
        if (ch != 1) return;
        cs_delete_rows(r1, r2);
    } else if (ch == 'C' || ch == 'c') {
        int hit;
        cs_col_name(curCol, dflt);
        if (cs_prompt_range("Delete columns (From>To)", dflt, inp, (int) sizeof(inp)) < 0) return;
        redraw();
        if (!del_range_text(inp, 0, &c1, &c2))
            { cs_msg("bad range coord"); return; }
        dep_check_range(1, c1, c2, list, (int) sizeof(list));
        hit = (list[0] != '\0');
        if (hit)
            { cs_msg("ERROR: would delete ref(s) at %s", list); return; }
        ch = cs_prompt_yn("verify Y/N - ");
        redraw();
        if (ch != 1) return;
        cs_delete_columns(c1, c2);
    } else if (ch == 'F' || ch == 'f') {
        cs_col_name(curCol, dflt);
        sprintf(dflt + strlen(dflt), "%d", curRow + 1);
        if (cs_prompt_coord("Delete (From>To)", dflt, inp, (int) sizeof(inp), 2) < 0) return;
        redraw();
        if (!cs_coord_text(inp, &c1, &r1, &c2, &r2)) {
            cs_msg("bad range coord"); return;
        }
        cs_range_norm(&c1, &r1, &c2, &r2);
        /* the manual's E)ntry case checks no dependencies and asks no
         * verification; the bulk clear asks for verify only (user
         * decision 2026-11) */
        ch = cs_prompt_yn("verify Y/N - ");
        redraw();
        if (ch != 1) return;
        cs_clear_range(c1, r1, c2, r2);
    } else if (ch == 'A' || ch == 'a') {
        ch = cs_prompt_yn("verify Y/N - ");
        redraw();
        if (ch != 1) return;
        cs_clear_all();
        fname[0] = '\0';
        curCol = curRow = winCol = winRow = 0;
        lockCols = lockRows = 0;
    } else return;
    redraw();
}

/* ============================ Format =============================== */
static void format_cmd(void)
{
    int ch;
    char pw[24], tmpb[24];
    char label[88];
    /* an ABSENT cell reads as unformatted: the per-cell attributes
     * allocate the cell only when one is actually SET (the sparse
     * store's get returns NULL - the dense slot was never NULL) */
    Cell *cur = cs_getcell(curCol, curRow);
    int notn = cur ? cur->notation : 0;
    int form = cur ? cur->formf : 0;

    sprintf(pw, "%d", colw[curCol]);
    sprintf(label,
        "P)recision (%d) or N)otation (%s) or W)idth (%d) or F)orm mode (%s)",
        colp[curCol],
        notn == 1 ? "sci" : notn == 2 ? "dec" : "auto",
        colw[curCol], form ? "set" : "clear");
    ch = cs_prompt_char(label);
    redraw();
    if (ch == 27) return;
    if (ch == 'N' || ch == 'n') {
        /* the notation cycle (flagged ext 2026-11):
         * (auto) -> (dec) -> (sci) -> (auto) ...; the encoding is
         * 0=auto 1=sci 2=dec, so the step is +2 mod 3 */
        if (!cur) { cur = cs_alloc_cell(curCol, curRow);
                    if (!cur) { cs_msg("can't create"); return; } }
        cur->notation = (cur->notation + 2) % 3;
        if (cur->notation != 0 && cur->type == CE_EMPTY) {
            /* the attribute lives on the cell (like the per-cell
             * precision): manual "can't create - not enough memory" */
            if (!cs_ensure_room()) { cs_msg("can't create"); return; }
            cur->type = CE_ALLOC;
            cs_recount_entries();
        }
        redraw();
    } else if (ch == 'W' || ch == 'w') {
        int w;
        if (cs_prompt_str(collab("Column", curCol, "width(3 - 63)"), pw, tmpb, 20) < 0) return;
        w = atoi(tmpb);
        if (w < 3 || w > 63) { cs_msg("Width must be 3 - 63"); return; }
        colw[curCol] = w;
        cs_stay_visible();   /* a widened column may no longer fit */
    } else if (ch == 'P' || ch == 'p') {
        int p;
        sprintf(pw, "%d", colp[curCol]);
        if (cs_prompt_str(collab("Column", curCol, "precision(0 - 24)"), pw, tmpb, 20) < 0) return;
        if (tmpb[0] == 'E' || tmpb[0] == 'e') {
            p = atoi(tmpb + 1);
            if (p < 0 || p > CS_MAX_P) { cs_msg("Prec. must be 0 - 24"); return; }
            if (!cur) { cur = cs_alloc_cell(curCol, curRow);
                        if (!cur) { cs_msg("can't create"); return; } }
            if (cur->type == CE_EMPTY) {
                /* per-cell format needs the cell: manual "can't create -
                 * not enough memory to set format precision" */
                cur->type = CE_ALLOC;
                cs_recount_entries();
            }
            cur->prec = p;
        } else {
            p = atoi(tmpb);
            if (p < 0 || p > CS_MAX_P) { cs_msg("Prec. must be 0 - 24"); return; }
            colp[curCol] = p;
        }
    } else if (ch == 'F' || ch == 'f') {
        if (!cur) { cur = cs_alloc_cell(curCol, curRow);
                    if (!cur) { cs_msg("can't create"); return; } }
        cur->formf = !cur->formf;
        if (cur->formf) {
            if (cur->type == CE_EMPTY) {
                /* mark the cell so the type line reads
                 * "(empty, but allocated)" — manual 11.3 form mode */
                cur->type = CE_ALLOC;
                cs_recount_entries();
            }
            cs_msg("Form mode (set)");
        } else {
            if (cur->type == CE_ALLOC) {
                cs_free_cell(cur);      /* releases it back to CE_EMPTY */
                cs_recount_entries();
            }
            cs_msg("Form mode (clear)");
        }
        return;
    } else return;
    redraw();
}

static const char *collab(const char *w, int col, const char *rest)
{
    static char buf[40];
    char cn[4];
    cs_col_name(col, cn);
    sprintf(buf, "%s %s %s", w, cn, rest);
    return buf;
}

/* ========================= Recalculate ============================= */
static void recalc_cmd(void)
{
    int ch = cs_prompt_char("Recalculate - A)ll or E)ntry");
    redraw();
    if (ch == 27) return;
    cs_recalc_limit_reset();          /* fresh overrun state per run */
    if (ch == 'A' || ch == 'a')
        cs_recalc_all();
    else if (ch == 'E' || ch == 'e') {
        cs_nerrlist = 0;              /* single-cell run: fresh queue */
        /* the explicit re-roll: a volatile cell under the cursor
         * redraws (the one draw-on context beside RECALC(cell)) */
        cs_volatile_draw_set(1);
        cs_recalc_entry(curCol, curRow);
        cs_volatile_draw_set(0);
    }
    else return;
    redraw();
    if (cs_nerrlist) {
        /* the manual's error replay: the ?n? cells report one after
         * another, confirmed with Space (see main.c walker) */
        {
            Cell *ec2 = cs_getcell(cs_errlist[0].col, cs_errlist[0].row);
            cs_sticky_err(cs_errlist[0].code,
                          ec2 ? ec2->cont : "",
                          cs_errlist[0].col, cs_errlist[0].row);
        }
        cs_set_errpend(1);
    }
    if (cs_recalc_limit_hit())
        /* the driver painted this before our redraws wiped it: the
         * overrun must be visible on the resting screen (flagged) */
        cs_msg("Recalculation limit reached");
}

/* ======================= What / Lock / misc ======================== */
/* the heading content of one cell as the What line shows it (flagged
 * extension 2026-11): text as typed, numbers in the display format of
 * their column, input formulas as "In!" (the window's token), and the
 * manual's "---" when there is no heading at that coordinate */
static void what_heading(const Cell *c, int col, char *out, int outn)
{
    out[0] = '\0';
    if (!c || !c->cont || !c->cont[0]) { strcpy(out, "---"); return; }
    if (c->type == CE_TEXT) {
        strncpy(out, c->cont, outn - 1);
        out[outn - 1] = '\0';
        return;
    }
    if (c->type == CE_NUM) {
        char buf[64];
        int prec;
        if (cs_is_inputf(c)) { strcpy(out, "In!"); return; }
        /* a string result is the value (the derived type) */
        if (c->strres) {
            strncpy(out, c->strres, outn - 1);
            out[outn - 1] = '\0';
            return;
        }
        prec = (c->prec >= 0) ? c->prec : colp[col];
        cs_fmt_num(&c->val, 20, prec, c->iserr, c->notation, buf,
                    (int) sizeof(buf));
        strncpy(out, buf, outn - 1);
        out[outn - 1] = '\0';
        return;
    }
    strcpy(out, "---");
}

static void what_cmd(void)
{
    /* the ROW heading lives in column A at the cursor's row; the
     * COLUMN heading lives in row 1 at the cursor's column (the two
     * reads were swapped: ";W" printed "A1,-" for an empty A7) */
    char rt[80], ct[80];
    what_heading(cs_getcell(0, curRow), 0, rt, (int) sizeof(rt));
    what_heading(cs_getcell(curCol, 0), curCol, ct, (int) sizeof(ct));
    cs_msg("row, column  = %s,%s", rt, ct);
}

void cs_lock_cmd(void)
{
    int ch = cs_prompt_char("Lock: R)ow C)ol B)oth");
    redraw();
    if (ch == 27) return;
    if (ch == 'B' || ch == 'b') {
        if (lockRows || lockCols) { lockRows = 0; lockCols = 0; }
        else { lockRows = curRow + 1; lockCols = curCol + 1; }
    } else if (ch == 'R' || ch == 'r')
        lockRows = lockRows ? 0 : curRow + 1;   /* lock rows 1..cursor row */
    else if (ch == 'C' || ch == 'c')
        lockCols = lockCols ? 0 : curCol + 1;   /* cols A..cursor */
    else return;
    redraw();
}

/* ============================== Sort ================================ */
/* ;# (user-ext 2026-11): reorder the rows of a rectangular range by
 * one key column, ascending or descending.  Each row carries its
 * cells within the range's columns; formulas anywhere in the sheet
 * get their in-range references remapped to the rows where the cells
 * landed (frozen '!' refs excepted, like the insert/delete shifter).
 * Empty keys sort last in both orders; the sort is stable. */
/* swap the two cells at (c, ra) and (c, rb): ra's content is saved
 * (its strings duplicated, since the relink frees the target), the
 * rb slot relinks into ra, and the saved content lands in rb.  An
 * absent side simply ends up empty. */
static void swap_two_cells(int c, int ra, int rb)
{
    Cell *a = cs_getcell(c, ra);
    Cell t;
    if (a) {
        t = *a;
        t.cont = a->cont ? cs_strdup(a->cont) : NULL;
        t.rep = a->rep ? cs_strdup(a->rep) : NULL;
        t.strres = a->strres ? cs_strdup(a->strres) : NULL;
        /* the allocate-first discipline (review item 6): a failed
         * strdup aborts the swap BEFORE anything moves - the sheet
         * is untouched */
        if ((a->cont && !t.cont) || (a->rep && !t.rep) ||
            (a->strres && !t.strres)) {
            free(t.cont); free(t.rep); free(t.strres);
            return;
        }
    } else
        memset(&t, 0, sizeof(t));
    if (cs_relink_cell(c, rb, c, ra)) {   /* rb's slot moves into ra */
        /* the reserve-first relink failed before ANY mutation: the
         * sheet is untouched - free the strdup temps and abort */
        free(t.cont); free(t.rep); free(t.strres);
        return;
    }
    if (t.type != CE_EMPTY) {
        Cell *dst = cs_alloc_cell(c, rb);
        if (dst) {
            *dst = t;
            dst->col = (unsigned char) c;
            dst->row = (unsigned char) rb;
            if (dst->type != CE_EMPTY) entriesUsed++;
        }
    }
}

void cs_sort_range(int c1, int r1, int c2, int r2, int keycol, int desc)
{
    int nrows = r2 - r1 + 1, ncols = c2 - c1 + 1;
    int i, j, ne, ne0;
    int *order, *nonempty, *empties, *perm;
    Cell *kc;

    if (nrows <= 1 || ncols <= 0 || keycol < c1 || keycol > c2)
        return;     /* one row has nothing to permute (defensive) */
    order = (int *) malloc((size_t) nrows * sizeof(int));
    nonempty = (int *) malloc((size_t) nrows * sizeof(int));
    empties = (int *) malloc((size_t) nrows * sizeof(int));
    perm = (int *) malloc((size_t) nrows * sizeof(int));
    if (!order || !nonempty || !empties || !perm) {
        free(order); free(nonempty); free(empties); free(perm);
        return;
    }
    /* partition: rows with a usable key sort first, empty-key rows
     * keep their relative order and go last (both directions) - the
     * keys are read from the LIVE cells (nothing changes until the
     * permutation is applied; NO snapshot of the rectangle - the
     * whole-sheet sort used to transiently copy all 32,385 slots,
     * empties included, user report 2026-11-15) */
    ne = ne0 = 0;
    for (i = 0; i < nrows; i++) {
        kc = cs_getcell(keycol, r1 + i);
        /* an ABSENT key cell reads as empty (the sparse store's
         * NULL return - the dense slot was never NULL) */
        if (!kc) empties[ne0++] = i;
        /* a string-result formula cell is a TEXT key (the derived
         * type, 2026-11-13) */
        else if (kc->type == CE_NUM && !kc->iserr && !kc->strres)
            nonempty[ne++] = i;
        else if (kc->type == CE_TEXT && kc->cont && kc->cont[0])
            nonempty[ne++] = i;
        else if (kc->strres && !kc->iserr) nonempty[ne++] = i;
        else empties[ne0++] = i;
    }
    /* stable insertion sort on the key.  ONE string class: ordinary
     * text and a formula's string result compare through the SAME
     * byte order - the old three-class version returned +1 in BOTH
     * directions across the text/strres boundary (an inconsistent
     * comparator, the user's 2026-11-16 report) */
    for (i = 1; i < ne; i++) {
        int t = nonempty[i];
        kc = cs_getcell(keycol, r1 + t);
        for (j = i - 1; j >= 0; j--) {
            Cell *k2 = cs_getcell(keycol, r1 + nonempty[j]);
            int cv;
            int c1s = !(kc->type == CE_NUM && !kc->iserr && !kc->strres);
            int c2s = !(k2->type == CE_NUM && !k2->iserr && !k2->strres);
            if (c1s != c2s) cv = c1s ? 1 : -1;   /* numbers first */
            else if (!c1s) cv = csn_cmp(&kc->val, &k2->val);
            else {
                const char *sa = (kc->type == CE_TEXT) ? kc->cont
                                                       : kc->strres;
                const char *sb = (k2->type == CE_TEXT) ? k2->cont
                                                       : k2->strres;
                cv = strcmp(sa, sb);
            }
            if (desc ? cv <= 0 : cv >= 0) break;
            nonempty[j + 1] = nonempty[j];
        }
        nonempty[j + 1] = t;
    }
    for (i = 0; i < ne; i++) order[i] = nonempty[i];
    for (i = 0; i < ne0; i++) order[ne + i] = empties[i];
    /* the permutation: perm[si] = the destination row of source row si */
    for (i = 0; i < nrows; i++) perm[order[i]] = r1 + i;   /* absolute rows */
    /* apply the permutation IN PLACE by swapping row pairs: no
     * snapshot at all - each swap relinks the cells through the row
     * index (the slots never move, the released targets are freed by
     * the relink, and an absent side ends up empty).  The swaps fix
     * the perm entries, so the ORIGINAL permutation is copied for
     * the reference remap below. */
    {
        int *perm0 = (int *) malloc((size_t) nrows * sizeof(int));
        int r;
        if (!perm0) { free(order); free(nonempty); free(empties);
                      free(perm); return; }
        memcpy(perm0, perm, (size_t) nrows * sizeof(int));
        for (r = 0; r < nrows; r++) {
            while (perm[r] != r1 + r) {
                int jj = perm[r] - r1, c;
                for (c = c1; c <= c2; c++)
                    swap_two_cells(c, r1 + r, r1 + jj);
                { int t2 = perm[r]; perm[r] = perm[jj]; perm[jj] = t2; }
            }
        }
        /* the formulas: remap in-range references to the new rows (the
         * scan locals must NOT shadow the range bounds r2/c2) */
        {
            int sr2 = 0, sc2 = 0;
            Cell *cl;
            while ((cl = cs_next_cell(&sc2, &sr2)) != NULL) {
                char *nc2;
                if (cl->cont && cl->type != CE_EMPTY) {
                    nc2 = cs_sort_refs(cl->cont, c1, r1, c2, r2, perm0);
                    if (nc2) { free(cl->cont); cl->cont = nc2; }
                }
                sc2 = cl->col + 1;
            }
        }
        free(perm0);
    }
    free(order); free(nonempty); free(empties); free(perm);
    cs_recount_entries();
    cs_mark_sheet_change();
}

static void sort_cmd(void)
{
    char buf[80];
    int c1, r1, c2, r2, kc, err, ch, desc;
    if (cs_prompt_coord("Range? ", "", buf, (int) sizeof(buf), 2) < 0) return;
    redraw();
    if (!cs_coord_text(buf, &c1, &r1, &c2, &r2)) { cs_msg("bad range coord"); return; }
    if (r1 == r2) { cs_msg("bad sort range"); return; }   /* one row: horizontal
                                                           * or 1x1 - nothing
                                                           * to sort (user
                                                           * 2026-11-15) */
    if (c1 == c2) {
        kc = c1 + 1;              /* a single column: the key is implied */
    } else {
        if (cs_prompt_str("Key (Column)? ", "", buf, (int) sizeof(buf)) < 0) return;
        redraw();
        err = 0;
        kc = cs_eval_keycol(buf, &err);
        if (err || kc < 1 || kc - 1 < c1 || kc - 1 > c2) {
            cs_msg("bad key column"); return;
        }
    }
    for (;;) {
        ch = cs_prompt_char("Order (A/D)? ");
        redraw();
        if (ch == 27) return;
        if (ch == 'A' || ch == 'a') { desc = 0; break; }
        if (ch == 'D' || ch == 'd') { desc = 1; break; }
    }
    cs_sort_range(c1, r1, c2, r2, kc - 1, desc);
    redraw();
}

static void storage_cmd(void)
{
    cs_msg("Room for %d entries", CS_BUDGET - entriesUsed);
}

/* ============================= Auto ================================ */
/* the next form-flag cell at or after (*col,*row), row-major */
static Cell *cs_next_form(int *col, int *row)
{
    int c = *col, r = *row;
    for (; r < CS_MAXROW; r++) {
        for (; c < CS_MAXCOL; c++) {
            Cell *cl = cs_getcell(c, r);
            if (cl && cl->formf) { *col = c; *row = r; return cl; }
        }
        c = 0;
    }
    return NULL;
}

static void auto_cmd(void)
{
    /* the form-flag cells are found LIVE: the course does not create
     * or delete form flags (commits and SET writes keep the
     * attribute), so no sequence buffer is needed at all - the
     * static int seq[CS_BUDGET] (16 KB of BSS) is gone */
    int sc = 0, sr = 0;
    Cell *stop;
    char buf[CS_EDITMAX + 1];   /* was buf[2]! second char typed went out
                                 * of bounds and smashed loop state */
    int k;

    if (!cs_next_form(&sc, &sr)) { cs_msg("no form flags"); return; }
    g_edit[0] = '\0';
    g_editlen = 0;
    g_entry_num = 1;
    g_editstate = EDIT_EDITING;   /* show "edit: <text>" during Auto */
    for (;;) {
        int cr = sr, cc = sc;
        curCol = cc; curRow = cr;
        g_entryrow = cr;          /* "current:" must show the stop cell */
        g_entrycol = cc;
        redraw();
        cs_msg("Form Control Mode");   /* bottom-left, manual 11.3 */
        /* entry loop: existing cell values are NOT cleared - a bare
         * RETURN keeps the stored value and jumps to the next stop */
        buf[0] = '\0';
        k = 0;
        for (;;) {
            int ch = getch();
            if (cs_was_resize(ch)) continue;
            if (ch == 27) {
                g_editstate = EDIT_IDLE;
                g_edit[0] = '\0';
                g_editlen = 0;
                redraw();
                cs_msg("Auto aborted");   /* paint after the redraw */
                return;
            }
            if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) break;
            if (ch == ';') {
                /* ';' is the command lead-in, never cell data
                 * (normal entry: main.c case ';' opens the menu) */
                cs_command_menu();
                continue;
            }
            if (ch == KEY_BACKSPACE || ch == 8 || ch == 127) {
                if (k > 0) buf[--k] = '\0';
            } else if (ch == 94) {
                /* '^' TEXT/NUMERIC toggle (manual 6.4), as in normal entry */
                g_entry_num = !g_entry_num;
            } else if (ch >= 32 && ch < 127 && k < CS_EDITMAX - 1) {
                /* '"' is a literal character, not a toggle */
                if (k == 0 && g_entry_num != ((ch >= '0' && ch <= '9') ||
                    ch == '+' || ch == '-' || ch == '(' || ch == '.'))
                    g_entry_num = ((ch >= '0' && ch <= '9') || ch == '+' ||
                                   ch == '-' || ch == '(' || ch == '.');
                buf[k++] = (char) ch;
                buf[k] = '\0';
            }
            g_editlen = k;
            strncpy(g_edit, buf, CS_EDITMAX);
            g_edit[CS_EDITMAX - 1] = '\0';
            redraw();
        }
        if (k > 0) {
            int ec;
            cs_recalc_limit_reset();  /* fresh overrun state per commit */
            ec = cs_commit_entry(cc, cr, buf);
            /* numeric eval error -> message (space is data inside Auto:
             * no "hit space" confirm here) */
            {
                Cell *fc3 = cs_getcell(cc, cr);
                if (fc3 && fc3->iserr)
                    cs_msg("Error %d-> %s", ec <= 0 ? 0 : ec, buf);
            }
            if (cs_recalc_limit_hit())
                cs_msg("Recalculation limit reached");
        }
        g_edit[0] = '\0';
        g_editlen = 0;
        /* the next form-flag stop at or after this one */
        sc = cc + 1;
        sr = cr;
        stop = cs_next_form(&sc, &sr);
        if (!stop) break;
    }
    g_editstate = EDIT_IDLE;
    cs_recalc_all();
    redraw();
    cs_msg("Auto complete");   /* paint after the final redraw */
    if (cs_recalc_limit_hit())
        cs_msg("Recalculation limit reached");
}

/* ========================== Quit / menu ============================ */
static void quit_cmd(void)
{
    int ch = cs_prompt_yn("verify Y/N - ");
    if (ch == 1) {
        endwin();
        exit(0);
    }
    redraw();
}

static void redraw(void) { cs_draw(); }

void cs_command_menu(void)
{
    int ch = cs_prompt_char("Command > ");
    redraw();
    if (ch == 27 || ch == '\n' || ch == '\r') return;
    switch (toupper(ch)) {
    case 'A': auto_cmd(); break;
    case 'C': copy_cmd(); break;
    case 'D': delete_cmd(); break;
    case 'E': winCol = curCol; winRow = curRow; redraw(); break;
    case 'F': format_cmd(); break;
    case 'G': cs_goto_cmd(); break;
    /* the original: RETURN on the second help page "switches back to
     * the applications main screen" - repaint it at once (the key
     * loop would otherwise leave page 2 up until the next key) */
    case 'H': cs_help_cmd(); redraw(); break;
    case '#': sort_cmd(); break;   /* sort (user-ext 2026-11, not in
                                   * the panel or the original help) */
    case 'I': insert_cmd(); break;
    case 'L': cs_load_dialog(0); break;
    case 'M': cs_load_dialog(1); break;
    case 'O': dirTB = !dirTB; redraw(); break;
    case 'P': cs_print_cmd(); break;
    case 'Q': quit_cmd(); break;
    case 'R': recalc_cmd(); break;
    case 'S': cs_save_dialog(); break;
    case 'W': what_cmd(); break;
    case '?': storage_cmd(); break;
    case '=': cs_lock_cmd(); break;
    case '*':
        extended = !extended;
        cs_stay_visible();   /* keep the cursor inside the new row count */
        redraw();
        break;
    default: break;
    }
}
