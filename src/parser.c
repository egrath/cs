/* parser.c - CalcStar expression tokenizer, parser and evaluator.
 *
 * All arithmetic is BCD decimal through num.c - no floating point
 * anywhere, matching the original CalcStar engine. */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "cs.h"

#define MAXDEPTH 12
#define REGR_MAX 512
#define LISTMAX  2048

/* Val.good == 0 : error, .err holds the CalcStar evaluation error code
   (0 = value range error, 1 illegal coord, 2 bad range, 3 missing paren,
   4 unknown function, 6 illegal terminal expr, 7 trailing junk,
   8 bad number, 9 regression error, 12 paren depth, 255 regr value) */

/* first-class references and ranges (user-ext 2026-11): lightweight,
 * no dynamic memory - a bare cell identifier evaluates to a VT_REF
 * and VAL() retrieves its value (option-3 semantics: references
 * coerce to values where a value is consumed) */
typedef struct {
    int col;       /* 0-based, fits cs_cell(col,row) */
    int row;
} CsRef;

typedef struct {
    CsRef start;
    CsRef end;
} CsRng;

enum { VT_NUM = 0, VT_STR, VT_BOOL, VT_REF, VT_RANGE };

typedef struct {
    Csn num;
    char  *str;    /* malloc'd when type==VT_STR */
    int    type;   /* VT_* explicit discriminator (user-ext 2026-11) */
    CsRef  ref;    /* VT_REF */
    CsRng  rng;    /* VT_RANGE */
    int    err;
    int    good;
} Val;

typedef struct {
    const char *p;
    int depth;
    int dry;       /* parse-only: side-effect statements are detected
                    * but NOT executed (the factorial-entry syntax
                    * check must not run SETs) */
} P;

static void val_free(Val *v)
{
    if (v->str) { free(v->str); v->str = NULL; }
}

static void val_err(Val *v, int code)
{
    val_free(v);
    v->str = NULL;
    v->type = VT_NUM;
    v->err = code; v->good = 0;
    csn_zero(&v->num);
}

/* dereference a Reference where a value is consumed (option-3
 * semantics, user-ext 2026-11): numeric cell -> its number, TEXT cell
 * -> its content as a string (today's behavior), empty/allocated/
 * error cell -> Error 1 (cellnum's convention - never silently 0).
 * Returns -1 when *v holds a value afterwards, else the error code. */
static int val_deref(Val *v)
{
    Cell *cl;
    if (v->type == VT_NUM || v->type == VT_BOOL || v->type == VT_STR)
        return -1;
    if (v->type != VT_REF) return 6;    /* a Range is not a value here */
    cl = cs_cell(v->ref.col, v->ref.row);
    if (cl && cl->type == CE_TEXT && cl->cont && !cl->iserr) {
        v->str = cs_strdup(cl->cont);
        if (!v->str) { val_err(v, 0); return 0; }
        v->type = VT_STR;
        return -1;
    }
    v->type = VT_NUM;
    csn_zero(&v->num);
    if (cl && cl->type == CE_NUM && !cl->iserr) {
        csn_copy(&v->num, &cl->val);
        return -1;
    }
    return 1;                           /* empty/allocated/error cell */
}

static Csn gInd[REGR_MAX], gDep[REGR_MAX];

static int is_coord_ahead(const char *p, int *cl, int *nd)
{
    int l = 0, d = 0;
    const char *q = p;
    while ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z')) { l++; q++; }
    while (*q >= '0' && *q <= '9') { d++; q++; }
    *cl = l; *nd = d;
    return l >= 1 && l <= 2 && d >= 1;
}

static const char *parse_coord_tok(const char *p, int *col, int *row,
                                   int *exit_err, char *out)
{
    char word[16];
    int i = 0, l = 0, d = 0;
    const char *q = p;
    while ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z')) { q++; l++; }
    while (*q >= '0' && *q <= '9') { q++; d++; }
    if (l < 1 || l > 2 || d < 1 || l + d > 13) { *exit_err = 1; return NULL; }
    for (i = 0; i < l; i++) word[i] = (char) toupper((unsigned char) p[i]);
    for (i = 0; i < d; i++) word[l + i] = p[l + i];
    word[l + d] = '\0';
    if (!cs_parse_coord(word, col, row)) { *exit_err = 1; return NULL; }
    if (out) strcpy(out, word);
    return (*q == '!') ? q + 1 : q;
}

static const char *parse_num(const char *p, Val *v)
{
    const char *q = p;
    char buf[96];
    int n = 0, dots = 0, ee = 0;
    int pr;
    while (((*q >= '0' && *q <= '9') || *q == '.') && n < 44) {
        if (*q == '.') dots++;
        buf[n++] = *q++;
    }
    if ((*q == 'E' || *q == 'e') && (q[1] == '+' || q[1] == '-' ||
        (q[1] >= '0' && q[1] <= '9'))) {
        buf[n++] = 'E'; q++;
        if (*q == '+' || *q == '-') buf[n++] = *q++;
        while (*q >= '0' && *q <= '9' && ee < 10) { buf[n++] = *q++; ee++; }
    }
    buf[n] = '\0';
    if (n == 0 || dots > 1 || (n == 1 && buf[0] == '.')) { val_err(v, 8); return NULL; }
    pr = csn_parse(buf, &v->num);
    if (pr == CSN_ER_SYNTAX) { val_err(v, 8); return NULL; }         /* malformed */
    if (pr == CSN_ER_OVERFLOW) { val_err(v, 0); return NULL; }       /* overflow */
    val_free(v);
    v->type = VT_NUM;
    v->good = 1;
    return q;
}

static const char *parse_string(const char *p, Val *v)
{
    const char *q = p + 1;
    char out[256];
    int n = 0;
    while (*q && *q != '"') {
        if (n < 250) out[n++] = *q;
        q++;
    }
    if (*q != '"') { val_err(v, 3); return NULL; }
    q++;
    out[n] = '\0';
    val_free(v);
    v->str = cs_strdup(out);
    if (!v->str) { val_err(v, 0); return NULL; }
    v->good = 1; v->type = VT_STR;
    return q;
}

/* ---- function arguments --------------------------------------------- */
/* One argument is a general expression (the typed evaluator passes
 * Reference/Range/Number/String/Bool values through; user-ext 2026-11).
 * No separate range-parsing mechanism remains: `coord > coord` builds
 * a Range value in parse_cmp like any other operator. */

typedef enum {
    FN_SUM, FN_CNT, FN_AVG, FN_MAX, FN_MIN,
    FN_SQRT, FN_LOG, FN_LN, FN_ABS, FN_EXP,
    FN_REGR, FN_PROJ, FN_DEPD, FN_SLOPE, FN_UNKNOWN,
    FN_SIN, FN_COS, FN_TAN, FN_ASIN, FN_ACOS, FN_ATAN, FN_PI,
    FN_SIGN, FN_MOD, FN_POWER, FN_LOG10, FN_DEGREES, FN_RADIANS,
    FN_DEG, FN_RAD,
    FN_FACT, FN_COMBIN, FN_PERMUT, FN_GCD, FN_LCM, FN_ROUND, FN_TRUNC,
    FN_REF, FN_VAL, FN_COLIDX, FN_MED,
    FN_SGN, FN_NCR, FN_NPR, FN_INTG, FN_FRAC
} FnId;

static int fnid(const char *name)
{
    if (!strcmp(name, "SUM"))   return FN_SUM;
    if (!strcmp(name, "CNT"))   return FN_CNT;
    if (!strcmp(name, "AVG"))   return FN_AVG;
    if (!strcmp(name, "MAX"))   return FN_MAX;
    if (!strcmp(name, "MIN"))   return FN_MIN;
    if (!strcmp(name, "SQRT"))  return FN_SQRT;
    if (!strcmp(name, "LOG"))   return FN_LOG;
    if (!strcmp(name, "LN"))    return FN_LN;
    if (!strcmp(name, "ABS"))   return FN_ABS;
    if (!strcmp(name, "EXP"))   return FN_EXP;
    if (!strcmp(name, "REGR"))  return FN_REGR;
    if (!strcmp(name, "PROJ"))  return FN_PROJ;
    if (!strcmp(name, "DEPD"))  return FN_DEPD;
    if (!strcmp(name, "SLOPE")) return FN_SLOPE;
    if (!strcmp(name, "SIN"))   return FN_SIN;
    if (!strcmp(name, "COS"))   return FN_COS;
    if (!strcmp(name, "TAN"))   return FN_TAN;
    if (!strcmp(name, "ASIN"))  return FN_ASIN;
    if (!strcmp(name, "ACOS"))  return FN_ACOS;
    if (!strcmp(name, "ATAN"))  return FN_ATAN;
    if (!strcmp(name, "PI"))    return FN_PI;
    if (!strcmp(name, "SIGN"))  return FN_SIGN;
    if (!strcmp(name, "MOD"))   return FN_MOD;
    if (!strcmp(name, "POWER")) return FN_POWER;
    if (!strcmp(name, "DEGREES")) return FN_DEGREES;
    if (!strcmp(name, "RADIANS")) return FN_RADIANS;
    if (!strcmp(name, "DEG"))   return FN_DEG;    /* short alias, DEGREES */
    if (!strcmp(name, "RAD"))   return FN_RAD;    /* short alias, RADIANS */
    if (!strcmp(name, "FACT"))  return FN_FACT;
    if (!strcmp(name, "COMBIN")) return FN_COMBIN;
    if (!strcmp(name, "PERMUT")) return FN_PERMUT;
    if (!strcmp(name, "GCD"))   return FN_GCD;
    if (!strcmp(name, "LCM"))   return FN_LCM;
    if (!strcmp(name, "ROUND")) return FN_ROUND;
    if (!strcmp(name, "TRUNC")) return FN_TRUNC;
    if (!strcmp(name, "AVERAGE")) return FN_AVG;
    if (!strcmp(name, "REF"))   return FN_REF;
    if (!strcmp(name, "VAL"))   return FN_VAL;
    if (!strcmp(name, "COLIDX")) return FN_COLIDX;
    if (!strcmp(name, "MED"))   return FN_MED;
    if (!strcmp(name, "SGN"))   return FN_SGN;
    if (!strcmp(name, "NCR"))   return FN_NCR;
    if (!strcmp(name, "NPR"))   return FN_NPR;
    if (!strcmp(name, "INTG"))  return FN_INTG;
    if (!strcmp(name, "FRAC"))  return FN_FRAC;
    return FN_UNKNOWN;
}

/* the coordinate limits for REF(): 1..127 columns (A..DW), 1..255 rows */
static int ref_coord_ok(const Csn *t, int lo, int hi)
{
    Csn one, mx;
    csn_from_int(&one, lo);
    csn_from_int(&mx, hi);
    return (csn_cmp(t, &one) >= 0 && csn_cmp(t, &mx) <= 0);
}

/* a column designator word ("A", "AB", ..., "DW"): 1-2 letters only.
 * Returns the 1-based index (1..127) or 0 when it is not one. */
static int colidx_word(const char *w, int len)
{
    int i, idx;
    if (len < 1 || len > 2) return 0;
    for (i = 0; i < len; i++) {
        if (!isalpha((unsigned char) w[i])) return 0;
    }
    if (len == 1) {
        idx = toupper((unsigned char) w[0]) - 'A' + 1;
    } else {
        idx = (toupper((unsigned char) w[0]) - 'A') * 26 +
              (toupper((unsigned char) w[1]) - 'A') + 27;
    }
    if (idx > CS_MAXCOL) return 0;      /* beyond DW */
    return idx;
}

/* 1 when the value is an exact integer; the integer part in *out */
static int csn_integer_part(const Csn *v, Csn *out)
{
    Csn zero, t, d;
    csn_zero(&zero);
    if (csn_trunc(&t, v, &zero)) return 0;
    if (csn_sub(&d, v, &t)) return 0;
    if (!csn_is_zero(&d)) return 0;
    csn_copy(out, &t);
    return 1;
}

/* the integer as a plain long (coordinates are small) */
static long csn_to_long_val(const Csn *v)
{
    char fb[80];
    csn_fmt_fixed(v, 0, fb, (int) sizeof(fb));
    if (!fb[0]) return 0;
    return atol(fb);
}

static Val parse_condexpr(P *ps);   /* forward */

static void skip_ws(const char **pp)
{
    while (**pp == ' ') (*pp)++;
}

/* consume one argument and coerce it to a NUMBER (option-3 semantics:
 * a Reference derefs; a String or Range is a type error).  Returns a
 * good Number Val or the error Val. */
static Val parse_numarg(P *ps)
{
    Val v;
    int e;
    memset(&v, 0, sizeof(v));
    csn_zero(&v.num);
    v = parse_condexpr(ps);
    if (!v.good) return v;
    e = val_deref(&v);
    if (e >= 0) { val_free(&v); val_err(&v, e); return v; }
    if (v.type == VT_STR || v.type == VT_RANGE) {
        val_free(&v);
        val_err(&v, 6);
        return v;
    }
    return v;
}

/* collect an argument list for SUM/CNT/AVG/MAX/MIN: Numbers (also
 * dereferenced References) and Bool values append, Strings are
 * skipped, a Range walks its numeric cells (an error cell in the
 * range is Error 0). */
static Val parse_list(P *ps, Csn *list, int *cnt)
{
    int n = 0;
    Val rv;
    memset(&rv, 0, sizeof(rv));
    rv.type = VT_NUM;
    rv.good = 1;
    for (;;) {
        Val v;
        memset(&v, 0, sizeof(v));
        csn_zero(&v.num);
        v = parse_condexpr(ps);
        if (!v.good) { val_free(&v); return v; }
        if (v.type == VT_RANGE) {
            int c, r;
            for (r = v.rng.start.row; r <= v.rng.end.row; r++)
                for (c = v.rng.start.col; c <= v.rng.end.col; c++) {
                    Cell *cl = cs_cell(c, r);
                    if (!cl) continue;
                    if (cl->type == CE_NUM && !cl->iserr) {
                        if (n < LISTMAX) csn_copy(&list[n++], &cl->val);
                    } else if (cl->iserr) { val_err(&rv, 0); return rv; }
                }
        } else {
            int e = val_deref(&v);
            if (e >= 0) { val_free(&v); val_err(&rv, e); return rv; }
            if (v.type == VT_STR) continue;    /* text is ignored */
            if (n < LISTMAX) csn_copy(&list[n++], &v.num);
        }
        val_free(&v);
        skip_ws(&ps->p);
        if (*ps->p == ',') { ps->p++; continue; }
        break;
    }
    *cnt = n;
    return rv;
}

static Val parse_primary(P *ps)
{
    Val v;
    const char *p = ps->p;
    memset(&v, 0, sizeof(v));
    skip_ws(&p);
    if (*p == '(') {
        Val t;
        ps->p = p + 1;
        ps->depth++;
        if (ps->depth > MAXDEPTH) { Val e; memset(&e, 0, sizeof(e)); val_err(&e, 12); return e; }
        t = parse_condexpr(ps);
        ps->depth--;
        skip_ws(&ps->p);
        if (!t.good) return t;
        if (*ps->p != ')') { val_free(&t); val_err(&t, 3); return t; }
        ps->p++;
        return t;
    }
    if (*p == '"') {
        const char *q = parse_string(p, &v);
        if (!q) return v;
        ps->p = q;
        return v;
    }
    if ((*p >= '0' && *p <= '9') || (*p == '.' && p[1] >= '0' && p[1] <= '9')) {
        const char *q = parse_num(p, &v);
        if (!q) return v;
        ps->p = q;
        return v;
    }
    if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
        int l, d;
        if (is_coord_ahead(p, &l, &d)) {
            int a1, b1, ee = 0;
            const char *q = parse_coord_tok(p, &a1, &b1, &ee, NULL);
            if (!q) { val_err(&v, ee); return v; }
            /* a bare cell identifier is a REFERENCE value (user-ext
             * 2026-11); the value comes out through val_deref() where
             * the reference is consumed (option-3 semantics) */
            val_free(&v);
            memset(&v, 0, sizeof(v));
            v.type = VT_REF;
            v.ref.col = a1;
            v.ref.row = b1;
            v.good = 1;
            ps->p = q;
            return v;
        }
        /* word: function name */
        {
            char name[16];
            int n = 0, f;
            const char *q = p;
            while ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z')) {
                if (n < 14) name[n++] = (char) toupper((unsigned char) *q);
                q++;
            }
            name[n] = '\0';
            skip_ws(&q);
            if (*q != '(') { val_err(&v, 6); ps->p = q; return v; }
            f = fnid(name);
            if (f == FN_UNKNOWN) { val_err(&v, 4); ps->p = q + 1; return v; }
            ps->p = q + 1;
            skip_ws(&ps->p);
            if (f == FN_SLOPE) {
                if (*ps->p == ')') ps->p++;
                else { val_err(&v, 6); return v; }
                if (!g_reg_ok) { val_err(&v, 9); return v; }
                val_free(&v);
                csn_copy(&v.num, &g_reg_slope); v.good = 1;
                return v;
            }
            if (f == FN_PI) {
                if (*ps->p == ')') ps->p++;
                else { val_err(&v, 6); return v; }
                val_free(&v);
                csn_pi(&v.num); v.good = 1;
                return v;
            }
            if (f == FN_REF) {
                /* REF(col,row): both coordinates are one-based numeric
                 * expressions (option-3: a Reference argument derefs,
                 * so REF(A1,5) uses A1's value).  Rejects fractional,
                 * zero/negative and out-of-limits coordinates. */
                Val a, b;
                int cc, rr;
                Csn tc, tr;
                a = parse_numarg(ps);
                if (!a.good) return a;
                skip_ws(&ps->p);
                if (*ps->p != ',') { val_err(&v, 3); val_free(&a); return v; }
                ps->p++;
                b = parse_numarg(ps);
                if (!b.good) { val_free(&a); val_free(&b); return b; }
                skip_ws(&ps->p);
                if (*ps->p != ')') { val_err(&v, 3); val_free(&a); val_free(&b); return v; }
                ps->p++;
                if (!csn_integer_part(&a.num, &tc) ||
                    !csn_integer_part(&b.num, &tr)) {
                    val_free(&a); val_free(&b);
                    val_err(&v, 0);            /* fractional coordinate */
                    return v;
                }
                if (!ref_coord_ok(&tc, 1, CS_MAXCOL) ||
                    !ref_coord_ok(&tr, 1, CS_MAXROW)) {
                    val_free(&a); val_free(&b);
                    val_err(&v, 1);            /* out of limits */
                    return v;
                }
                cc = (int) csn_to_long_val(&tc);
                rr = (int) csn_to_long_val(&tr);
                val_free(&a); val_free(&b);
                val_free(&v);
                memset(&v, 0, sizeof(v));
                csn_zero(&v.num);
                v.type = VT_REF;
                v.ref.col = cc - 1;
                v.ref.row = rr - 1;
                v.good = 1;
                return v;
            }
            if (f == FN_VAL) {
                /* VAL(cell): the value of a single referenced cell */
                Val a = parse_condexpr(ps);
                Cell *cl;
                if (!a.good) return a;
                if (a.type != VT_REF) { val_free(&a); val_err(&v, 6); return v; }
                skip_ws(&ps->p);
                if (*ps->p != ')') { val_err(&v, 3); val_free(&a); return v; }
                ps->p++;
                cl = cs_cell(a.ref.col, a.ref.row);
                val_free(&a);
                val_free(&v);
                memset(&v, 0, sizeof(v));
                csn_zero(&v.num);
                v.type = VT_NUM;
                if (cl && cl->type == CE_NUM && !cl->iserr) {
                    csn_copy(&v.num, &cl->val);
                    v.good = 1;
                    return v;
                }
                val_err(&v, 1);     /* empty/allocated/text/error cell */
                return v;
            }
            if (f == FN_COLIDX) {
                /* COLIDX(x): the 1-based column index.  The argument is
                 * a column designator (a 1-2-letter word NOT followed
                 * by '(' - scoped to COLIDX, no global grammar change),
                 * a quoted designator string, or a Reference (its
                 * column; the row is ignored). */
                Val a;
                const char *p = ps->p;
                skip_ws(&p);
                if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
                    /* a word: the designator form when it is 1-2
                     * letters cleanly ended (not a function call, not
                     * a coordinate like A1 - that falls through to the
                     * expression path) */
                    const char *q = p;
                    while ((*q >= 'A' && *q <= 'Z') ||
                           (*q >= 'a' && *q <= 'z')) q++;
                    if (q - p <= 2 && (*q == ')' || *q == ' ')) {
                        int idx = colidx_word(p, (int) (q - p));
                        ps->p = q;
                        skip_ws(&ps->p);
                        if (*ps->p != ')') { val_err(&v, 3); return v; }
                        ps->p++;
                        val_free(&v);
                        memset(&v, 0, sizeof(v));
                        csn_zero(&v.num);
                        v.type = VT_NUM;
                        if (idx) {
                            csn_from_int(&v.num, idx);
                            v.good = 1;
                            return v;
                        }
                        val_err(&v, 1);    /* beyond DW */
                        return v;
                    }
                }
                /* the general forms: a quoted designator or a Reference */
                if (*p == ')') { val_err(&v, 3); return v; }   /* no arg */
                a = parse_condexpr(ps);
                if (!a.good) return a;
                skip_ws(&ps->p);
                if (*ps->p != ')') { val_err(&v, 3); val_free(&a); return v; }
                ps->p++;
                if (a.type == VT_STR) {
                    int idx = colidx_word(a.str, (int) strlen(a.str));
                    val_free(&a);
                    val_free(&v);
                    memset(&v, 0, sizeof(v));
                    csn_zero(&v.num);
                    v.type = VT_NUM;
                    if (idx) {
                        csn_from_int(&v.num, idx);
                        v.good = 1;
                        return v;
                    }
                    val_err(&v, 1);        /* not a column designator */
                    return v;
                }
                if (a.type == VT_REF) {
                    int idx = a.ref.col + 1;
                    val_free(&a);
                    val_free(&v);
                    memset(&v, 0, sizeof(v));
                    csn_zero(&v.num);
                    v.type = VT_NUM;
                    csn_from_int(&v.num, idx);
                    v.good = 1;
                    return v;
                }
                val_free(&a);
                val_err(&v, 6);            /* wrong type */
                return v;
            }
            if (f == FN_PROJ || f == FN_DEPD) {
                Val a = parse_numarg(ps);
                Val t;
                int err;
                if (!a.good) return a;
                skip_ws(&ps->p);
                if (*ps->p != ')') { val_err(&v, 3); val_free(&a); return v; }
                ps->p++;
                if (!g_reg_ok) { val_err(&v, 9); return v; }
                val_free(&v);
                if (f == FN_PROJ) {
                    err = csn_mul(&t.num, &g_reg_slope, &a.num);
                    if (err) { val_free(&a); val_err(&v, 255); return v; }
                    err = csn_add(&v.num, &g_reg_icept, &t.num);
                    if (err) { val_free(&a); val_err(&v, 255); return v; }
                } else {
                    if (csn_is_zero(&g_reg_slope)) { val_free(&a); val_err(&v, 255); return v; }
                    err = csn_sub(&t.num, &a.num, &g_reg_icept);
                    if (err) { val_free(&a); val_err(&v, 255); return v; }
                    err = csn_div(&v.num, &t.num, &g_reg_slope);
                    if (err) { val_free(&a); val_err(&v, 255); return v; }
                }
                val_free(&a);
                v.good = 1;
                return v;
            }
            if (f == FN_MOD || f == FN_POWER || f == FN_COMBIN ||
                f == FN_PERMUT || f == FN_GCD || f == FN_LCM ||
                f == FN_ROUND || f == FN_TRUNC ||
                f == FN_NCR || f == FN_NPR) {
                Val a2;
                int err;
                Val a = parse_numarg(ps);
                if (!a.good) return a;
                skip_ws(&ps->p);
                if (*ps->p != ',') { val_err(&v, 3); val_free(&a); return v; }
                ps->p++;
                a2 = parse_numarg(ps);
                if (!a2.good) { val_free(&a); val_free(&a2); return a2; }
                skip_ws(&ps->p);
                if (*ps->p != ')') { val_err(&v, 3); val_free(&a); val_free(&a2); return v; }
                ps->p++;
                val_free(&v);
                switch (f) {
                case FN_MOD:
                    err = csn_mod(&v.num, &a.num, &a2.num);
                    break;
                case FN_POWER:
                    err = csn_pow(&v.num, &a.num, &a2.num);
                    break;
                case FN_COMBIN:
                case FN_NCR:
                    err = csn_comb(&v.num, &a.num, &a2.num);
                    break;
                case FN_PERMUT:
                case FN_NPR:
                    err = csn_perm(&v.num, &a.num, &a2.num);
                    break;
                case FN_GCD:
                    err = csn_gcd(&v.num, &a.num, &a2.num);
                    break;
                case FN_LCM:
                    err = csn_lcm(&v.num, &a.num, &a2.num);
                    break;
                case FN_ROUND:
                    err = csn_round(&v.num, &a.num, &a2.num);
                    break;
                default:
                    err = csn_trunc(&v.num, &a.num, &a2.num);
                    break;
                }
                val_free(&a); val_free(&a2);
                if (err) {
                    /* div by zero (MOD 0) and overflow use Error 0;
                     * domain errors (fact limits) also map to Error 0 */
                    val_err(&v, 0);
                    return v;
                }
                v.good = 1;
                return v;
            }
            if (f == FN_REGR) {
                Val a = parse_condexpr(ps);
                Val b;
                int c1, r1, c2, r2, c3, r3;
                if (!a.good) return a;
                if (a.type != VT_RANGE) { val_free(&a); val_err(&v, 2); return v; }
                c1 = a.rng.start.col; r1 = a.rng.start.row;
                c2 = a.rng.end.col;   r2 = a.rng.end.row;
                val_free(&a);
                skip_ws(&ps->p);
                if (*ps->p != ',') { val_err(&v, 3); return v; }
                ps->p++;
                memset(&b, 0, sizeof(b));
                csn_zero(&b.num);
                b = parse_condexpr(ps);
                if (!b.good) { val_free(&b); return b; }
                /* the dependency anchor: exactly one Reference */
                if (b.type == VT_REF) {
                    c3 = b.ref.col; r3 = b.ref.row;
                } else {
                    val_free(&b);
                    val_err(&v, 2);
                    return v;
                }
                val_free(&b);
                skip_ws(&ps->p);
                if (*ps->p != ')') { val_err(&v, 3); return v; }
                ps->p++;
                if (c1 != c2 && r1 != r2) { val_err(&v, 2); return v; }   /* straight line */
                if (c3 >= CS_MAXCOL || r3 >= CS_MAXROW) { val_err(&v, 255); return v; }
                {
                    int vertical = (c1 == c2);
                    int alen = vertical ? (r2 - r1 + 1) : (c2 - c1 + 1);
                    Csn sx, sy, sxx, sxy, ncs, sxslope;
                    int i, n = 0, err;
                    csn_zero(&sx); csn_zero(&sy);
                    csn_zero(&sxx); csn_zero(&sxy);
                    csn_zero(&ncs); csn_zero(&sxslope);
                    for (i = 0; i < alen; i++) {
                        int cc = vertical ? c1 : c1 + i;
                        int rr = vertical ? r1 + i : r1;
                        int dc = vertical ? c3 : c3 + i;
                        int dr = vertical ? r3 + i : r3;
                        Cell *x = cs_cell(cc, rr);
                        Cell *y = cs_cell(dc, dr);
                        Csn px;
                        if (!x || !y || x->type != CE_NUM || x->iserr ||
                            y->type != CE_NUM || y->iserr) { val_err(&v, 9); return v; }
                        if (n < REGR_MAX) {
                            csn_copy(&gInd[n], &x->val);
                            csn_copy(&gDep[n], &y->val);
                            n++;
                            err = csn_add(&sx, &sx, &x->val); if (err) { val_err(&v, 255); return v; }
                            err = csn_add(&sy, &sy, &y->val); if (err) { val_err(&v, 255); return v; }
                            err = csn_mul(&px, &x->val, &x->val);
                            if (err) { val_err(&v, 255); return v; }
                            err = csn_add(&sxx, &sxx, &px); if (err) { val_err(&v, 255); return v; }
                            err = csn_mul(&px, &x->val, &y->val);
                            if (err) { val_err(&v, 255); return v; }
                            err = csn_add(&sxy, &sxy, &px); if (err) { val_err(&v, 255); return v; }
                        }
                    }
                    if (n == 0) { val_err(&v, 255); return v; }
                    csn_from_int(&ncs, n);
                    csn_mul(&sxslope, &ncs, &sxx);
                    err = csn_mul(&v.num, &sx, &sx);    /* sx * sx */
                    if (err) { val_err(&v, 255); return v; }
                    err = csn_sub(&sxslope, &sxslope, &v.num);  /* denom = n*sxx - sx^2 */
                    if (err) { val_err(&v, 255); return v; }
                    if (csn_is_zero(&sxslope)) { val_err(&v, 9); return v; }
                    /* slope = (n*sxy - sx*sy) / denom */
                    {
                        Csn numr, t;
                        err = csn_mul(&numr, &ncs, &sxy);
                        if (err) { val_err(&v, 255); return v; }
                        err = csn_mul(&t, &sx, &sy);
                        if (err) { val_err(&v, 255); return v; }
                        err = csn_sub(&numr, &numr, &t);
                        if (err) { val_err(&v, 255); return v; }
                        err = csn_div(&g_reg_slope, &numr, &sxslope);
                        if (err) { val_err(&v, 255); return v; }
                        /* icept = (sy - slope*sx) / n */
                        err = csn_mul(&t, &g_reg_slope, &sx);
                        if (err) { val_err(&v, 255); return v; }
                        err = csn_sub(&t, &sy, &t);
                        if (err) { val_err(&v, 255); return v; }
                        err = csn_div(&g_reg_icept, &t, &ncs);
                        if (err) { val_err(&v, 255); return v; }
                        /* meany = sy / n */
                        err = csn_div(&g_reg_meany, &sy, &ncs);
                        if (err) { val_err(&v, 255); return v; }
                        g_reg_ok = 1;
                        val_free(&v);
                        csn_copy(&v.num, &g_reg_meany); v.good = 1;
                        return v;
                    }
                }
            }
            /* SQRT LOG LN ABS EXP first */
            if (f == FN_SQRT || f == FN_LOG || f == FN_LN ||
                f == FN_ABS || f == FN_EXP ||
                f == FN_SIN || f == FN_COS || f == FN_TAN ||
                f == FN_ASIN || f == FN_ACOS || f == FN_ATAN ||
                f == FN_SIGN || f == FN_SGN || f == FN_DEGREES ||
                f == FN_DEG || f == FN_RADIANS ||
                f == FN_RAD || f == FN_FACT ||
                f == FN_INTG || f == FN_FRAC) {
                int err;
                Val a = parse_numarg(ps);
                if (!a.good) return a;
                skip_ws(&ps->p);
                if (*ps->p != ')') { val_err(&v, 3); val_free(&a); return v; }
                ps->p++;
                val_free(&v);
                switch (f) {
                case FN_SQRT:
                    if (a.num.sign && !csn_is_zero(&a.num)) { val_err(&v, 0); val_free(&a); return v; }
                    err = csn_sqrt(&v.num, &a.num);
                    break;
                case FN_LOG:
                    if (csn_is_zero(&a.num) || a.num.sign) { val_err(&v, 0); val_free(&a); return v; }
                    err = csn_log10(&v.num, &a.num);
                    break;
                case FN_LN:
                    if (csn_is_zero(&a.num) || a.num.sign) { val_err(&v, 0); val_free(&a); return v; }
                    err = csn_ln(&v.num, &a.num);
                    break;
                case FN_ABS:
                    csn_abs(&v.num, &a.num);
                    err = CSN_OK;
                    break;
                case FN_SIGN:
                case FN_SGN:
                    err = csn_sign(&v.num, &a.num);
                    break;
                case FN_DEGREES:
                case FN_DEG:
                    err = csn_degrees(&v.num, &a.num);
                    break;
                case FN_RADIANS:
                case FN_RAD:
                    err = csn_radians(&v.num, &a.num);
                    break;
                case FN_FACT:
                    err = csn_fact(&v.num, &a.num);
                    break;
                case FN_INTG:
                    /* the integer part: truncate toward zero (the
                     * manual's truncate rule) */
                    {
                        Csn zero;
                        csn_zero(&zero);
                        err = csn_trunc(&v.num, &a.num, &zero);
                    }
                    break;
                case FN_FRAC:
                    /* the fractional part, sign-preserving:
                     * FRAC(x) = x - INTG(x)  (-3.5 -> -0.5) */
                    {
                        Csn zero, t;
                        csn_zero(&zero);
                        err = csn_trunc(&t, &a.num, &zero);
                        if (!err) err = csn_sub(&v.num, &a.num, &t);
                    }
                    break;
                case FN_SIN:
                    err = csn_sin(&v.num, &a.num);
                    break;
                case FN_COS:
                    err = csn_cos(&v.num, &a.num);
                    break;
                case FN_TAN:
                    err = csn_tan(&v.num, &a.num);
                    break;
                case FN_ASIN:
                    err = csn_asin(&v.num, &a.num);
                    break;
                case FN_ACOS:
                    err = csn_acos(&v.num, &a.num);
                    break;
                case FN_ATAN:
                    err = csn_atan(&v.num, &a.num);
                    break;
                default:
                    err = csn_exp(&v.num, &a.num);   /* overflow -> range error */
                    break;
                }
                val_free(&a);
                if (err) { val_err(&v, 0); return v; }
                v.good = 1;
                return v;
            }
            /* SUM/CNT/AVG/MAX/MIN */
            {
                Csn list[LISTMAX];
                int cnt = 0, err = CSN_OK;
                Val ok = parse_list(ps, list, &cnt);
                if (!ok.good) return ok;
                val_free(&ok);
                skip_ws(&ps->p);
                if (*ps->p != ')') { val_err(&v, 3); return v; }
                ps->p++;
                val_free(&v);
                v.good = 1;
                if (cnt == 0) {
                    csn_zero(&v.num);
                    return v;
                }
                if (f == FN_SUM) {
                    int i;
                    csn_copy(&v.num, &list[0]);
                    for (i = 1; i < cnt; i++) {
                        err = csn_add(&v.num, &v.num, &list[i]);
                        if (err) { val_err(&v, 0); return v; }
                    }
                } else if (f == FN_CNT) {
                    csn_from_int(&v.num, cnt);
                } else if (f == FN_AVG) {
                    int i;
                    Csn ncs;
                    csn_copy(&v.num, &list[0]);
                    for (i = 1; i < cnt; i++) {
                        err = csn_add(&v.num, &v.num, &list[i]);
                        if (err) { val_err(&v, 0); return v; }
                    }
                    csn_from_int(&ncs, cnt);
                    err = csn_div(&v.num, &v.num, &ncs);
                    if (err) { val_err(&v, 0); return v; }
                } else if (f == FN_MAX) {
                    int i;
                    csn_copy(&v.num, &list[0]);
                    for (i = 1; i < cnt; i++)
                        if (csn_cmp(&list[i], &v.num) > 0)
                            csn_copy(&v.num, &list[i]);
                } else if (f == FN_MED) {
                    /* the median (user-ext 2026-11-13): the numbers
                     * sorted ascending; odd count -> the middle
                     * element, even count -> the average of the two
                     * middle elements (Excel-style); empty -> 0 like
                     * AVG; the values are the SAME list SUM/AVG
                     * collect (text/empties skipped, an error cell in
                     * a range is Error 0) */
                    int i, j;
                    Csn t;
                    for (i = 1; i < cnt; i++) {
                        t = list[i];
                        for (j = i - 1; j >= 0 && csn_cmp(&list[j], &t) > 0; j--)
                            list[j + 1] = list[j];
                        list[j + 1] = t;
                    }
                    if (cnt % 2 == 1) {
                        csn_copy(&v.num, &list[cnt / 2]);
                    } else {
                        Csn two, sum;
                        int err;
                        err = csn_add(&sum, &list[cnt / 2 - 1], &list[cnt / 2]);
                        if (err) { val_err(&v, 0); return v; }
                        csn_from_int(&two, 2);
                        err = csn_div(&v.num, &sum, &two);
                        if (err) { val_err(&v, 0); return v; }
                    }
                } else {
                    int i;
                    csn_copy(&v.num, &list[0]);
                    for (i = 1; i < cnt; i++)
                        if (csn_cmp(&list[i], &v.num) < 0)
                            csn_copy(&v.num, &list[i]);
                }
                return v;
            }
        }
    }
    val_err(&v, 6);
    ps->p = p;
    return v;
}

/* percent postfix binds to the primary just parsed */
static Val parse_unary(P *ps)
{
    const char *p = ps->p;
    Val v;
    memset(&v, 0, sizeof(v));
    skip_ws(&p);
    if (*p == '-' || *p == '+') {
        Val t;
        int neg = (*p == '-');
        int e;
        ps->p = p + 1;
        t = parse_unary(ps);
        if (!t.good) return t;
        e = val_deref(&t);
        if (e >= 0) { val_free(&t); val_err(&t, e); return t; }
        if (t.type == VT_STR) { val_free(&t); val_err(&t, 6); return t; }
        if (neg) csn_neg(&t.num, &t.num);
        return t;
    }
    v = parse_primary(ps);
    if (!v.good) return v;
    /* NOTE: no eager dereference here - a Reference must survive to
     * the operators (the '>' range construction sees raw references);
     * each operator dereferences its operands at the point of use */
    /* '**' power (manual 4.7): right-associative, binds tighter than
     * '*' and '/' so that 12*10**-3 is 12*(10**-3) */
    for (;;) {
        const char *p2 = ps->p;
        skip_ws(&p2);
        if (*p2 == '*' && p2[1] == '*') {
            Val b;
            int err, e;
            ps->p = p2 + 2;
            b = parse_unary(ps);
            if (!b.good) { val_free(&v); val_free(&b); return b; }
            e = val_deref(&v);
            if (e >= 0) { val_free(&v); val_free(&b); val_err(&v, e); return v; }
            e = val_deref(&b);
            if (e >= 0) { val_free(&v); val_free(&b); val_err(&v, e); return v; }
            if (v.type == VT_STR || b.type == VT_STR) { val_free(&v); val_free(&b); val_err(&v, 6); return v; }
            if (v.type == VT_RANGE || b.type == VT_RANGE) { val_free(&v); val_free(&b); val_err(&v, 6); return v; }
            err = csn_pow(&v.num, &v.num, &b.num);
            val_free(&b);
            if (err) { val_err(&v, 0); return v; }
            continue;
        }
        break;
    }
    for (;;) {
        const char *p2 = ps->p;
        skip_ws(&p2);
        if (*p2 == '%') {
            int err, e;
            e = val_deref(&v);
            if (e >= 0) { val_free(&v); val_err(&v, e); return v; }
            if (v.type == VT_STR || v.type == VT_RANGE) { val_free(&v); val_err(&v, 6); return v; }
            err = csn_scale10(&v.num, &v.num, -2);   /* v / 100 exactly */
            if (err) { val_err(&v, 0); return v; }
            v.type = VT_NUM;
            ps->p = p2 + 1;
            /* "140%E4": a percent followed by a term multiplies it by
               the term (140% x E4 = 1.4 x E4) */
            {
                const char *q2 = ps->p;
                skip_ws(&q2);
                if ((*q2 >= '0' && *q2 <= '9') || *q2 == '.' ||
                    *q2 == '(' || *q2 == '"' ||
                    ((*q2 >= 'A' && *q2 <= 'Z') || (*q2 >= 'a' && *q2 <= 'z'))) {
                    Val b = parse_unary(ps);
                    if (!b.good) { val_free(&v); val_free(&b); return b; }
                    {
                        int e = val_deref(&b);
                        if (e >= 0) { val_free(&v); val_free(&b); val_err(&v, e); return v; }
                    }
                    if (v.type == VT_STR || b.type == VT_STR) { val_free(&v); val_free(&b); val_err(&v, 6); return v; }
                    if (v.type == VT_RANGE || b.type == VT_RANGE) { val_free(&v); val_free(&b); val_err(&v, 6); return v; }
                    err = csn_mul(&v.num, &v.num, &b.num);
                    val_free(&b);
                    if (err) { val_err(&v, 0); return v; }
                    continue;
                }
            }
            continue;
        }
        break;
    }
    return v;
}

static Val parse_mul(P *ps)
{
    Val a;
    memset(&a, 0, sizeof(a));
    a = parse_unary(ps);
    if (!a.good) return a;
    /* no eager dereference: 'a' stays raw (possibly a Reference) until
     * an operator actually consumes it */
    for (;;) {
        const char *p = ps->p;
        Val b;
        int op, err, e;
        memset(&b, 0, sizeof(b));
        skip_ws(&p);
        if (*p == ':') break;
        if (*p != '*' && *p != '/') break;
        op = (*p == '*') ? 0 : 1;
        ps->p = p + 1;
        b = parse_unary(ps);
        if (!b.good) { val_free(&a); val_free(&b); return b; }
        /* both operands are consumed here: deref (option 3) */
        e = val_deref(&a);
        if (e >= 0) { val_free(&a); val_free(&b); val_err(&a, e); return a; }
        e = val_deref(&b);
        if (e >= 0) { val_free(&a); val_free(&b); val_err(&a, e); return a; }
        if (a.type == VT_STR || b.type == VT_STR) { val_free(&a); val_free(&b); val_err(&a, 6); return a; }
        if (a.type == VT_RANGE || b.type == VT_RANGE) { val_free(&a); val_free(&b); val_err(&a, 6); return a; }
        if (a.type == VT_BOOL && b.type == VT_BOOL && op == 0) {
            /* logical AND */
            csn_from_int(&a.num,
                         (!csn_is_zero(&a.num) && !csn_is_zero(&b.num)) ? 1 : 0);
        } else if (op == 0) {
            err = csn_mul(&a.num, &a.num, &b.num);
            if (err) { val_free(&a); val_free(&b); val_err(&a, 0); return a; }
        } else {
            if (csn_is_zero(&b.num)) { val_free(&a); val_free(&b); val_err(&a, 0); return a; }
            err = csn_div(&a.num, &a.num, &b.num);
            if (err) { val_free(&a); val_free(&b); val_err(&a, 0); return a; }
        }
        val_free(&b);
    }
    return a;
}

static Val parse_add(P *ps)
{
    Val a;
    memset(&a, 0, sizeof(a));
    a = parse_mul(ps);
    if (!a.good) return a;
    /* no eager dereference: 'a' stays raw until an operator consumes it */
    for (;;) {
        const char *p = ps->p;
        Val b;
        int op, err, e;
        memset(&b, 0, sizeof(b));
        skip_ws(&p);
        if (*p == ':') break;
        if (*p != '+' && *p != '-') break;
        op = *p;
        if (p[1] == '\0') { val_free(&a); val_err(&a, 7); ps->p = p + 1; return a; }
        ps->p = p + 1;
        b = parse_mul(ps);
        if (!b.good) { val_free(&a); val_free(&b); return b; }
        /* both operands are consumed here: deref (option 3) */
        e = val_deref(&a);
        if (e >= 0) { val_free(&a); val_free(&b); val_err(&a, e); return a; }
        e = val_deref(&b);
        if (e >= 0) { val_free(&a); val_free(&b); val_err(&a, e); return a; }
        if (op == '+') {
            if (a.type == VT_BOOL && b.type == VT_BOOL) {
                /* logical OR */
                csn_from_int(&a.num,
                             (!csn_is_zero(&a.num) || !csn_is_zero(&b.num)) ? 1 : 0);
            } else {
                err = csn_add(&a.num, &a.num, &b.num);
                if (err) { val_free(&a); val_free(&b); val_err(&a, 0); return a; }
                a.type = VT_NUM;
            }
        } else {
            err = csn_sub(&a.num, &a.num, &b.num);
            if (err) { val_free(&a); val_free(&b); val_err(&a, 0); return a; }
            a.type = VT_NUM;
        }
        val_free(&b);
    }
    return a;
}

static Val parse_cmp(P *ps)
{
    Val a;
    memset(&a, 0, sizeof(a));
    a = parse_add(ps);
    if (!a.good) return a;
    {
        const char *p = ps->p;
        Val b;
        int op, r, e;
        memset(&b, 0, sizeof(b));
        skip_ws(&p);
        if (*p == ':') return a;
        if (*p == '<') {
            if (p[1] == '=') op = 2;       /* <= */
            else if (p[1] == '>') op = 4;  /* <> */
            else op = 1;                   /* <  */
        } else if (*p == '>') op = 5;
        else if (*p == '=') op = 3;
        else return a;
        ps->p = p + ((op == 2 || op == 4) ? 2 : (op == 5 ? 1 : 1));
        memset(&b, 0, sizeof(b));
        csn_zero(&b.num);
        b = parse_add(ps);
        if (!b.good) { val_free(&a); val_free(&b); return b; }
        /* range construction: reference > reference (user-ext 2026-11)
         * - decided BEFORE any dereference, so it never competes with
         * the numeric comparison.  The range is a normalized
         * RECTANGLE (user decision 2026-11: A1>E5 addresses the whole
         * block; REGR keeps its own 1-D requirement) */
        if (op == 5 && a.type == VT_REF && b.type == VT_REF) {
            CsRng rg;
            rg.start = a.ref;
            rg.end = b.ref;
            cs_range_norm(&rg.start.col, &rg.start.row,
                          &rg.end.col, &rg.end.row);
            val_free(&b);
            val_free(&a);
            memset(&a, 0, sizeof(a));
            csn_zero(&a.num);
            a.type = VT_RANGE;
            a.rng = rg;
            a.good = 1;
            return a;
        }
        /* comparisons consume values: references deref (option 3) */
        e = val_deref(&a);
        if (e >= 0) { val_free(&a); val_free(&b); val_err(&a, e); return a; }
        e = val_deref(&b);
        if (e >= 0) { val_free(&a); val_free(&b); val_err(&a, e); return a; }
        if (a.type == VT_STR || b.type == VT_STR) { val_free(&a); val_free(&b); val_err(&a, 6); return a; }
        if (a.type == VT_RANGE || b.type == VT_RANGE) { val_free(&a); val_free(&b); val_err(&a, 6); return a; }
        r = csn_cmp(&a.num, &b.num);
        switch (op) {
        case 1: r = (r < 0); break;
        case 2: r = (r <= 0); break;
        case 3: r = (r == 0); break;
        case 4: r = (r != 0); break;
        default: r = (r > 0); break;
        }
        csn_from_int(&a.num, r ? 1 : 0);
        a.type = VT_BOOL;
        val_free(&b);
        return a;
    }
}

/* ---- conditional branches and side-effect statements ---------------- */

/* the side-effect state (eval.c): RECALC()/RECALC(cell) request that
 * the calling context iterates whole-sheet passes */
extern int cs_recalc_requested;

/* poll for an ESC keystroke (screen.c, non-blocking); 1 = ESC seen */
extern int cs_poll_escape(void);

/* execute SET(cell,value): the value is fully evaluated first, then
 * the target is written (SET(A1,A1+1) uses A1's previous value) */
static void exec_set_stmt(const char *args, int *err)
{
    const char *w;
    P sub;
    Val tgt, v;
    char just, oldjust;
    Cell *cell;
    int e, oldtype;
    Csn oldval;
    char *oldcont;

    *err = -1;
    /* the target: a general expression that must resolve to exactly
     * one Reference (a Range target is Error 6); NO dereference - the
     * reference is the cell to write */
    w = args;
    while (*w == ' ') w++;
    memset(&sub, 0, sizeof(sub));
    sub.p = w;
    sub.dry = 0;
    tgt = parse_condexpr(&sub);
    if (!tgt.good) {
        *err = tgt.err;
        val_free(&tgt);
        return;
    }
    if (tgt.type != VT_REF) { val_free(&tgt); *err = 6; return; }
    cell = cs_cell(tgt.ref.col, tgt.ref.row);
    if (!cell) { val_free(&tgt); *err = 1; return; }
    skip_ws(&sub.p);
    if (*sub.p != ',') { val_free(&tgt); *err = 7; return; }
    sub.p++;

    /* the value: fully evaluated BEFORE the target is modified */
    v = parse_condexpr(&sub);
    if (!v.good) {
        *err = v.err;                   /* 0 = the value-range class */
        val_free(&v);
        return;
    }
    while (*sub.p == ' ') sub.p++;
    if (*sub.p) { val_free(&v); val_free(&tgt); *err = 7; return; }
    e = val_deref(&v);                  /* option 3: a value derefs */
    if (e >= 0) { val_free(&v); val_free(&tgt); *err = e; return; }
    if (v.type == VT_RANGE) { val_free(&v); val_free(&tgt); *err = 6; return; }

    /* the entry-pool budget for a newly allocated cell */
    if (cell->type == CE_EMPTY && !cs_ensure_room()) {
        cs_msg("MEMORY IS TOO LOW");
        *err = 0;                       /* the value-range class */
        val_free(&v);
        val_free(&tgt);
        return;
    }

    /* the change notification is HONEST (the stability criterion is
     * "a pass changes no stored value"): snapshot the old state and
     * mark the sheet dirty only when the stored state really differs */
    oldtype = cell->type;
    csn_copy(&oldval, &cell->val);
    oldjust = cell->just;
    oldcont = cell->cont ? cs_strdup(cell->cont) : NULL;

    if (v.type == VT_STR) {
        /* text: an optional \r / \l / \c prefix sets the justification */
        const char *txt = v.str;
        just = 'L';
        if (txt[0] == '\\' && (txt[1] == 'r' || txt[1] == 'R')) { just = 'R'; txt += 2; }
        else if (txt[0] == '\\' && (txt[1] == 'l' || txt[1] == 'L')) { just = 'L'; txt += 2; }
        else if (txt[0] == '\\' && (txt[1] == 'c' || txt[1] == 'C')) { just = 'C'; txt += 2; }
        cs_set_text(cell, txt, just);
    } else {
        /* numeric: the cell becomes a plain value (no reference text) */
        char fb[64];
        csn_fmt_fixed(&v.num, 12, fb, (int) sizeof(fb));
        if (!fb[0]) csn_fmt_sci(&v.num, 12, fb, (int) sizeof(fb));
        cs_set_num(cell, fb, &v.num, 0, NULL);
    }
    /* idempotent writes do not keep the iteration alive */
    if (oldtype == CE_EMPTY ||
        oldtype != cell->type ||
        (cell->type == CE_NUM && csn_cmp(&cell->val, &oldval) != 0) ||
        (cell->type == CE_TEXT &&
         (oldjust != cell->just ||
          (oldcont ? !cell->cont || strcmp(oldcont, cell->cont) != 0
                   : cell->cont != NULL)))) {
        cs_mark_sheet_change();
    }
    if (oldcont) free(oldcont);
    val_free(&v);
    val_free(&tgt);
    cs_recount_entries();
}

/* execute RECALC() / RECALC(cell): both mark the recalculation request
 * (the whole-sheet passes re-evaluate every cell, so the requested
 * cell is included - flagged deviation from the letter of the spec) */
static void exec_recalc_stmt(const char *args, int *err)
{
    const char *w;
    P sub;
    Val a;

    *err = -1;
    w = args;
    while (*w == ' ') w++;
    if (*w) {
        memset(&sub, 0, sizeof(sub));
        sub.p = w;
        a = parse_condexpr(&sub);
        if (!a.good) {
            *err = a.err;
            val_free(&a);
            return;
        }
        /* the argument is a single Reference */
        if (a.type != VT_REF) { val_free(&a); *err = 6; return; }
        val_free(&a);
    }
    cs_recalc_requested = 1;
}

/* execute one statement piece.  Returns 1 = the piece was a
 * side-effect call (*err: -1 = ok, else the error code), 0 = not a
 * side-effect call (a value piece). */
static int side_effect_stmt(const char *piece, int *err, int dry)
{
    char buf[CS_EDITMAX + 4];
    int sl, isset, isrec;
    const char *kw;
    *err = -1;
    while (*piece == ' ') piece++;
    sl = (int) strlen(piece);
    while (sl > 0 && piece[sl - 1] == ' ') sl--;
    if (sl < 4 || sl >= (int) sizeof(buf)) return 0;
    memcpy(buf, piece, (size_t) sl);
    buf[sl] = '\0';
    if (buf[sl - 1] != ')') return 0;
    isset = (toupper((unsigned char) buf[0]) == 'S' &&
             toupper((unsigned char) buf[1]) == 'E' &&
             toupper((unsigned char) buf[2]) == 'T' &&
             buf[3] == '(');
    isrec = (toupper((unsigned char) buf[0]) == 'R' &&
             toupper((unsigned char) buf[1]) == 'E' &&
             toupper((unsigned char) buf[2]) == 'C' &&
             toupper((unsigned char) buf[3]) == 'A' &&
             toupper((unsigned char) buf[4]) == 'L' &&
             toupper((unsigned char) buf[5]) == 'C' &&
             buf[6] == '(');
    if (!isset && !isrec) return 0;
    kw = isset ? buf + 3 : buf + 6;
    buf[sl - 1] = '\0';                 /* drop the closing ')' */
    if (dry) {
        /* parse-only mode (the factorial-entry syntax check): the
         * statement is recognized, not executed */
        *err = -1;
        return 1;
    }
    if (isset) exec_set_stmt(kw + 1, err);
    else exec_recalc_stmt(kw + 1, err);
    /* the exec errors propagate: run_branch treats a return of 2 with
     * *err >= 0 as a failing side-effect call (0 = the value-range
     * class is a real error too) */
    return (*err == -1) ? 1 : 2;
}

/* a conditional branch as a statement list: pieces split at ';'
 * (depth- and quote-aware).  Exactly one piece is the value-producing
 * expression; the others must be side-effect calls, executed in
 * written order.  Returns 0 on success (the result in *rv), else the
 * error code (13 = user interrupt). */
static int run_branch(char *text, Val *rv, int dry)
{
    char *p = text;
    int values = 0;
    const char *w = text;
    val_free(rv);
    memset(rv, 0, sizeof(*rv));
    csn_zero(&rv->num);
    while (*w == ' ') w++;
    if (!*w) {                          /* an empty branch assigns 0 */
        rv->good = 1;
        return 0;
    }
    for (;;) {
        char *q = p;
        int depth = 0, inq = 0, save, r, serr;
        while (*q) {
            if (*q == '"') inq = !inq;
            else if (!inq && *q == '(') depth++;
            else if (!inq && *q == ')') depth--;
            else if (!inq && depth == 0 && *q == ';') break;
            q++;
        }
        save = *q;
        *q = '\0';
        r = side_effect_stmt(p, &serr, dry);
        if (r == 2) {                       /* a failing side-effect call */
            val_free(rv);
            memset(rv, 0, sizeof(*rv));
            csn_zero(&rv->num);
            val_err(rv, serr);
            return 1;
        }
        if (r == 0) {
            /* the value piece: exactly one per branch */
            P sub;
            Val v;
            if (values) { val_err(rv, 7); return 1; }
            memset(&sub, 0, sizeof(sub));
            sub.p = p;
            sub.dry = dry;
            v = parse_condexpr(&sub);
            if (!v.good) {
                int e = v.err;
                val_free(&v);
                val_err(rv, e);
                return 1;
            }
            while (*sub.p == ' ') sub.p++;
            if (*sub.p) { val_free(&v); val_err(rv, 7); return 1; }
            *rv = v;
            values++;
        }
        if (!dry && cs_poll_escape()) {     /* interrupt between statements */
            val_free(rv);
            memset(rv, 0, sizeof(*rv));
            csn_zero(&rv->num);
            val_err(rv, KW_ABORT);
            cs_msg("User interrupted execution");
            return 1;
        }
        *q = (char) save;
        if (!save) break;
        p = q + 1;
    }
    if (values != 1) { val_err(rv, 7); return 1; }
    return 0;
}

/* the extent of a conditional branch starting at q: the matching ':'
 * at paren depth 0 (outside quotes), or the enclosing ',' / ')', or
 * the string end.  *term receives the terminator (or '\0'). */
static const char *branch_end(const char *q, char *term)
{
    int depth = 0, inq = 0;
    *term = '\0';
    for (; *q; q++) {
        if (*q == '"') inq = !inq;
        else if (!inq && *q == '(') depth++;
        else if (!inq && *q == ')') {
            if (depth == 0) { *term = ')'; return q; }
            depth--;
        }
        else if (!inq && depth == 0 && *q == ',') { *term = ','; return q; }
        else if (!inq && depth == 0 && *q == ':') { *term = ':'; return q; }
    }
    return q;
}

static Val parse_condexpr(P *ps)
{
    Val cond;
    memset(&cond, 0, sizeof(cond));
    cond = parse_cmp(ps);
    if (!cond.good) return cond;
    {
        const char *p = ps->p;
        skip_ws(&p);
        if (*p == ':') {
            /* the condition is consumed HERE: dereference it (option 3)
             * before the truth test - a bare Reference cond reads its
             * cell; a Range cond is a type error */
            int de = val_deref(&cond);
            if (de >= 0) { val_free(&cond); val_err(&cond, de); return cond; }
        }
        if (*p == ':') {
            /* DEFERRED branch execution: the branch texts are copied
             * and only the SELECTED one is parsed and executed - the
             * unselected branch must not run its side effects */
            char tbuf[CS_EDITMAX + 4], fbuf[CS_EDITMAX + 4];
            const char *q = p + 1, *fe;
            char term;
            int tl, fl, ctrue, r;
            Val t, f;
            q = branch_end(q, &term);
            tl = (int) (q - (p + 1));
            if (tl >= (int) sizeof(tbuf)) tl = (int) sizeof(tbuf) - 1;
            memcpy(tbuf, p + 1, (size_t) tl);
            tbuf[tl] = '\0';
            if (term == ':') {
                fe = branch_end(q + 1, &term);
                fl = (int) (fe - (q + 1));
                if (fl >= (int) sizeof(fbuf)) fl = (int) sizeof(fbuf) - 1;
                memcpy(fbuf, q + 1, (size_t) fl);
                fbuf[fl] = '\0';
                ps->p = fe;             /* the enclosing context continues */
            } else {
                fbuf[0] = '\0';
                ps->p = q;
            }
            ctrue = cond.type == VT_STR ? (cond.str && cond.str[0])
                                        : !csn_is_zero(&cond.num);
            memset(&t, 0, sizeof(t)); csn_zero(&t.num);
            memset(&f, 0, sizeof(f)); csn_zero(&f.num);
            t.good = 1; f.good = 1;     /* empty branches assign 0 */
            r = run_branch(ctrue ? tbuf : fbuf, ctrue ? &t : &f, ps->dry);
            if (r) {
                /* the Val carries the error code (13 = user interrupt) */
                if (ctrue) { val_free(&f); val_free(&cond); return t; }
                val_free(&t); val_free(&cond);
                return f;
            }
            if (ctrue) { val_free(&f); val_free(&cond); return t; }
            val_free(&t); val_free(&cond);
            return f;
        }
    }
    return cond;
}

/* the shared evaluation core: parse one expression and return the
 * TYPED value (a Reference or Range stays intact for the caller) */
static int cs_eval_typed(const char *s, Val *out)
{
    P ps;
    Val v;
    char buf[CS_EDITMAX + 4];
    int i, n = 0, inq = 0;

    /* CalcStar's '\' inline comment (manual II.18): everything from a
     * backslash to the end of the entry is non-evaluative.  The cut
     * happens outside quoted strings so "...\..." text keeps it. */
    for (i = 0; s[i]; i++) {
        char c = s[i];
        if (c == '"') inq = !inq;
        if (c == '\\' && !inq) break;
        if (n < CS_EDITMAX) buf[n++] = c;
    }
    buf[n] = '\0';

    memset(out, 0, sizeof(*out));
    csn_zero(&out->num);
    ps.p = buf;
    ps.depth = 0;
    ps.dry = 0;
    v = parse_condexpr(&ps);
    if (!v.good) {
        int e = v.err;
        val_free(&v);
        val_err(out, e);
        return e ? e : -1;    /* -1 == CalcStar "Error 0" (value-range) */
    }
    {
        const char *p = ps.p;
        while (*p == ' ') p++;
        if (*p) { val_free(&v); val_err(out, 7); return 7; }  /* junk */
    }
    *out = v;
    return 0;
}

int cs_eval_formula(const char *s, Csn *val, char **str)
{
    Val v;
    int e = cs_eval_typed(s, &v);
    if (e) {
        csn_zero(val);
        return e;
    }
    /* a top-level Reference derefs (option 3); a Range is not a
     * storable result */
    if (v.type == VT_REF) {
        int e = val_deref(&v);
        if (e >= 0) { val_free(&v); csn_zero(val); return e; }
    }
    if (v.type == VT_RANGE) { val_free(&v); csn_zero(val); return 6; }
    if (v.type == VT_STR) {
        csn_zero(val);
        if (str) *str = v.str; else free(v.str);
        return 0;
    }
    val_free(&v);
    csn_copy(val, &v.num);
    if (str) *str = NULL;
    return 0;
}

/* parse-only syntax check (user-ext 2026-11): 0 = the text is a
 * valid expression (side-effect statements are detected, NOT run);
 * else the parse error code */
int cs_parse_only(const char *s)
{
    Val v;
    int e = cs_eval_typed(s, &v);
    val_free(&v);
    return e;
}

/* evaluate an expression to a NUMBER for the factorial entries
 * (user-ext 2026-11-13): 0 = ok with the value in *out (References
 * coerce), else the error code and *out zeroed */
int cs_eval_fact_source(const char *s, Csn *out)
{
    Val v;
    int e = cs_eval_typed(s, &v);
    if (e) { csn_zero(out); return e; }
    {
        int d = val_deref(&v);
        if (d >= 0) { val_free(&v); csn_zero(out); return d; }
    }
    if (v.type != VT_NUM) { val_free(&v); csn_zero(out); return 6; }
    csn_copy(out, &v.num);
    val_free(&v);
    return 0;
}

/* evaluate an expression that designates a RANGE (user-ext 2026-11):
 * the command dialogs use it when their static coordinate text does
 * not parse.  Accepts a Range value or a single Reference (a
 * one-cell range); the range arrives normalized and straight-line
 * checked from parse_cmp. */
/* evaluate a sort-key column answer (user-ext 2026-11): a bare
 * designator word (A..DW, cleanly ended), or a general expression
 * that yields a Number (the 1-based index) or a Reference (its
 * column).  Returns the 1-based index, or -1 with *err set. */
int cs_eval_keycol(const char *s, int *err)
{
    Val v;
    int e, idx;
    const char *p = s;
    /* the bare designator word: 1-2 letters cleanly ended */
    if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
        const char *q = p;
        while ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z')) q++;
        if (q - p <= 2 && *q == '\0') {
            idx = colidx_word(p, (int) (q - p));
            if (!idx) { *err = 1; return -1; }
            return idx;
        }
    }
    e = cs_eval_typed(s, &v);
    if (e) { *err = e; return -1; }
    if (v.type == VT_NUM) {
        Csn t;
        csn_zero(&t);
        if (!csn_integer_part(&v.num, &t)) { val_free(&v); *err = 0; return -1; }
        idx = (int) csn_to_long_val(&t);
    } else if (v.type == VT_REF) {
        idx = v.ref.col + 1;
    } else {
        val_free(&v);
        *err = 6;
        return -1;
    }
    val_free(&v);
    if (idx < 1 || idx > CS_MAXCOL) { *err = 1; return -1; }
    return idx;
}

int cs_eval_range(const char *s, int *c1, int *r1, int *c2, int *r2)
{
    Val v;
    int e = cs_eval_typed(s, &v);
    if (e) return e;
    if (v.type == VT_REF) {
        *c1 = *c2 = v.ref.col;
        *r1 = *r2 = v.ref.row;
        val_free(&v);
        return 0;
    }
    if (v.type == VT_RANGE) {
        *c1 = v.rng.start.col;
        *r1 = v.rng.start.row;
        *c2 = v.rng.end.col;
        *r2 = v.rng.end.row;
        val_free(&v);
        return 0;
    }
    val_free(&v);
    return 6;                           /* not a reference/range */
}
