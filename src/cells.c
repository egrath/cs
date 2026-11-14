/* cells.c - cell storage, coordinates, recalculation */
#include <stdlib.h>
#include <string.h>
#include "cs.h"

int  colw[CS_MAXCOL];
int  colp[CS_MAXCOL];
int  curCol = 0, curRow = 0;
int  winCol = 0, winRow = 0;
int  dirTB = 0;
int  extended = 0;
int  lockCols = 0, lockRows = 0;
int  showForm = 0;
char fname[40] = "";
char cs_cmdfile[40] = "";
int  entriesUsed = 0;
int  orderTB = 0;
int  cs_headless = 0;

/* regression state, updated whenever a REGR() is evaluated */
int    g_reg_ok = 0;
Csn    g_reg_slope, g_reg_icept, g_reg_meany;

void cs_reg_reset(void)
{
    g_reg_ok = 0;
    csn_zero(&g_reg_slope);
    csn_zero(&g_reg_icept);
    csn_zero(&g_reg_meany);
}

char g_edit[CS_EDITMAX];
int  g_editlen = 0;
int  g_editstate = EDIT_IDLE;
int  g_entry_num = 1;
int  g_entryrow = 0, g_entrycol = 0;

/* ---- the sparse cell store -------------------------------------
 * A chunked pool of Cell slots plus a per-row sorted index.  The pool
 * allocates 250-slot chunks on demand (each chunk well under 64 KiB,
 * the 16-bit-portability rule; 16 chunks cover the 4000 budget).  A
 * free slot is threaded onto cs_freelist through its cont field.  The
 * per-row index holds {col, ref} entries sorted by column; total
 * index memory is bounded by the budget, not by the coordinate space.
 * Refs are 1-based (0 = none): ref-1 = chunk * CS_CHUNK + slot. */
#define CS_CHUNK   250
#define CS_NCHUNK  (CS_BUDGET / CS_CHUNK)

typedef struct {
    unsigned char col;      /* 0-based, < CS_MAXCOL               */
    unsigned short ref;     /* 1-based slot reference, 0 = none   */
} CsIdxEnt;

typedef struct {
    unsigned char n, cap;   /* entries; cap 0 = the array is NULL */
    CsIdxEnt *e;
} CsRow;

static Cell  *cs_chunks[CS_NCHUNK];
static int    cs_chunks_used = 0;
static Cell  *cs_freelist = NULL;
static CsRow  cs_rows[CS_MAXROW];

static Cell *cs_ref_ptr(unsigned short ref)
{
    unsigned short r = (unsigned short) (ref - 1);
    return &cs_chunks[r / CS_CHUNK][r % CS_CHUNK];
}

/* the ref of a pool slot, found by scanning the chunk table (used
 * only at take time) */
static unsigned short cs_ptr_ref(const Cell *c)
{
    int k;
    for (k = 0; k < cs_chunks_used; k++)
        if (c >= cs_chunks[k] && c < cs_chunks[k] + CS_CHUNK)
            return (unsigned short) (1 + (c - cs_chunks[k]) +
                     k * CS_CHUNK);
    return 0;
}

/* take a slot from the free list (or a fresh chunk); the caller only
 * receives it after the index insert succeeds, otherwise the slot
 * returns to the list */
static Cell *cs_pool_take(int col, int rw, unsigned short *refout)
{
    Cell *c;
    int i;
    if (entriesUsed >= CS_BUDGET) return NULL;
    if (!cs_freelist) {
        Cell *ch;
        if (cs_chunks_used >= CS_NCHUNK) return NULL;
        ch = (Cell *) malloc(sizeof(Cell) * (size_t) CS_CHUNK);
        if (!ch) return NULL;
        cs_chunks[cs_chunks_used++] = ch;

        for (i = CS_CHUNK - 1; i >= 0; i--) {
            ch[i].cont = (char *) cs_freelist;
            cs_freelist = &ch[i];
        }
    }
    c = cs_freelist;
    cs_freelist = (Cell *) c->cont;
    /* the dense matrix's never-written state */
    memset(c, 0, sizeof(Cell));
    c->type = CE_EMPTY;
    c->just = 'L';
    c->prec = -1;
    c->col = (unsigned char) col;
    c->row = (unsigned char) rw;
    if (refout)
        *refout = cs_ptr_ref(c);
    return c;
}


/* ---- the public sparse API ------------------------------------- */

static int cs_row_find(const CsRow *rp, int col)
{
    int lo = 0, hi = rp->n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (rp->e[mid].col < col) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

Cell *cs_getcell(int col, int rw)
{
    CsRow *rp;
    int i;
    if (col < 0 || col >= CS_MAXCOL || rw < 0 || rw >= CS_MAXROW)
        return NULL;
    rp = &cs_rows[rw];
    if (rp->n == 0) return NULL;
    i = cs_row_find(rp, col);
    if (i >= rp->n || rp->e[i].col != col) return NULL;
    return cs_ref_ptr(rp->e[i].ref);
}

static Cell *cs_row_insert(int col, int rw, Cell *c, unsigned short ref)
{
    CsRow *rp = &cs_rows[rw];
    int i;
    if (rp->n == rp->cap) {
        int nc = rp->cap ? rp->cap * 2 : 4;
        CsIdxEnt *ne = (CsIdxEnt *) realloc(rp->e,
                        (size_t) nc * sizeof(CsIdxEnt));
        if (!ne) return NULL;
        rp->e = ne;
        rp->cap = (unsigned char) nc;
    }
    i = cs_row_find(rp, col);
    memmove(&rp->e[i + 1], &rp->e[i],
            (size_t) (rp->n - i) * sizeof(CsIdxEnt));
    rp->e[i].col = (unsigned char) col;
    rp->e[i].ref = ref;
    rp->n++;
    return c;
}

Cell *cs_alloc_cell(int col, int rw)
{
    CsRow *rp;
    Cell *c;
    unsigned short ref = 0;
    int i;
    if (col < 0 || col >= CS_MAXCOL || rw < 0 || rw >= CS_MAXROW)
        return NULL;
    rp = &cs_rows[rw];
    if (rp->n) {
        /* reuse an existing slot even if it indexes an empty cell */
        i = cs_row_find(rp, col);
        if (i < rp->n && rp->e[i].col == col)
            return cs_ref_ptr(rp->e[i].ref);
    }
    c = cs_pool_take(col, rw, &ref);
    if (!c) return NULL;
    if (!cs_row_insert(col, rw, c, ref)) {
        /* the index could not grow: the slot goes back unused */
        c->cont = (char *) cs_freelist;
        cs_freelist = c;
        return NULL;
    }
    return c;
}

void cs_free_cell_at(int col, int rw)
{
    CsRow *rp;
    Cell *c;
    unsigned short ref = 0;
    int i;
    if (col < 0 || col >= CS_MAXCOL || rw < 0 || rw >= CS_MAXROW)
        return;
    rp = &cs_rows[rw];
    if (rp->n == 0) return;
    i = cs_row_find(rp, col);
    if (i >= rp->n || rp->e[i].col != col) return;
    ref = rp->e[i].ref;
    memmove(&rp->e[i], &rp->e[i + 1],
            (size_t) (rp->n - i - 1) * sizeof(CsIdxEnt));
    rp->n--;
    c = cs_ref_ptr(ref);
    cs_free_cell(c);            /* the owned resources + the resets */
    c->cont = (char *) cs_freelist;
    cs_freelist = c;
}

/* relink a slot from (fc,fr) to (tc,tr) without moving it in the pool
 * (row/column insert and delete shifts).  An occupied target is
 * released first - the dense struct-assignment overwrote it and
 * leaked its strings.  An absent source does nothing. */
void cs_relink_cell(int fc, int fr, int tc, int tr)
{
    CsRow *sp = &cs_rows[fr], *dp = &cs_rows[tr];
    Cell *c;
    unsigned short ref;
    int i;
    if (fc < 0 || fc >= CS_MAXCOL || fr < 0 || fr >= CS_MAXROW ||
        tc < 0 || tc >= CS_MAXCOL || tr < 0 || tr >= CS_MAXROW)
        return;
    i = cs_row_find(sp, fc);
    if (i >= sp->n || sp->e[i].col != fc) return;
    ref = sp->e[i].ref;
    memmove(&sp->e[i], &sp->e[i + 1],
            (size_t) (sp->n - i - 1) * sizeof(CsIdxEnt));
    sp->n--;
    if (dp->n) {                    /* an occupied target: release it */
        int j = cs_row_find(dp, tc);
        if (j < dp->n && dp->e[j].col == tc)
            cs_free_cell_at(tc, tr);
    }
    c = cs_ref_ptr(ref);
    c->col = (unsigned char) tc;
    c->row = (unsigned char) tr;
    if (dp->n == dp->cap) {
        int nc = dp->cap ? dp->cap * 2 : 4;
        CsIdxEnt *ne = (CsIdxEnt *) realloc(dp->e,
                        (size_t) nc * sizeof(CsIdxEnt));
        if (!ne) {                  /* the slot is lost from the index:
                                     * keep it consistent (freelist) */
            c->cont = (char *) cs_freelist;
            cs_freelist = c;
            if (c->type != CE_EMPTY) entriesUsed--;
            c->type = CE_EMPTY;
            return;
        }
        dp->e = ne;
        dp->cap = (unsigned char) nc;
    }
    i = cs_row_find(dp, tc);
    memmove(&dp->e[i + 1], &dp->e[i],
            (size_t) (dp->n - i) * sizeof(CsIdxEnt));
    dp->e[i].col = (unsigned char) tc;
    dp->e[i].ref = ref;
    dp->n++;
}

/* the ordered scan: the first populated cell at or after (*col,*rw).
 * The caller advances with *col = cl->col + 1, *rw = cl->row. */
Cell *cs_next_cell(int *col, int *rw)
{
    int c = *col, r = *rw;
    for (; r < CS_MAXROW; r++) {
        CsRow *rp = &cs_rows[r];
        int i;
        if (rp->n == 0) continue;
        i = (r == *rw) ? cs_row_find(rp, c) : 0;
        if (i < rp->n) {
            *col = rp->e[i].col;
            *rw = r;
            return cs_ref_ptr(rp->e[i].ref);
        }
    }
    return NULL;
}

char *cs_strdup(const char *s)
{
    char *p = (char *) malloc(strlen(s) + 1);
    if (p) strcpy(p, s);
    return p;
}

void cs_col_name(int col, char *buf)
{
    if (col < 26) {
        buf[0] = (char) ('A' + col);
        buf[1] = '\0';
    } else {
        col -= 26;
        buf[0] = (char) ('A' + col / 26);
        buf[1] = (char) ('A' + col % 26);
        buf[2] = '\0';
    }
}

int cs_name_to_col(const char *s)
{
    int c = -1;
    if (s[0] >= 'A' && s[0] <= 'Z') {
        c = s[0] - 'A';
        if (s[1] >= 'A' && s[1] <= 'Z')
            c = 26 + c * 26 + (s[1] - 'A');
    }
    return c;
}

/* parse "B7" -> col 1 row 6.  returns 1 ok, 0 bad */
/* the column letter 'A'..'Z' / 'a'..'z' as 0..25, or -1 */
static int col_letter(char ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a';
    return -1;
}

int cs_parse_coord(const char *s, int *col, int *row)
{
    int c = 0, r = 0, n = 0, v;
    if (!s || !*s) return 0;
    /* letters accept either case - coordinate fields (Goto, TAB, copy
     * ranges, save/load corners) fold like the formula parser and the
     * reference scanners already do (user request 2026-11) */
    v = col_letter(s[0]);
    if (v < 0) return 0;
    c = v;
    s++;
    v = col_letter(s[0]);
    if (v >= 0) {
        c = 26 + c * 26 + v;
        s++;
    }
    if (col_letter(s[0]) >= 0) return 0;   /* three or more letters */
    while (*s >= '0' && *s <= '9') { r = r * 10 + (*s - '0'); s++; n++; }
    if (!n || *s) return 0;
    if (r < 1 || r > CS_MAXROW || c < 0 || c >= CS_MAXCOL) return 0;
    *col = c; *row = r - 1;
    return 1;
}

int cs_parse_range(const char *s, int *c1, int *r1, int *c2, int *r2)
{
    char buf[32];
    const char *p;
    int i;
    p = s;
    i = 0;
    while (*p && *p != '>' && i < 15) buf[i++] = *p++;
    buf[i] = '\0';
    if (*p != '>') return 0;
    p++;
    if (!cs_parse_coord(buf, c1, r1)) return 0;
    if (!cs_parse_coord(p, c2, r2)) return 0;
    cs_range_norm(c1, r1, c2, r2);
    return 1;
}

/* resolve dialog coordinate text (user-ext 2026-11, branch ref-val):
 * the static parse first (plain coord, coord>coord), then the
 * expression evaluator - a Reference or Range value, so REF()/VAL()
 * work in the command prompts (delete/copy From-To, Goto, corners).
 * 1 = ok; on failure the caller keeps its own message. */
int cs_coord_text(const char *s, int *c1, int *r1, int *c2, int *r2)
{
    if (cs_parse_coord(s, c1, r1)) { *c2 = *c1; *r2 = *r1; return 1; }
    if (strchr(s, '>') && cs_parse_range(s, c1, r1, c2, r2)) return 1;
    if (cs_eval_range(s, c1, r1, c2, r2) == 0) return 1;
    return 0;
}

/* sort reference remap (user-ext 2026-11): coordinate tokens inside
 * the sorted block are rewritten to the row where their cell landed.
 * perm[i] = the destination row (0-based) of the block's row r1+i.
 * Same token conventions as the insert/delete shifter: comments after
 * '\' and "..." strings are skipped, '!'-frozen references are not
 * remapped.  Returns a malloc'd replacement or NULL when unchanged. */
char *cs_sort_refs(const char *s, int c1, int r1, int c2, int r2,
                   const int *perm)
{
    char *out = (char *) malloc(strlen(s) * 2 + 16);
    char *o = out;
    const char *p = s;
    int changed = 0, inq = 0;
    if (!out) return NULL;
    while (*p) {
        if (*p == '"') { inq = !inq; *o++ = *p++; continue; }
        if (*p == '\\' && !inq) {
            /* '\' comment: the rest is copied verbatim */
            size_t rest = strlen(p);
            memcpy(o, p, rest);
            o += rest;
            break;
        }
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
            const char *st = p;
            int nl = 0;
            while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) { p++; nl++; }
            if (nl <= 2 && *p >= '0' && *p <= '9') {
                while (*p >= '0' && *p <= '9') p++;
                if (*p == '!' || *p == '>' || *p == ',' || *p == ')' ||
                    *p == ' ' || *p == ':' || *p == '+' || *p == '-' ||
                    *p == '*' || *p == '/' || *p == '%' || *p == '\0' ||
                    *p == '<' || *p == '=' || *p == '&') {
                    char tmp[8], tmp2[4];
                    int i, cc, rr, ok, bang;
                    for (i = 0; st + i < p; i++) tmp[i] = (char) ((st[i] >= 'a' && st[i] <= 'z') ? st[i] - 32 : st[i]);
                    tmp[i] = '\0';
                    ok = cs_parse_coord(tmp, &cc, &rr);
                    bang = *p == '!';
                    if (ok && !bang &&
                        cc >= c1 && cc <= c2 && rr >= r1 && rr <= r2) {
                        int newr = perm[rr - r1];
                        cs_col_name(cc, tmp2);
                        o += sprintf(o, "%s%d", tmp2, newr + 1);
                        changed = 1;
                        if (bang) *o++ = '!';
                        if (*p == '!') p++;
                        continue;
                    }
                    memcpy(o, st, (size_t) (p - st));
                    o += p - st;
                    if (bang) *o++ = '!';
                    if (*p == '!') p++;
                    continue;
                }
            }
            /* not a coordinate: a word (fn name etc.) - copy verbatim */
            memcpy(o, st, (size_t) (p - st));
            o += p - st;
        }
        *o++ = *p++;
    }
    *o = '\0';
    if (!changed) { free(out); return NULL; }
    return out;
}

void cs_range_norm(int *c1, int *r1, int *c2, int *r2)
{
    int t;
    if (*c1 > *c2) { t = *c1; *c1 = *c2; *c2 = t; }
    if (*r1 > *r2) { t = *r1; *r1 = *r2; *r2 = t; }
}

void cs_set_text(Cell *c, const char *cont, char just)
{
    cs_free_cell(c);
    c->type = CE_TEXT;
    c->just = just;
    if (just == 'W') c->rep = cs_strdup(cont);
    c->cont = cs_strdup(cont);
    csn_zero(&c->val);
    c->iserr = 0;
    c->strres = NULL;
    entriesUsed++;
}

void cs_set_num(Cell *c, const char *cont, const Csn *val, int iserr, const char *strres)
{
    cs_free_cell(c);
    c->type = CE_NUM;
    c->just = 'R';
    c->cont = cs_strdup(cont);
    csn_copy(&c->val, val);
    c->iserr = iserr;
    if (strres) c->strres = cs_strdup(strres);
    entriesUsed++;
}

void cs_free_cell(Cell *c)
{
    if (!c) return;
    if (c->type != CE_EMPTY) entriesUsed--;
    if (c->cont) { free(c->cont); c->cont = NULL; }
    if (c->rep) { free(c->rep); c->rep = NULL; }
    if (c->strres) { free(c->strres); c->strres = NULL; }
    c->type = CE_EMPTY;
    c->just = 'L';
    csn_zero(&c->val);
    c->iserr = 0;
    c->factf = 0;
    /* NOTE: formf deliberately survives - the user's form course must
     * keep its stop-cells across data re-entry (manual: ";A ... is very
     * useful when certain entries are subject to frequent changes").
     * prec and notation (per-cell format attributes) survive the same
     * way.  cs_clear_all() resets the flags explicitly. */
    c->prec = -1;
}

void cs_clear_all(void)
{
    int r, c;
    for (r = 0; r < CS_MAXROW; r++) {
        if (cs_rows[r].e) { free(cs_rows[r].e); cs_rows[r].e = NULL; }
        cs_rows[r].n = 0;
        cs_rows[r].cap = 0;
    }
    for (r = 0; r < cs_chunks_used; r++) {
        free(cs_chunks[r]);
        cs_chunks[r] = NULL;
    }
    cs_chunks_used = 0;
    cs_freelist = NULL;
    entriesUsed = 0;
    for (c = 0; c < CS_MAXCOL; c++) { colw[c] = CS_DEF_W; colp[c] = CS_DEF_P; }
}

int cs_ensure_room(void)
{
    if (entriesUsed >= CS_BUDGET) return 0;
    return 1;
}

/* the FILE NAME as the 10-character status box shows it (user report
 * 2026-11-14: a long path overran the box and cut the UI): the
 * basename after the last '/', the last extension stripped, and the
 * rest truncated to 10 characters.  The stored fname keeps the full
 * name so ;S re-saves into the same place. */
void cs_fname_disp(const char *name, char *out, int outn)
{
    const char *b, *dot;
    int n;
    out[0] = '\0';
    if (!name || !name[0]) return;
    b = strrchr(name, '/');
    b = b ? b + 1 : name;
    dot = strrchr(b, '.');
    n = dot ? (int) (dot - b) : (int) strlen(b);
    if (n > 10) n = 10;
    if (n >= outn) n = outn - 1;
    if (n > 0) memcpy(out, b, (size_t) n);
    out[n] = '\0';
}

/* re-count real entries (copy writes directly into cells and can drift
 * from the count; the auto/memory warning and ";? Room for" rely on it) */
void cs_recount_entries(void)
{
    int r, i, n = 0;
    for (r = 0; r < CS_MAXROW; r++) {
        const CsRow *rp = &cs_rows[r];
        for (i = 0; i < rp->n; i++)
            if (cs_ref_ptr(rp->e[i].ref)->type != CE_EMPTY) n++;
    }
    entriesUsed = n;
}

/* ---------------------------------------------------------------+
 | reference scanner: callback for every non-locked coordinate     |
 | reference (cell or range end) found in a formula string         |
 +--------------------------------------------------------------- */
int cs_scan_refs(const char *s, void (*cb)(int, int, void *), void *ctx)
{
    int col, row, inq = 0;
    const char *p = s;
    while (*p) {
        if (*p == '"') { inq = !inq; p++; continue; }
        if (*p == '\\' && !inq) return 0;   /* '\' comment: no refs follow */
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
            const char *st = p;
            int nl = 0;
            while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) { p++; nl++; }
            if (nl <= 2 && (*p >= '0' && *p <= '9')) {
                /* could be coordinate or unknown function name */
                while (*p >= '0' && *p <= '9') p++;
                if (*p == '!' || *p == '>' || *p == ',' || *p == ')' ||
                    *p == ' ' || *p == ':' || *p == '+' || *p == '-' ||
                    *p == '*' || *p == '/' || *p == '%' || *p == '\0' ||
                    *p == '<' || *p == '=' || *p == '&') {
                    char tmp[8];
                    int i, ok;
                    for (i = 0; st + i < p; i++) tmp[i] = (char) ((st[i] >= 'a' && st[i] <= 'z') ? st[i] - 32 : st[i]);
                    tmp[i] = '\0';
                    ok = cs_parse_coord(tmp, &col, &row);
                    if (ok && cb) cb(col, row, ctx);
                    if (*p == '!') p++;
                    continue;
                }
            }
            /* not a coordinate: skip the whole word (function name) */
            p = st + 1;
            continue;
        }
        p++;
    }
    return 0;
}

/* shift every (non-bang) reference by (dc,dr); caller owns result */
char *cs_adjust_refs(const char *s, int dc, int dr)
{
    char *out = (char *) malloc(strlen(s) * 2 + 16);
    char *o = out;
    const char *p = s;
    int inq = 0;
    if (!out) return NULL;
    while (*p) {
        if (*p == '"') { inq = !inq; *o++ = *p++; continue; }
        if (*p == '\\' && !inq) {
            /* '\' comment: the rest is copied verbatim */
            size_t rest = strlen(p);
            memcpy(o, p, rest);
            o += rest;
            break;
        }
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
            const char *st = p;
            int nl = 0;
            while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) { p++; nl++; }
            if (nl <= 2 && *p >= '0' && *p <= '9') {
                while (*p >= '0' && *p <= '9') p++;
                if (*p == '!' || *p == '>' || *p == ',' || *p == ')' ||
                    *p == ' ' || *p == ':' || *p == '+' || *p == '-' ||
                    *p == '*' || *p == '/' || *p == '%' || *p == '\0' ||
                    *p == '<' || *p == '=' || *p == '&') {
                    char tmp[8], tmp2[8];
                    int i, newc, newr, ok;
                    for (i = 0; st + i < p; i++) tmp[i] = (char) ((st[i] >= 'a' && st[i] <= 'z') ? st[i] - 32 : st[i]);
                    tmp[i] = '\0';
                    ok = cs_parse_coord(tmp, &newc, &newr);
                    if (ok && *p != '!') {
                        newc += dc; newr += dr;
                    }
                    if (*p == '!') p++;
                    if (ok) {
                        cs_col_name(newc, tmp2);
                        o += sprintf(o, "%s%d", tmp2, newr + 1);
                        if (*(p - 1) == '!') *o++ = '!';
                    } else {
                        memcpy(o, st, (size_t) (p - st));
                        o += p - st;
                    }
                    continue;
                }
            }
            /* not a coordinate: a word (function name, text) - copy the
             * run verbatim.  The letter run is consumed from st to p. */
            memcpy(o, st, (size_t) (p - st));
            o += p - st;
        }
        *o++ = *p++;
    }
    *o = '\0';
    return out;
}

/* shift every (non-bang) reference row/col across an insert/delete line.
   iscol: 0 -> rows, 1 -> cols.  insert: 1 for insert (+1 at & beyond),
   0 for delete (-1 strictly beyond).  Returns fresh string or NULL if
   nothing was changed. */
/* shift every coordinate reference in s by `delta` (iscol: columns vs
 * rows).  With ge != 0 references AT `at` shift too (insertion mode,
 * delta +1), otherwise only those beyond it (deletion mode). */
static char *shift_refs_core(const char *s, int iscol, int at, int delta, int ge)
{
    char *out = (char *) malloc(strlen(s) * 2 + 16);
    char *o = out;
    const char *p = s;
    int changed = 0, inq = 0;
    if (!out) return NULL;
    while (*p) {
        if (*p == '"') { inq = !inq; *o++ = *p++; continue; }
        if (*p == '\\' && !inq) {
            /* '\' comment: the rest is copied verbatim */
            size_t rest = strlen(p);
            memcpy(o, p, rest);
            o += rest;
            break;
        }
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
            const char *st = p;
            int nl = 0;
            while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) { p++; nl++; }
            if (nl <= 2 && *p >= '0' && *p <= '9') {
                while (*p >= '0' && *p <= '9') p++;
                if (*p == '!' || *p == '>' || *p == ',' || *p == ')' ||
                    *p == ' ' || *p == ':' || *p == '+' || *p == '-' ||
                    *p == '*' || *p == '/' || *p == '%' || *p == '\0' ||
                    *p == '<' || *p == '=' || *p == '&') {
                    char tmp[8], tmp2[4];
                    int i, newc, newr, ok, bang;
                    for (i = 0; st + i < p; i++) tmp[i] = (char) ((st[i] >= 'a' && st[i] <= 'z') ? st[i] - 32 : st[i]);
                    tmp[i] = '\0';
                    ok = cs_parse_coord(tmp, &newc, &newr);
                    bang = *p == '!';
                    if (ok && !bang) {
                        int did = 0;
                        if (iscol) {
                            if (ge ? newc >= at : newc > at) { newc += delta; did = 1; }
                        } else {
                            if (ge ? newr >= at : newr > at) { newr += delta; did = 1; }
                        }
                        if (did && newc >= 0 && newc < CS_MAXCOL &&
                            newr >= 0 && newr < CS_MAXROW) {
                            cs_col_name(newc, tmp2);
                            o += sprintf(o, "%s%d", tmp2, newr + 1);
                            changed = 1;
                            if (bang) *o++ = '!';
                            if (*p == '!') p++;
                            continue;
                        }
                    }
                    memcpy(o, st, (size_t) (p - st));
                    o += p - st;
                    if (bang) *o++ = '!';
                    if (*p == '!') p++;
                    continue;
                }
            }
            /* not a coordinate: a word (fn name etc.) - copy verbatim */
            memcpy(o, st, (size_t) (p - st));
            o += p - st;
        }
        *o++ = *p++;
    }
    *o = '\0';
    if (!changed) { free(out); return NULL; }
    return out;
}

char *cs_shift_refs(const char *s, int iscol, int at, int insert)
{
    return shift_refs_core(s, iscol, at, insert ? 1 : -1, insert);
}

/* range-delete variant: every reference BEYOND `at` shifts by `delta`
 * (negative for deletions) */
char *cs_shift_refs_n(const char *s, int iscol, int at, int delta)
{
    return shift_refs_core(s, iscol, at, delta, 0);
}

/* ------------------------------------------------------------+
 | numeric formatting: truncate (not round) to prec decimals;   |
 | fall back to exponential form if it does not fit the width   |
 | (all digit work handled by the BCD engine).                  |
 | notation (flagged ext 2026-11): 0 = auto (the manual's       |
 | behavior above), 1 = always scientific ("!n!" when even that |
 | does not fit), 2 = always decimal (too wide: keep the head,  |
 | a '*' as the last symbol marks the visual truncation)        |
 +----------------------------------------------------------- */
void cs_fmt_num(const Csn *v, int width, int prec, int iserr, int notation,
                char *buf, int bufn)
{
    if (iserr) { sprintf(buf, "?n?"); return; }
    if (notation == 1) {
        /* forced scientific: fit the exponential form to the width */
        int frac = width - 6;
        if (frac < 0) frac = 0;
        while (frac > 0) {
            char e[64];
            csn_fmt_sci(v, frac, e, (int) sizeof(e));
            if ((int) strlen(e) <= width) break;
            frac--;
        }
        csn_fmt_sci(v, frac, buf, bufn);
        if ((int) strlen(buf) <= width) return;
        if (width > 0 && width >= 3 && width < bufn) strcpy(buf, "!n!");
        else buf[0] = '\0';
        return;
    }
    if (notation == 2) {
        /* forced decimal: never fall back; a too-wide value keeps its
         * head and ends in '*' (the user sees the truncation) */
        int lead = v->exp10 - 1;
        int ipart = (v->ndig && lead >= 0) ? lead + 1 : 1;
        int need = ipart + (prec > 0 ? 1 + prec : 0) + 2;
        if (need > bufn - 1) {
            /* the fixed form cannot even be built (pathologically huge
             * value): the manual's "not wide enough" token */
            if (width >= 3 && width < bufn) strcpy(buf, "!n!");
            else buf[0] = '\0';
            return;
        }
        csn_fmt_fixed(v, prec, buf, bufn);
        if (buf[0] != '\0' && (int) strlen(buf) <= width) return;
        if (width >= 2 && width < bufn) {
            memcpy(buf + width - 1, "*", (size_t) 1);
            buf[width] = '\0';
        }
        return;
    }
    /* integer digits of the fixed form; if too wide for the buffer, go
     * straight to the exponential fallback */
    {
        int lead = v->exp10 - 1;
        int ipart = (v->ndig && lead >= 0) ? lead + 1 : 1;
        int need = ipart + (prec > 0 ? 1 + prec : 0) + 2;
        if (need > bufn - 1) {
            /* exponential form per the manual (e.g. 7.35E-6) */
            int frac = width - 6;
            if (frac < 0) frac = 0;
            while (frac > 0) {
                char e[64];
                csn_fmt_sci(v, frac, e, (int) sizeof(e));
                if ((int) strlen(e) <= width) break;
                frac--;
            }
            csn_fmt_sci(v, frac, buf, bufn);
            if ((int) strlen(buf) <= width) return;
        } else {
            csn_fmt_fixed(v, prec, buf, bufn);
            if (buf[0] != '\0' && (int) strlen(buf) <= width) return;
        }
    }
    /* exponential fallback (value does not fit even in "long" form) */
    {
        int frac = width - 6;
        if (frac < 0) frac = 0;
        while (frac > 0) {
            char e[64];
            csn_fmt_sci(v, frac, e, (int) sizeof(e));
            if ((int) strlen(e) <= width) break;
            frac--;
        }
        csn_fmt_sci(v, frac, buf, bufn);
        if ((int) strlen(buf) <= width) return;
    }
    /* does not fit in any form: the manual's "!n! -- column is not wide
     * enough" token */
    if (width > 0 && width >= 3 && width < bufn) {
        strcpy(buf, "!n!");
    } else buf[0] = '\0';
}
