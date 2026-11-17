/* parser.c - CalcStar expression tokenizer, parser and evaluator.
 *
 * All arithmetic is BCD decimal through num.c - no floating point
 * anywhere, matching the original CalcStar engine. */
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "cs.h"

#define MAXDEPTH 12

/* Val.good == 0 : error, .err holds the CalcStar evaluation error code
   (0 = value range error, 1 illegal coord, 2 bad range, 3 missing paren,
   4 unknown function, 6 illegal terminal expr, 7 trailing junk,
   8 bad number, 9 regression error, 12 paren depth, 255 regr value) */

/* first-class references and ranges (user-ext 2026-11): lightweight,
 * no dynamic memory - a bare cell identifier evaluates to a VT_REF
 * and VAL() retrieves its value (option-3 semantics: references
 * coerce to values where a value is consumed) */
typedef struct {
    int col;       /* 0-based coordinate */
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
/* the evaluation context: WHICH cell a formula is being evaluated
 * for.  Set by the recalculation, the commit and the copy (ROW()/
 * COL() then answer the cell's own position); clear for the
 * context-free evaluations (the ?-calculator, the dialogs) where the
 * current cursor answers. */
int cs_eval_ctx_on = 0;
int cs_eval_ctx_col = 0, cs_eval_ctx_row = 0;

/* the Zadeh fuzzy algebra (2026-11-15): the BCD constants for the
 * domain [0,1] and the single Boolean-interpretation threshold
 * (FUZZY_TRUE_THRESHOLD = 0.7 - the ONLY place the threshold value
 * is written) */
#define FUZZY_TRUE_THRESHOLD "0.7"
static Csn cs_fuzzy_zero, cs_fuzzy_one, cs_fuzzy_true;
static int cs_fuzzy_init = 0;
static void cs_fuzzy_consts(void)
{
    if (!cs_fuzzy_init) {
        csn_parse("0", &cs_fuzzy_zero);
        csn_parse("1", &cs_fuzzy_one);
        csn_parse(FUZZY_TRUE_THRESHOLD, &cs_fuzzy_true);
        cs_fuzzy_init = 1;
    }
}

int cs_empty_strict = 0;    /* 1 = an empty reference is Error 1 (the
                             * factorial entry's "?n? until filled"
                             * semantics); 0 = the manual's empty-is-
                             * zero arithmetic rule */

static int val_deref(Val *v)
{
    Cell *cl;
    if (v->type == VT_NUM || v->type == VT_BOOL || v->type == VT_STR)
        return -1;
    if (v->type != VT_REF) return 6;    /* a Range is not a value here */
    cl = cs_getcell(v->ref.col, v->ref.row);
    /* a formula whose current result is a string IS a text value
     * (the derived type, user report 2026-11-13) */
    if (cl && cl->strres && !cl->iserr) {
        v->str = cs_strdup(cl->strres);
        if (!v->str) { val_err(v, 0); return 0; }
        v->type = VT_STR;
        return -1;
    }
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
    /* an EMPTY (or allocated-empty) referenced cell is ZERO in
     * arithmetic - the manual's rule, restored 2026-11-14 (the user's
     * copy traces: a relative copy across empty cells must yield
     * numbers, not ?n?).  The strict "never silently 0" contract
     * lives in VAL(), the explicit typed accessor, and in the
     * factorial entry's prefix (cs_empty_strict: "?n? until filled").
     * An ERROR cell stays Error 1. */
    if ((!cl || cl->type == CE_EMPTY || cl->type == CE_ALLOC) &&
        !cs_empty_strict)
        return -1;                      /* the zeroed value stands:
                                         * absent = empty = 0 */
    return 1;                           /* an error / strict-empty cell */
}

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
    FN_SIN, FN_COS, FN_TAN, FN_ASIN, FN_ACOS, FN_ATAN, FN_PI, FN_E,
    FN_ROW, FN_COL,
    FN_SIGN, FN_MOD, FN_POWER, FN_LOG10, FN_DEGREES, FN_RADIANS,
    FN_DEG, FN_RAD,
    FN_FACT, FN_COMBIN, FN_PERMUT, FN_GCD, FN_LCM, FN_ROUND, FN_TRUNC,
    FN_REF, FN_VAL, FN_COLIDX, FN_MED,
    FN_SGN, FN_NCR, FN_NPR, FN_INTG, FN_FRAC, FN_INVFACT,
    FN_CHR, FN_ORD,
    FN_FNOT, FN_FAND, FN_FOR, FN_FXOR,
    FN_EVEN, FN_ODD,
    FN_HYPOT, FN_ATAN2, FN_CEIL, FN_FLOOR,
    FN_DB, FN_DBM, FN_DBW, FN_LIN, FN_STEP,
    FN_CLAMP, FN_DIST, FN_LERP, FN_UNLERP,
    FN_SUMSQ, FN_RMS, FN_VAR, FN_VARP, FN_STDEV, FN_STDEVP,
    FN_SUMABS, FN_SUMPOS, FN_SUMNEG, FN_MAXABS, FN_MINABS,
    FN_PCT, FN_QUART,
    FN_CORR, FN_RSQ, FN_INTEG,
    FN_CONV,
    FN_SPLINE, FN_DIFF,
    FN_ROOT,
    FN_ISPRIME,
    FN_RAND, FN_RANDF, FN_SEED
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
    if (!strcmp(name, "LG"))    return FN_LOG;    /* the base-10 alias */
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
    if (!strcmp(name, "ROW"))   return FN_ROW;
    if (!strcmp(name, "COL"))   return FN_COL;
    if (!strcmp(name, "E"))     return FN_E;
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
    if (!strcmp(name, "INVFACT")) return FN_INVFACT;
    if (!strcmp(name, "CHR"))   return FN_CHR;
    if (!strcmp(name, "NOT"))   return FN_FNOT;
    if (!strcmp(name, "AND"))   return FN_FAND;
    if (!strcmp(name, "OR"))    return FN_FOR;
    if (!strcmp(name, "XOR"))   return FN_FXOR;
    if (!strcmp(name, "EVEN"))  return FN_EVEN;
    if (!strcmp(name, "ODD"))   return FN_ODD;
    if (!strcmp(name, "HYPOT")) return FN_HYPOT;
    if (!strcmp(name, "ATAN2")) return FN_ATAN2;
    if (!strcmp(name, "CEIL"))  return FN_CEIL;
    if (!strcmp(name, "FLOOR")) return FN_FLOOR;
    if (!strcmp(name, "DB"))    return FN_DB;
    if (!strcmp(name, "DBM"))   return FN_DBM;
    if (!strcmp(name, "DBW"))   return FN_DBW;
    if (!strcmp(name, "LIN"))   return FN_LIN;
    if (!strcmp(name, "STEP"))  return FN_STEP;
    if (!strcmp(name, "CLAMP")) return FN_CLAMP;
    if (!strcmp(name, "DIST"))  return FN_DIST;
    if (!strcmp(name, "LERP"))  return FN_LERP;
    if (!strcmp(name, "UNLERP")) return FN_UNLERP;
    if (!strcmp(name, "SUMSQ")) return FN_SUMSQ;
    if (!strcmp(name, "RMS"))   return FN_RMS;
    if (!strcmp(name, "SUMABS")) return FN_SUMABS;
    if (!strcmp(name, "SUMPOS")) return FN_SUMPOS;
    if (!strcmp(name, "SUMNEG")) return FN_SUMNEG;
    if (!strcmp(name, "MAXABS")) return FN_MAXABS;
    if (!strcmp(name, "MINABS")) return FN_MINABS;
    if (!strcmp(name, "VAR"))   return FN_VAR;
    if (!strcmp(name, "VARP"))  return FN_VARP;
    if (!strcmp(name, "STDEV")) return FN_STDEV;
    if (!strcmp(name, "STDEVP")) return FN_STDEVP;
    if (!strcmp(name, "PCT"))   return FN_PCT;
    if (!strcmp(name, "QUART")) return FN_QUART;
    if (!strcmp(name, "CORR"))  return FN_CORR;
    if (!strcmp(name, "RSQ"))   return FN_RSQ;
    if (!strcmp(name, "INTEG")) return FN_INTEG;
    if (!strcmp(name, "CONV"))  return FN_CONV;
    if (!strcmp(name, "SPLINE")) return FN_SPLINE;
    if (!strcmp(name, "DIFF"))  return FN_DIFF;
    if (!strcmp(name, "ROOT"))  return FN_ROOT;
    if (!strcmp(name, "ISPRIME")) return FN_ISPRIME;
    if (!strcmp(name, "RAND"))   return FN_RAND;
    if (!strcmp(name, "RANDF"))  return FN_RANDF;
    if (!strcmp(name, "SEED"))   return FN_SEED;
    if (!strcmp(name, "ORD"))   return FN_ORD;
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

/* the integer as a plain long, WITHOUT atol: a checked digit loop
 * (the portability audit, 2026-11-16 - atol cannot reject a BCD
 * value wider than the host long, and a 32-bit long holds only 10
 * digits; the callers either bound the value first or treat a
 * failed fit as their error) */
static long csn_to_long_val(const Csn *v)
{
    char fb[80];
    long acc = 0;
    int i, neg = 0;
    csn_fmt_fixed(v, 0, fb, (int) sizeof(fb));
    if (!fb[0]) return 0;
    if (fb[0] == '-') { neg = 1; i = 1; } else i = 0;
    for (; fb[i]; i++) {
        int d = fb[i] - '0';
        if (fb[i] < '0' || fb[i] > '9') break;
        if (acc > (LONG_MAX - d) / 10) return neg ? LONG_MIN : LONG_MAX;
        acc = acc * 10 + d;
    }
    return neg ? -acc : acc;
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

/* ATAN2(y,x): the quadrant-aware angle (engineering batch A,
 * 2026-11-15).  x > 0: atan(y/x); x < 0: atan(y/x) +- pi by the sign
 * of y; x = 0: +-pi/2 by the sign of y; (0,0) is a domain error (the
 * C convention - the user's decision).  The result is in (-pi, pi]. */
static int eng_atan2(Csn *r, const Csn *y, const Csn *x)
{
    Csn zero, q, pi, two;
    int xs, ys, err;
    csn_zero(&zero);
    xs = csn_cmp(x, &zero);
    ys = csn_cmp(y, &zero);
    if (xs == 0 && ys == 0) return 1;           /* domain */
    if (xs > 0) {
        err = csn_div(&q, y, x);
        if (err) return err;
        return csn_atan(r, &q);
    }
    if (xs < 0) {
        err = csn_div(&q, y, x);
        if (err) return err;
        err = csn_atan(r, &q);
        if (err) return err;
        csn_pi(&pi);
        if (ys >= 0) return csn_add(r, r, &pi);
        return csn_sub(r, r, &pi);
    }
    /* x == 0: the vertical axis */
    csn_pi(&pi);
    csn_from_int(&two, 2);
    err = csn_div(r, &pi, &two);
    if (err) return err;
    if (ys < 0) csn_neg(r, r);
    return CSN_OK;
}

/* the PAIRED RANGE WALKER (engineering batch C, 2026-11-15): two
 * aligned ranges (the shapes must agree - the user's locked rule),
 * walked in row-major order.  The pairing policy differs by caller:
 * the statistics SKIP a pair when either side has no number, the
 * table functions treat a hole as an error - so the walker takes a
 * `holes` flag: 1 = skip text/empty pairs, 0 = Error 0.
 * Batch E splits the parse from the walk: DIFF/SPLINE take a
 * trailing scalar t that must be parsed BEFORE the walk. */

/* parse the two aligned ranges (and, with with_t, a trailing scalar
 * argument): 0 = ok, -1 = error (errp), -2 = shape mismatch */
static int pair_parse(P *ps, int with_t, Csn *tout, int *errp,
                      int *c1p, int *r1p, int *d1p, int *e1p,
                      int *swp, int *shp)
{
    Val a, b;
    int c1, r1, c2, r2, d1, e1, d2, e2, sw, sh, dw, dh;
    *errp = 0;
    a = parse_condexpr(ps);
    if (!a.good) { *errp = a.err ? a.err : 0; return -1; }
    if (a.type != VT_RANGE) { val_free(&a); *errp = 6; return -1; }
    c1 = a.rng.start.col; r1 = a.rng.start.row;
    c2 = a.rng.end.col;   r2 = a.rng.end.row;
    val_free(&a);
    skip_ws(&ps->p);
    if (*ps->p != ',') { *errp = 3; return -1; }
    ps->p++;
    b = parse_condexpr(ps);
    if (!b.good) { *errp = b.err ? b.err : 0; return -1; }
    if (b.type != VT_RANGE) { val_free(&b); *errp = 6; return -1; }
    d1 = b.rng.start.col; e1 = b.rng.start.row;
    d2 = b.rng.end.col;   e2 = b.rng.end.row;
    val_free(&b);
    if (with_t) {
        Val t;
        skip_ws(&ps->p);
        if (*ps->p != ',') { *errp = 3; return -1; }
        ps->p++;
        t = parse_numarg(ps);
        if (!t.good) { *errp = t.err ? t.err : 0; return -1; }
        csn_copy(tout, &t.num);
        val_free(&t);
    }
    skip_ws(&ps->p);
    if (*ps->p != ')') { *errp = 3; return -1; }
    ps->p++;
    sw = c2 - c1 + 1; sh = r2 - r1 + 1;
    dw = d2 - d1 + 1; dh = e2 - e1 + 1;
    if (sw != dw || sh != dh) { *errp = 0; return -2; }
    *c1p = c1; *r1p = r1; *d1p = d1; *e1p = e1;
    *swp = sw; *shp = sh;
    return 0;
}

/* the shared row-major walk: 0 = ok (the visit count is the
 * caller's), else the error code 0 (the value-range class) */
static int pair_walk(int c1, int r1, int d1, int e1, int sw, int sh,
                     int holes, int *errp, void *ctx,
                     int (*visit)(void *ctx, const Csn *x,
                                  const Csn *y))
{
    int i, j;
    for (i = 0; i < sh; i++)
        for (j = 0; j < sw; j++) {
            Cell *x = cs_getcell(c1 + j, r1 + i);
            Cell *y = cs_getcell(d1 + j, e1 + i);
            int e;
            int xok = (x && x->type == CE_NUM && !x->iserr &&
                       !x->strres);
            int yok = (y && y->type == CE_NUM && !y->iserr &&
                       !y->strres);
            if (!xok || !yok) {
                if (holes) continue;
                *errp = 0;
                return 1;
            }
            e = visit(ctx, &x->val, &y->val);
            if (e) { *errp = 0; return 1; }    /* any visit failure
                                                  is the value-range
                                                  class */
        }
    return 0;
}

/* the CORR/RSQ accumulators (the TWO-PASS CENTERED form, the user's
 * report 2026-11-16: the raw n*sxx - sx^2 form cancels catastrophically
 * when the values have large offsets but small variations; the means
 * come first, then the centered deviations).  Pass 1: the means. */
struct corr_mean {
    Csn sx, sy;
    long n;
};

static int corr_mean_visit(void *ctx, const Csn *x, const Csn *y)
{
    struct corr_mean *cm = (struct corr_mean *) ctx;
    int err;
    err = csn_add(&cm->sx, &cm->sx, x);
    if (err) return 1;
    err = csn_add(&cm->sy, &cm->sy, y);
    if (err) return 1;
    cm->n++;
    return 0;
}

/* pass 2: the centered deviations from the means */
struct corr_cen {
    Csn mx, my;
    Csn sxy, sxx, syy;
    long n;
};

static int corr_cen_visit(void *ctx, const Csn *x, const Csn *y)
{
    struct corr_cen *cc = (struct corr_cen *) ctx;
    Csn dx, dy, t;
    int err;
    err = csn_sub(&dx, x, &cc->mx);
    if (err) return 1;
    err = csn_sub(&dy, y, &cc->my);
    if (err) return 1;
    err = csn_mul(&t, &dx, &dx);
    if (err) return 1;
    err = csn_add(&cc->sxx, &cc->sxx, &t);
    if (err) return 1;
    err = csn_mul(&t, &dy, &dy);
    if (err) return 1;
    err = csn_add(&cc->syy, &cc->syy, &t);
    if (err) return 1;
    err = csn_mul(&t, &dx, &dy);
    if (err) return 1;
    err = csn_add(&cc->sxy, &cc->sxy, &t);
    if (err) return 1;
    cc->n++;
    return 0;
}

/* the DIFF accumulator (engineering batch E, 2026-11-15): the slope
 * of the piecewise-LINEAR interpolant at t.  t at an interior knot
 * uses the segment to the RIGHT; at the last knot the last segment;
 * t outside the span is an error (checked after the walk). */
struct diff_acc {
    Csn t;
    Csn slope;
    Csn px, py;         /* the previous (last) pair */
    Csn qx, qy;         /* the pair before it */
    long n;
    int found;
};

static int diff_visit(void *ctx, const Csn *x, const Csn *y)
{
    struct diff_acc *da = (struct diff_acc *) ctx;
    Csn dx, dy;
    int err;
    if (da->n == 0) {
        csn_copy(&da->px, x);
        csn_copy(&da->py, y);
        da->n = 1;
        return 0;
    }
    err = csn_sub(&dx, x, &da->px);
    if (err) return 1;
    if (csn_is_zero(&dx) || dx.sign) return 1;   /* not increasing */
    if (!da->found && csn_cmp(&da->t, &da->px) >= 0 &&
        csn_cmp(&da->t, x) < 0) {
        err = csn_sub(&dy, y, &da->py);
        if (err) return 1;
        err = csn_div(&da->slope, &dy, &dx);
        if (err) return 1;
        da->found = 1;
    }
    csn_copy(&da->qx, &da->px);
    csn_copy(&da->qy, &da->py);
    csn_copy(&da->px, x);
    csn_copy(&da->py, y);
    da->n++;
    return 0;
}

/* the SPLINE table (engineering batch E, 2026-11-15): the knots
 * collected through the pair walk (holes are errors), then the
 * natural cubic spline: second derivatives M via the THOMAS
 * algorithm on the tridiagonal system (the system is strictly
 * diagonally dominant - 2(h1+h2) vs h1+h2 - so no zero pivots), and
 * the standard piecewise evaluation
 *   S = A*y1 + B*y2 + ((A^3-A)*M1 + (B^3-B)*M2)*h^2/6,
 * A = (x2-t)/h, B = (t-x1)/h.  M0 = M(n-1) = 0 (natural); n = 2
 * degenerates to the straight line.  The bound is the MED buffer's
 * same 512 bound as the median's buffer (MED_MAX, defined later);
 * the worst-case transient is 5 Csn arrays (~130 KB, flagged - each
 * allocation stays under 64 KB). */
#define SPLINE_MAX 512

struct spline_tab {
    Csn *xs, *ys;
    long n;
    int cap;
};

static int spline_visit(void *ctx, const Csn *x, const Csn *y)
{
    struct spline_tab *st = (struct spline_tab *) ctx;
    if (st->n >= st->cap) return 1;
    csn_copy(&st->xs[st->n], x);
    csn_copy(&st->ys[st->n], y);
    st->n++;
    return 0;
}

/* the COUNTING pass (the user's memory report, 2026-11-16): the
 * arrays were allocated at SPLINE_MAX regardless of the point count
 * (a two-point spline cost ~47 KB transient); the count comes first
 * and the allocation is exact */
static int spline_count_visit(void *ctx, const Csn *x, const Csn *y)
{
    struct spline_tab *st = (struct spline_tab *) ctx;
    (void) x; (void) y;
    if (st->n >= SPLINE_MAX) return 1;
    st->n++;
    return 0;
}

/* the INTEG accumulator: the trapezoidal rule over consecutive
 * pairs; x must be STRICTLY increasing in walk order (the user's
 * locked rule) and every pair must be numeric (holes are errors) */
struct integ_acc {
    Csn total;
    Csn px, py;         /* the previous x, y */
    long n;
};

static int integ_visit(void *ctx, const Csn *x, const Csn *y)
{
    struct integ_acc *ia = (struct integ_acc *) ctx;
    Csn dx, sy2, t;
    int err;
    if (ia->n > 0) {
        err = csn_sub(&dx, x, &ia->px);
        if (err) return 1;
        if (csn_is_zero(&dx) || dx.sign) return 1;   /* not increasing */
        err = csn_add(&sy2, &ia->py, y);
        if (err) return 1;
        err = csn_mul(&t, &dx, &sy2);
        if (err) return 1;
        {
            Csn two;
            csn_from_int(&two, 2L);
            err = csn_div(&t, &t, &two);
            if (err) return 1;
        }
        err = csn_add(&ia->total, &ia->total, &t);
        if (err) return 1;
    }
    csn_copy(&ia->px, x);
    csn_copy(&ia->py, y);
    ia->n++;
    return 0;
}

/* ---- ROOT: the expression-based root finder (engineering batch F,
 * 2026-11-15).  ROOT(f, lo, hi) bisects f(X) = 0 on the bracket
 * [lo, hi]: the FIRST argument is captured as raw text (a comma at
 * the top level ends it) and re-evaluated per trial with the bare
 * word X (any case) bound to the trial value - the COLIDX
 * scoped-argument precedent.  X can never collide with a reference
 * (references are letter+digits) or a function name (functions need
 * '(').  The bracket must contain a sign change; the iteration
 * stops at the RELATIVE tolerance scale*1e-24 or 300 passes and
 * returns the interval's midpoint.  CAVEATS (flagged): every trial
 * is a fresh full parse, so side effects in the argument execute
 * once PER TRIAL (the dialog caveat, multiplied); a string result
 * is an error. ---- */
#define ROOT_MAX 240
#define ROOT_PASSES 300

static Csn g_bound_x;           /* the trial value of X */
static int g_bound_on = 0;      /* the binding is active */

/* the top-level position of `stop` in the argument text (quotes and
 * parentheses nest); NULL when the argument never closes */
static const char *root_arg_end(const char *p, char stop)
{
    int depth = 0;
    while (*p) {
        if (*p == '"') {
            p++;
            while (*p && *p != '"') p++;
            if (!*p) return NULL;
        } else if (*p == '(') {
            depth++;
        } else if (*p == ')') {
            if (depth == 0) return NULL;
            depth--;
        } else if (depth == 0 && *p == stop) {
            return p;
        }
        p++;
    }
    return NULL;
}

/* evaluate the captured text with X = xval: 0 = ok, 1 = any failure
 * (an evaluation error, a string result, an interrupt) */
static int root_eval(const char *text, const Csn *xval, Csn *out)
{
    int saved_on = g_bound_on;
    Csn saved_x;
    Csn val;
    char *sf = NULL;
    int e;
    csn_zero(&val);
    csn_copy(&saved_x, &g_bound_x);
    csn_copy(&g_bound_x, xval);
    g_bound_on = 1;
    e = cs_eval_formula(text, &val, &sf);
    g_bound_on = saved_on;
    csn_copy(&g_bound_x, &saved_x);
    if (sf) { free(sf); return 1; }     /* a string result is no root */
    if (e) return 1;
    csn_copy(out, &val);
    return 0;
}

/* ---- ISPRIME (engineering batch G, 2026-11-15): the deterministic
 * Miller-Rabin test with the 12 bases 2..37 - PROVEN exact for every
 * x < 3.3e24 (Sorenson-Webster), so within the 19-digit cap the
 * answer is not probabilistic.  The cap exists because modmul needs
 * the full product a*b to stay inside the 40-digit mantissa: with
 * m <= 1e19 the product reaches 1e38, at 20 digits it overflows.
 * x < 2 (including negatives) -> 0 (primality is a positive-integer
 * property); a fractional or unextractable x -> Error 0 (the EVEN/
 * ODD contract). ---- */
#define ISPRIME_MAXPOW10 "10000000000000000000"    /* 1e19: the cap */

/* t = (a*b) mod m, both a, b < m <= 1e19: the product is < 1e38,
 * inside the mantissa */
static int mr_modmul(Csn *r, const Csn *a, const Csn *b, const Csn *m)
{
    Csn t;
    int err;
    err = csn_mul(&t, a, b);
    if (err) return err;
    return csn_mod(r, &t, m);
}

/* r = a^e mod m, square-and-multiply; the exponent's bits are
 * extracted low-first (e <= 1e19 -> <= 64 bits) */
static int mr_modpow(Csn *r, const Csn *base, const Csn *e, const Csn *m)
{
    Csn bits[64], eb, two, t;
    int nb = 0, i, err;
    Csn acc;
    csn_copy(&eb, e);
    csn_from_int(&two, 2L);
    while (!csn_is_zero(&eb) && nb < 64) {
        if (csn_mod(&t, &eb, &two)) return CSN_ER_DOMAIN;
        csn_copy(&bits[nb++], &t);
        /* subtract the bit first: the remainder is EVEN, so the
         * division by 2 is exact - csn_div is the REAL quotient
         * (1/2 = 0.5), not the integer one */
        if (csn_sub(&eb, &eb, &t)) return CSN_ER_DOMAIN;
        if (csn_div(&eb, &eb, &two)) return CSN_ER_DOMAIN;
    }
    if (!csn_is_zero(&eb)) return CSN_ER_OVERFLOW;   /* > 64 bits */
    csn_from_int(&acc, 1L);
    for (i = nb - 1; i >= 0; i--) {
        err = mr_modmul(&acc, &acc, &acc, m);        /* square */
        if (err) return err;
        if (!csn_is_zero(&bits[i])) {
            err = mr_modmul(&acc, &acc, base, m);    /* multiply */
            if (err) return err;
        }
    }
    csn_copy(r, &acc);
    return CSN_OK;
}

/* the 12 deterministic bases */
static const int mr_bases[12] = { 2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37 };

/* 1 = prime, 0 = composite (deterministic under the cap); the
 * caller has validated the integer and the cap */
static int mr_test(const Csn *x)
{
    Csn one, d, m1, mm, t, base;
    int s, b;
    csn_from_int(&one, 1L);
    csn_copy(&mm, x);
    /* d = x-1, s = its 2-adic valuation */
    csn_sub(&d, &mm, &one);
    csn_copy(&m1, &d);                  /* x-1 */
    s = 0;
    {
        Csn two;
        csn_from_int(&two, 2L);
        for (;;) {
            Csn rem;
            if (csn_mod(&rem, &d, &two)) return 0;
            if (!csn_is_zero(&rem)) break;
            if (csn_div(&d, &d, &two)) return 0;
            s++;
        }
    }
    for (b = 0; b < 12; b++) {
        Csn acc;
        int j, ok = 0;
        csn_from_int(&base, (long) mr_bases[b]);
        if (csn_cmp(x, &base) == 0) return 1;        /* x IS the base */
        if (csn_mod(&t, x, &base)) return 0;
        if (csn_is_zero(&t)) return 0;               /* base | x */
        if (mr_modpow(&acc, &base, &d, &mm)) return 0;
        if (!csn_cmp(&acc, &one) || !csn_cmp(&acc, &m1))
            continue;                                /* this base passes */
        for (j = 0; j < s - 1; j++) {
            if (mr_modmul(&acc, &acc, &acc, &mm)) return 0;
            if (!csn_cmp(&acc, &m1)) { ok = 1; break; }
        }
        if (!ok) return 0;                           /* composite */
    }
    return 1;
}

/* ---- RAND / RANDF / SEED (engineering batch G, 2026-11-15): the
 * BCD linear congruential generator.  state = (state*a + c) mod
 * 10^40 with Hull-Dobell-complete constants (period exactly 10^40):
 * c coprime to 10, a == 1 (mod 20) - a-1 = ...460 is divisible by
 * 2, 5 and 4.  The update splits the state base 10^20 so every
 * product stays inside the 40-digit mantissa EXACTLY:
 *   lo = (s0*a + c) mod 10^20,  hi = (s1*a + carry) mod 10^20,
 * with s = s1*10^20 + s0.  The DRAW uses the HIGH digits (the low
 * digits of a decimal LCG are weak): hi20 = floor(state/10^20),
 * rejection-sampled against the range for perfect uniformity (the
 * rejection probability is below N/10^20 - practically never). ---- */
static Csn g_lcg;               /* the generator state */
static int g_lcg_seeded = 0;

#define LCG_A  "847562105789531461"          /* == 1 (mod 20) */
#define LCG_C  "1234567890123456789"         /* coprime to 10 */
#define LCG_P20 "100000000000000000000"      /* 10^20 */
#define LCG_P40 "10000000000000000000000000000000000000000"  /* 10^40 */

/* one update: state = (state*a + c) mod 10^40, exact */
static void lcg_update(void)
{
    Csn a, c, p20, p40, s1, s0, p0, p1, t, hi, lo, carry;
    csn_parse(LCG_A, &a);
    csn_parse(LCG_C, &c);
    csn_parse(LCG_P20, &p20);
    csn_parse(LCG_P40, &p40);
    /* the split: s1 = the HIGH digits (the floor - scale10 alone
     * would leave the fraction), s0 = state mod 10^20 */
    if (csn_scale10(&s1, &g_lcg, -20)) return;
    if (csn_floor(&s1, &s1)) return;
    if (csn_mod(&s0, &g_lcg, &p20)) return;
    /* the halves */
    if (csn_mul(&p0, &s0, &a)) return;          /* < 1e38 */
    if (csn_mul(&p1, &s1, &a)) return;          /* < 1e36 */
    if (csn_add(&t, &p0, &c)) return;           /* < 1.01e38 */
    if (csn_mod(&lo, &t, &p20)) return;
    if (csn_div(&carry, &t, &p20)) return;      /* exact: < 1.01e18 */
    if (csn_add(&hi, &p1, &carry)) return;      /* < 1.01e36 */
    if (csn_mod(&hi, &hi, &p20)) return;
    /* state = hi*10^20 + lo (< 10^40, no truncation) */
    if (csn_scale10(&hi, &hi, 20)) return;
    if (csn_add(&g_lcg, &hi, &lo)) return;
    if (csn_mod(&g_lcg, &g_lcg, &p40)) return;
}

/* the first-use seed: the wall clock mixed with the process CPU
 * clock - both ISO C89 (time/clock, <time.h>).  time_t's width is
 * implementation-defined, so the value enters through its DECIMAL
 * digit string (sprintf %lu of the unsigned magnitude), never
 * through a fixed-width integer. */
static void lcg_seed(void)
{
    time_t now = time(NULL);
    clock_t ck = clock();
    unsigned long tu = (now < 0) ? (unsigned long) -now
                                 : (unsigned long) now;
    unsigned long ku = (unsigned long) (ck % 1000000L);
    char buf[32];
    Csn p20, p40, tt;
    sprintf(buf, "%lu", (unsigned long) tu);
    if (csn_parse(buf, &g_lcg) != CSN_OK) csn_from_int(&g_lcg, 1L);
    csn_parse(LCG_P20, &p20);
    csn_parse(LCG_P40, &p40);
    if (csn_scale10(&g_lcg, &g_lcg, 6)) return;         /* * 10^6 */
    csn_from_int(&tt, (long) ku);
    if (csn_add(&g_lcg, &g_lcg, &tt)) return;
    if (csn_mod(&g_lcg, &g_lcg, &p40)) return;
    lcg_update();                                       /* one stir */
    g_lcg_seeded = 1;
}

/* the uniform integer draw on [0, n-1] from the HIGH digits, with
 * rejection (n >= 1); 0 = ok */
static int lcg_draw(const Csn *n, Csn *out)
{
    Csn p20, q, lim, hi;
    int tries;
    csn_parse(LCG_P20, &p20);
    for (tries = 0; tries < 100; tries++) {
        lcg_update();
        if (csn_scale10(&hi, &g_lcg, -20)) return 1;
        if (csn_floor(&hi, &hi)) return 1;      /* the high 20 digits */
        if (csn_div(&q, &p20, n)) return 1;             /* floor */
        if (csn_mul(&lim, &q, n)) return 1;             /* <= 1e20 */
        if (csn_cmp(&hi, &lim) < 0) {
            if (csn_mod(out, &hi, n)) return 1;
            return 0;
        }
    }
    /* practically unreachable; the honest fallback */
    if (csn_mod(out, &hi, n)) return 1;
    return 0;
}

/* ---- CONV: the unit conversion table (engineering batch D,
 * 2026-11-15).  Every factor is an EXACT RATIONAL num/den to the
 * dimension's canonical unit; psi and HP are the exact defining
 * rationals (lbf and the foot-pound-second definitions), not the
 * rounded 16-digit standards.  Names are 7-bit ASCII (the string
 * model's alphabet): mm2/cm2/m3, ohm/kohm, uF, km/h.  The bare "f"
 * is FAHRENHEIT - the farad is spelled "farad"/"uf" (the one name
 * clash in the table, flagged).  Lookup is case-insensitive. ---- */

/* dimension ids; TEMP is the special non-multiplicative one */
#define UDD_LEN   0     /* canonical m   */
#define UDD_AREA  1     /* m2            */
#define UDD_VOL   2     /* l             */
#define UDD_MASS  3     /* kg            */
#define UDD_PRES  4     /* Pa            */
#define UDD_TEMP  5     /* C (offsets)   */
#define UDD_ENER  6     /* J             */
#define UDD_POW   7     /* W             */
#define UDD_VOLT  8     /* V             */
#define UDD_CURR  9     /* A             */
#define UDD_OHM  10     /* ohm           */
#define UDD_FARD 11     /* farad         */
#define UDD_INDU 12     /* H             */
#define UDD_SPD  13     /* m/s           */
#define UDD_FREQ 14     /* Hz            */

struct conv_unit {
    const char *name;
    int dim;
    const char *num;    /* canonical = value * num/den */
    const char *den;
};

static const struct conv_unit conv_units[] = {
    /* length */
    {"mm",   UDD_LEN,  "1", "1000"},
    {"cm",   UDD_LEN,  "1", "100"},
    {"m",    UDD_LEN,  "1", "1"},
    {"km",   UDD_LEN,  "1000", "1"},
    {"inch", UDD_LEN,  "254", "10000"},
    {"ft",   UDD_LEN,  "3048", "10000"},
    {"mile", UDD_LEN,  "1609344", "1000"},
    /* area */
    {"mm2",  UDD_AREA, "1", "1000000"},
    {"cm2",  UDD_AREA, "1", "10000"},
    {"dm2",  UDD_AREA, "1", "100"},
    {"m2",   UDD_AREA, "1", "1"},
    {"hectare", UDD_AREA, "10000", "1"},
    /* volume (US gallon = 231 in^3); the SI cubic prefixes are the
     * DEFINED liter equivalences (1 cm3 = 1 ml, 1 dm3 = 1 l) */
    {"mm3",  UDD_VOL,  "1", "1000000"},
    {"cm3",  UDD_VOL,  "1", "1000"},
    {"dm3",  UDD_VOL,  "1", "1"},
    {"ml",   UDD_VOL,  "1", "1000"},
    {"l",    UDD_VOL,  "1", "1"},
    {"m3",   UDD_VOL,  "1000", "1"},
    {"km3",  UDD_VOL,  "1000000000000", "1"},
    {"gallon", UDD_VOL, "3785411784", "1000000000"},
    /* mass */
    {"mg",   UDD_MASS, "1", "1000000"},
    {"g",    UDD_MASS, "1", "1000"},
    {"kg",   UDD_MASS, "1", "1"},
    {"tonne", UDD_MASS, "1000", "1"},
    {"lb",   UDD_MASS, "45359237", "100000000"},
    /* pressure (psi = lbf/in^2, the exact rational) */
    {"pa",   UDD_PRES, "1", "1"},
    {"kpa",  UDD_PRES, "1000", "1"},
    {"mpa",  UDD_PRES, "1000000", "1"},
    {"bar",  UDD_PRES, "100000", "1"},
    {"mbar", UDD_PRES, "100", "1"},
    {"psi",  UDD_PRES, "44482216152605", "6451600000"},
    /* temperature (special) */
    {"c",    UDD_TEMP, "0", "0"},
    {"f",    UDD_TEMP, "0", "0"},
    {"k",    UDD_TEMP, "0", "0"},
    /* energy (cal = 4.184 J, ISO) */
    {"j",    UDD_ENER, "1", "1"},
    {"kj",   UDD_ENER, "1000", "1"},
    {"wh",   UDD_ENER, "3600", "1"},
    {"kwh",  UDD_ENER, "3600000", "1"},
    {"cal",  UDD_ENER, "4184", "1000"},
    /* power (HP = 550 ft*lbf/s, the exact rational) */
    {"w",    UDD_POW,  "1", "1"},
    {"kw",   UDD_POW,  "1000", "1"},
    {"hp",   UDD_POW,  "745699871582270220", "1000000000000000"},
    /* electrical: each quantity its own dimension */
    {"v",    UDD_VOLT, "1", "1"},
    {"mv",   UDD_VOLT, "1", "1000"},
    {"a",    UDD_CURR, "1", "1"},
    {"ma",   UDD_CURR, "1", "1000"},
    {"ohm",  UDD_OHM,  "1", "1"},
    {"kohm", UDD_OHM,  "1000", "1"},
    {"farad", UDD_FARD, "1", "1"},
    {"uf",   UDD_FARD, "1", "1000000"},
    {"h",    UDD_INDU, "1", "1"},
    /* speed */
    {"m/s",  UDD_SPD,  "1", "1"},
    {"km/h", UDD_SPD,  "5", "18"},
    {"mph",  UDD_SPD,  "44704", "100000"},
    /* frequency */
    {"hz",   UDD_FREQ, "1", "1"},
    {"khz",  UDD_FREQ, "1000", "1"},
    {"mhz",  UDD_FREQ, "1000000", "1"},
    {"ghz",  UDD_FREQ, "1000000000", "1"}
};

/* case-insensitive 7-bit compare for the unit names */
static int conv_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char) *a) != tolower((unsigned char) *b))
            return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* find a unit: 1 = ok (dim out, num/den parsed), 0 = unknown */
static int conv_lookup(const char *name, int *dimp, Csn *num, Csn *den)
{
    size_t i;
    for (i = 0; i < sizeof(conv_units) / sizeof(conv_units[0]); i++) {
        if (conv_eq(conv_units[i].name, name)) {
            *dimp = conv_units[i].dim;
            if (csn_parse(conv_units[i].num, num) != CSN_OK)
                return 0;
            if (csn_parse(conv_units[i].den, den) != CSN_OK)
                return 0;
            return 1;
        }
    }
    return 0;
}

/* a unit ARGUMENT: an expression that derefs to a String
 * (a literal or a text cell); 1 = ok, 0 = wrong type (Error 6) */
static int conv_unit_arg(P *ps, char *out, int outn)
{
    Val v;
    int e;
    memset(&v, 0, sizeof(v));
    csn_zero(&v.num);
    v = parse_condexpr(ps);
    if (!v.good) { val_free(&v); return -1; }
    e = val_deref(&v);
    if (e >= 0) { val_free(&v); return -1; }
    if (v.type != VT_STR || !v.str) { val_free(&v); return 0; }
    strncpy(out, v.str, (size_t) outn - 1);
    out[outn - 1] = '\0';
    val_free(&v);
    return 1;
}

/* the streaming aggregation for SUM/CNT/AVG/MAX/MIN (2026-11-14): no
 * value array at all - running aggregates only.  The function id
 * selects the mode; the caller owns the error reporting. */
typedef struct {
    Csn sum;        /* SUM/AVG */
    Csn ext;        /* MAX/MIN: the extreme seen so far */
    Csn sumsq;      /* SUMSQ/RMS: the running sum of squares */
    Csn mean;       /* VAR family: the running mean (Welford) */
    Csn m2;         /* VAR family: the sum of squared deviations */
    int cnt;
    int started;    /* an extreme was stored */
} CsAgg;

/* -1 = ok, else the error code (0 = the value-range class) */
static int agg_item(CsAgg *ag, int f, const Csn *num)
{
    int err;
    Csn t, nn, delta, adj, d2;
    switch (f) {
    case FN_SUM:
    case FN_AVG:
        err = csn_add(&ag->sum, &ag->sum, num);
        if (err) return 0;
        break;
    case FN_MAX:
        if (!ag->started || csn_cmp(num, &ag->ext) > 0)
            csn_copy(&ag->ext, num);
        break;
    case FN_MIN:
        if (!ag->started || csn_cmp(num, &ag->ext) < 0)
            csn_copy(&ag->ext, num);
        break;
    case FN_FAND:
        /* the fuzzy conjunction: MIN with the [0,1] domain check
         * (the threshold is NEVER applied here, 2026-11-15) */
        if (csn_cmp(num, &cs_fuzzy_zero) < 0 ||
            csn_cmp(num, &cs_fuzzy_one) > 0) return 0;
        if (!ag->started || csn_cmp(num, &ag->ext) < 0)
            csn_copy(&ag->ext, num);
        break;
    case FN_FOR:
        /* the fuzzy disjunction: MAX with the [0,1] domain check */
        if (csn_cmp(num, &cs_fuzzy_zero) < 0 ||
            csn_cmp(num, &cs_fuzzy_one) > 0) return 0;
        if (!ag->started || csn_cmp(num, &ag->ext) > 0)
            csn_copy(&ag->ext, num);
        break;
    case FN_SUMSQ:
    case FN_RMS:
        /* the running sum of squares (engineering batch B) */
        err = csn_mul(&t, num, num);
        if (err) return 0;
        err = csn_add(&ag->sumsq, &ag->sumsq, &t);
        if (err) return 0;
        break;
    case FN_SUMABS:
    case FN_SUMPOS:
    case FN_SUMNEG:
    case FN_MAXABS:
    case FN_MINABS:
        /* the abs family (user spec 2026-11-17): the |x| transform is
         * a sign-bit clear on a local copy - one accumulator field
         * per mode, no arrays */
        csn_copy(&t, num);
        t.sign = 0;                      /* |x| */
        if (f == FN_SUMPOS && csn_cmp(num, &cs_fuzzy_zero) <= 0)
            break;                       /* zeros add nothing */
        if (f == FN_SUMNEG && csn_cmp(num, &cs_fuzzy_zero) >= 0)
            break;                       /* the magnitudes only */
        if (f == FN_SUMABS || f == FN_SUMPOS || f == FN_SUMNEG) {
            err = csn_add(&ag->sum, &ag->sum, &t);
            if (err) return 0;
        } else if (f == FN_MAXABS) {
            if (!ag->started || csn_cmp(&t, &ag->ext) > 0)
                csn_copy(&ag->ext, &t);
        } else {                         /* FN_MINABS */
            if (!ag->started || csn_cmp(&t, &ag->ext) < 0)
                csn_copy(&ag->ext, &t);
        }
        break;
    case FN_VAR:
    case FN_VARP:
    case FN_STDEV:
    case FN_STDEVP:
        /* Welford's single pass: mean += (x-mean)/n;
         * M2 += (x-mean_old)*(x-mean_new) - numerically stable and
         * streaming (no buffer, no second walk).  An intermediate
         * overflow/underflow is the family's Error 0. */
        csn_from_int(&nn, (long) ag->cnt + 1);
        err = csn_sub(&delta, num, &ag->mean);
        if (err) return 0;
        err = csn_div(&adj, &delta, &nn);
        if (err) return 0;
        err = csn_add(&ag->mean, &ag->mean, &adj);
        if (err) return 0;
        err = csn_sub(&adj, num, &ag->mean);
        if (err) return 0;
        err = csn_mul(&d2, &delta, &adj);
        if (err) return 0;
        err = csn_add(&ag->m2, &ag->m2, &d2);
        if (err) return 0;
        break;
    default:
        break;                      /* CNT counts only */
    }
    ag->cnt++;
    ag->started = 1;
    return -1;
}

/* stream the argument list: Numbers (also dereferenced References) and
 * Bool values aggregate, Strings are rejected (Error 6 - the strict
 * mixing rule), a Range walks its numeric cells (an error cell in the
 * range is Error 0).  -1 = ok. */
static int parse_stream(P *ps, int f, CsAgg *ag)
{
    if (f == FN_FAND || f == FN_FOR) cs_fuzzy_consts();
    for (;;) {
        Val v;
        int e;
        memset(&v, 0, sizeof(v));
        csn_zero(&v.num);
        v = parse_condexpr(ps);
        if (!v.good) { e = v.err; val_free(&v); return e ? e : 0; }
        if (v.type == VT_RANGE) {
            int c, r;
            for (r = v.rng.start.row; r <= v.rng.end.row; r++)
                for (c = v.rng.start.col; c <= v.rng.end.col; c++) {
                    Cell *cl = cs_getcell(c, r);
                    if (!cl) continue;
                    /* a string-result formula cell is a TEXT value in
                     * the range: skipped like text (2026-11-13) */
                    if (cl->type == CE_NUM && !cl->iserr && !cl->strres) {
                        e = agg_item(ag, f, &cl->val);
                        if (e >= 0) { val_free(&v); return e; }
                    } else if (cl->iserr && !cl->strres) {
                        val_free(&v); return 0;
                    }
                }
        } else {
            e = val_deref(&v);
            if (e >= 0) { val_free(&v); return e; }
            /* the STRICT mixing rule (user-refined 2026-11-13): a
             * directly-entered String argument in a numeric list is an
             * error (ranges SKIP their non-numerics instead - the
             * documented MED/SUM semantics) */
            if (v.type == VT_STR) { val_free(&v); return 6; }
            e = agg_item(ag, f, &v.num);
            if (e >= 0) { val_free(&v); return e; }
        }
        val_free(&v);
        skip_ws(&ps->p);
        if (*ps->p == ',') { ps->p++; continue; }
        break;
    }
    return -1;
}

/* MED's collector: a bounded heap buffer (MED_MAX values).  A range
 * beyond the bound is an ERROR (Error 0) - never a silently truncated
 * median (user spec 2026-11-14). */
#define MED_MAX 512
/* collect ONE argument (a range's numerics, or a scalar) into the
 * bounded buffer: -1 = ok, else the error code.  The buffer grows to
 * MED_MAX; beyond the bound is Error 0 (a silently truncated result
 * would be a lie).  Shared by MED, PCT and QUART. */
static int med_collect_arg(P *ps, Csn **bufp, int *capp, int *cnt)
{
    int n = *cnt;
    Val v;
    int e;
    memset(&v, 0, sizeof(v));
    csn_zero(&v.num);
    v = parse_condexpr(ps);
    if (!v.good) { e = v.err; val_free(&v); return e ? e : 0; }
    if (v.type == VT_RANGE) {
        int c, r;
        for (r = v.rng.start.row; r <= v.rng.end.row; r++)
            for (c = v.rng.start.col; c <= v.rng.end.col; c++) {
                Cell *cl = cs_getcell(c, r);
                if (!cl) continue;
                if (cl->type == CE_NUM && !cl->iserr && !cl->strres) {
                    Csn *ne;
                    if (n == *capp) {
                        *capp *= 2;
                        if (*capp > MED_MAX) *capp = MED_MAX;
                        if (n >= *capp) { val_free(&v); return 0; }
                        ne = (Csn *) realloc(*bufp,
                                (size_t) *capp * sizeof(Csn));
                        if (!ne) { val_free(&v); return 0; }
                        *bufp = ne;
                    }
                    csn_copy(&(*bufp)[n++], &cl->val);
                } else if (cl->iserr && !cl->strres) {
                    val_free(&v); return 0;
                }
            }
    } else {
        e = val_deref(&v);
        if (e >= 0) { val_free(&v); return e; }
        if (v.type == VT_STR) { val_free(&v); return 6; }
        {
            Csn *ne;
            if (n == *capp) {
                *capp *= 2;
                if (*capp > MED_MAX) *capp = MED_MAX;
                if (n >= *capp) { val_free(&v); return 0; }
                ne = (Csn *) realloc(*bufp, (size_t) *capp * sizeof(Csn));
                if (!ne) { val_free(&v); return 0; }
                *bufp = ne;
            }
            csn_copy(&(*bufp)[n++], &v.num);
        }
    }
    val_free(&v);
    *cnt = n;
    return -1;
}

/* the argument LIST (MED): every argument collected in order */
static int parse_med_collect(P *ps, Csn **bufp, int *capp, int *cnt)
{
    for (;;) {
        int e = med_collect_arg(ps, bufp, capp, cnt);
        if (e >= 0) return e;
        skip_ws(&ps->p);
        if (*ps->p == ',') { ps->p++; continue; }
        break;
    }
    return -1;
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
            /* a digit TAIL sealed by '(' is part of the name (ATAN2,
             * 2026-11-15).  Safe: is_coord_ahead ran first, so a word
             * that letters+digits would make a coordinate never gets
             * here - a function name may never LOOK like a
             * coordinate. */
            if (*q >= '0' && *q <= '9') {
                const char *r = q;
                while (*r >= '0' && *r <= '9') r++;
                if (*r == '(') {
                    while (q < r) {
                        if (n < 14) name[n++] = (char) *q;
                        q++;
                    }
                }
            }
            name[n] = '\0';
            skip_ws(&q);
            if (*q != '(') {
                /* the ROOT bound variable: a bare X (any case) inside
                 * a captured ROOT argument - the COLIDX
                 * scoped-argument precedent.  Safe: is_coord_ahead
                 * ran first (X2 is a reference) and a function call
                 * would have its '('. */
                if (g_bound_on && n == 1 && name[0] == 'X') {
                    val_free(&v);
                    memset(&v, 0, sizeof(v));
                    csn_copy(&v.num, &g_bound_x);
                    v.good = 1;
                    ps->p = q;
                    return v;
                }
                val_err(&v, 6);
                ps->p = q;
                return v;
            }
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
            if (f == FN_E) {
                if (*ps->p == ')') ps->p++;
                else { val_err(&v, 6); return v; }
                val_free(&v);
                csn_e(&v.num); v.good = 1;
                return v;
            }
            if (f == FN_ROW || f == FN_COL) {
                /* the row/column, 1-indexed: IN A STORED FORMULA the
                 * cell's own position (the evaluation context set by
                 * the recalculation, the commit and the copy);
                 * asked context-free (the ?-calculator, the dialogs)
                 * the current cursor answers (2026-11-14) */
                if (*ps->p == ')') ps->p++;
                else { val_err(&v, 6); return v; }
                val_free(&v);
                if (cs_eval_ctx_on)
                    csn_from_int(&v.num, f == FN_ROW ?
                                 (long) (cs_eval_ctx_row + 1) :
                                 (long) (cs_eval_ctx_col + 1));
                else
                    csn_from_int(&v.num, f == FN_ROW ?
                                 (long) (curRow + 1) :
                                 (long) (curCol + 1));
                v.good = 1;
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
                /* VAL(cell): the TYPED value of a single referenced
                 * cell (user-refined 2026-11-13): a numeric cell -> a
                 * Number, a TEXT cell -> its content as a String, an
                 * empty/allocated/error cell -> Error 1 */
                Val a = parse_condexpr(ps);
                Cell *cl;
                if (!a.good) return a;
                if (a.type != VT_REF) { val_free(&a); val_err(&v, 6); return v; }
                skip_ws(&ps->p);
                if (*ps->p != ')') { val_err(&v, 3); val_free(&a); return v; }
                ps->p++;
                cl = cs_getcell(a.ref.col, a.ref.row);
                val_free(&a);
                val_free(&v);
                memset(&v, 0, sizeof(v));
                csn_zero(&v.num);
                v.type = VT_NUM;
                /* the derived type first: a string result is the value */
                if (cl && cl->strres && !cl->iserr) {
                    v.str = cs_strdup(cl->strres);
                    if (!v.str) { val_err(&v, 0); return v; }
                    v.type = VT_STR;
                    v.good = 1;
                    return v;
                }
                if (cl && cl->type == CE_NUM && !cl->iserr) {
                    csn_copy(&v.num, &cl->val);
                    v.good = 1;
                    return v;
                }
                if (cl && cl->type == CE_TEXT && cl->cont && !cl->iserr) {
                    v.str = cs_strdup(cl->cont);
                    if (!v.str) { val_err(&v, 0); return v; }
                    v.type = VT_STR;
                    v.good = 1;
                    return v;
                }
                val_err(&v, 1);     /* empty/allocated/error cell */
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
            if (f == FN_INVFACT) {
                /* INVFACT(y): the inverse factorial - exact integers
                 * walk, the rest solves ln Gamma(x+1) = ln y with
                 * Newton (the full BCD precision) */
                Val a = parse_numarg(ps);
                int err;
                if (!a.good) return a;
                skip_ws(&ps->p);
                if (*ps->p != ')') { val_err(&v, 3); val_free(&a); return v; }
                ps->p++;
                val_free(&v);
                err = csn_invfact(&v.num, &a.num);
                val_free(&a);
                if (err) { val_err(&v, 0); return v; }
                v.good = 1;
                return v;
            }
            if (f == FN_FNOT || f == FN_FAND || f == FN_FOR || f == FN_FXOR)
                cs_fuzzy_consts();
            if (f == FN_EVEN || f == FN_ODD) {
                /* EVEN(x)/ODD(x): 1 if the integer x is even/odd,
                 * 0 otherwise (user request 2026-11-15).  A
                 * fractional argument is Error 0 (the CHR contract).
                 * The PARITY is the parity of the LAST DIGIT - no
                 * native conversion at all, exact for any integer
                 * magnitude (the portability audit: the old path
                 * converted through long, wrong on 32-bit hosts) */
                Val a = parse_numarg(ps);
                Csn t;
                int last;
                if (!a.good) return a;
                skip_ws(&ps->p);
                if (*ps->p != ')') { val_err(&v, 3); val_free(&a); return v; }
                ps->p++;
                if (!csn_integer_part(&a.num, &t)) {
                    val_free(&a); val_err(&v, 0); return v;
                }
                last = (t.exp10 > t.ndig)
                           ? 0      /* the value ends in mantissa
                                     * zeros (1E+500: even!) */
                           : ((t.ndig > 0) ? t.nd[t.ndig - 1] : 0);
                val_free(&a);
                csn_from_int(&v.num,
                             f == FN_EVEN ? (long) (last % 2 == 0)
                                          : (long) (last % 2 != 0));
                v.good = 1;
                return v;
            }
            if (f == FN_ISPRIME) {
                /* ISPRIME(x): 1 for a prime, 0 otherwise (engineering
                 * batch G, 2026-11-15).  A fractional or oversized
                 * argument is Error 0 (the EVEN/ODD contract); more
                 * than 19 digits is Error 0 (the modmul cap); x < 2
                 * (including negatives) is 0. */
                Val a = parse_numarg(ps);
                Csn t, cap;
                if (!a.good) return a;
                skip_ws(&ps->p);
                if (*ps->p != ')') { val_err(&v, 3); val_free(&a); return v; }
                ps->p++;
                val_free(&v);
                csn_zero(&v.num);
                if (!csn_integer_part(&a.num, &t) ||
                    csn_cmp(&t, &a.num) != 0) {
                    val_free(&a); val_err(&v, 0); return v;
                }
                csn_parse(ISPRIME_MAXPOW10, &cap);
                if (csn_cmp(&t, &cap) >= 0) {
                    val_free(&a); val_err(&v, 0); return v;
                }
                val_free(&a);
                {
                    Csn two, tt;
                    /* the checks stay in BCD: a 19-digit value can
                     * exceed LONG_MAX (the csn_to_long_val round trip
                     * overflows) - mr_test consumes the Csn directly */
                    csn_from_int(&two, 2L);
                    if (csn_cmp(&t, &two) < 0) { v.good = 1; return v; }
                    if (csn_cmp(&t, &two) == 0) {
                        csn_from_int(&v.num, 1L);
                        v.good = 1;
                        return v;
                    }
                    if (!csn_mod(&tt, &t, &two) && csn_is_zero(&tt)) {
                        v.good = 1;                   /* even */
                        return v;
                    }
                }
                csn_from_int(&v.num, (long) mr_test(&t));
                v.good = 1;
                return v;
            }
            if (f == FN_CEIL || f == FN_FLOOR || f == FN_DB ||
                f == FN_DBM || f == FN_DBW || f == FN_LIN ||
                f == FN_STEP) {
                /* the one-argument engineering family (batch A,
                 * 2026-11-15).  DB/DBM/DBW require a POSITIVE argument
                 * (log of nonpositive is a domain); LIN is 10^(dB/20)
                 * and accepts any dB; STEP(0) = 1 (Heaviside at the
                 * origin - flagged convention). */
                Val a = parse_numarg(ps);
                Csn t, k, zero;
                int err = 0;
                if (!a.good) return a;
                skip_ws(&ps->p);
                if (*ps->p != ')') { val_err(&v, 3); val_free(&a); return v; }
                ps->p++;
                val_free(&v);
                csn_zero(&v.num);
                csn_zero(&zero);
                switch (f) {
                case FN_CEIL:
                    err = csn_ceil(&v.num, &a.num);
                    break;
                case FN_FLOOR:
                    err = csn_floor(&v.num, &a.num);
                    break;
                case FN_STEP:
                    csn_from_int(&v.num,
                                 csn_cmp(&a.num, &zero) < 0 ? 0L : 1L);
                    break;
                case FN_DB:
                case FN_DBM:
                case FN_DBW:
                    if (csn_cmp(&a.num, &zero) <= 0) { err = 1; break; }
                    err = csn_log10(&t, &a.num);
                    if (!err) {
                        csn_from_int(&k, f == FN_DB ? 20L : 10L);
                        err = csn_mul(&v.num, &k, &t);
                        if (!err && f == FN_DBM) {
                            csn_from_int(&k, 30L);
                            err = csn_add(&v.num, &v.num, &k);
                        }
                    }
                    break;
                case FN_LIN:
                    csn_from_int(&k, 20L);
                    err = csn_div(&t, &a.num, &k);
                    if (!err) {
                        csn_from_int(&k, 10L);
                        err = csn_pow(&v.num, &k, &t);
                    }
                    break;
                }
                val_free(&a);
                if (err) { val_err(&v, 0); return v; }
                v.good = 1;
                return v;
            }
            if (f == FN_CLAMP || f == FN_LERP || f == FN_UNLERP) {
                /* the three-argument engineering family (batch A):
                 * CLAMP(x,lo,hi) with lo > hi a domain error;
                 * LERP(a,b,t) = a + (b-a)*t (t outside [0,1]
                 * extrapolates - flagged); UNLERP(a,b,x) = (x-a)/(b-a)
                 * (b = a is a domain error). */
                Val a, b, c;
                Csn t, t2;
                int err;
                a = parse_numarg(ps);
                if (!a.good) return a;
                skip_ws(&ps->p);
                if (*ps->p != ',') { val_err(&v, 3); val_free(&a); return v; }
                ps->p++;
                b = parse_numarg(ps);
                if (!b.good) { val_free(&a); return b; }
                skip_ws(&ps->p);
                if (*ps->p != ',') {
                    val_err(&v, 3); val_free(&a); val_free(&b); return v;
                }
                ps->p++;
                c = parse_numarg(ps);
                if (!c.good) { val_free(&a); val_free(&b); return c; }
                skip_ws(&ps->p);
                if (*ps->p != ')') {
                    val_err(&v, 3);
                    val_free(&a); val_free(&b); val_free(&c);
                    return v;
                }
                ps->p++;
                val_free(&v);
                csn_zero(&v.num);
                err = 0;
                switch (f) {
                case FN_CLAMP:
                    if (csn_cmp(&b.num, &c.num) > 0) { err = 1; break; }
                    if (csn_cmp(&a.num, &b.num) < 0)
                        csn_copy(&v.num, &b.num);
                    else if (csn_cmp(&a.num, &c.num) > 0)
                        csn_copy(&v.num, &c.num);
                    else
                        csn_copy(&v.num, &a.num);
                    break;
                case FN_LERP:
                    err = csn_sub(&t, &b.num, &a.num);
                    if (!err) err = csn_mul(&t, &t, &c.num);
                    if (!err) err = csn_add(&v.num, &a.num, &t);
                    break;
                case FN_UNLERP:
                    err = csn_sub(&t, &b.num, &a.num);
                    if (!err) err = csn_sub(&t2, &c.num, &a.num);
                    if (!err) err = csn_div(&v.num, &t2, &t);
                    break;
                }
                val_free(&a); val_free(&b); val_free(&c);
                if (err) { val_err(&v, 0); return v; }
                v.good = 1;
                return v;
            }
            if (f == FN_DIST) {
                /* DIST(x1,y1,x2,y2): the point distance through the
                 * scaled hypot (batch A, 2026-11-15) */
                Val a, b, c, d;
                Csn dx, dy;
                int err;
                a = parse_numarg(ps);
                if (!a.good) return a;
                skip_ws(&ps->p);
                if (*ps->p != ',') { val_err(&v, 3); val_free(&a); return v; }
                ps->p++;
                b = parse_numarg(ps);
                if (!b.good) { val_free(&a); return b; }
                skip_ws(&ps->p);
                if (*ps->p != ',') {
                    val_err(&v, 3); val_free(&a); val_free(&b); return v;
                }
                ps->p++;
                c = parse_numarg(ps);
                if (!c.good) { val_free(&a); val_free(&b); return c; }
                skip_ws(&ps->p);
                if (*ps->p != ',') {
                    val_err(&v, 3);
                    val_free(&a); val_free(&b); val_free(&c); return v;
                }
                ps->p++;
                d = parse_numarg(ps);
                if (!d.good) {
                    val_free(&a); val_free(&b); val_free(&c); return d;
                }
                skip_ws(&ps->p);
                if (*ps->p != ')') {
                    val_err(&v, 3);
                    val_free(&a); val_free(&b); val_free(&c); val_free(&d);
                    return v;
                }
                ps->p++;
                val_free(&v);
                csn_zero(&v.num);
                err = csn_sub(&dx, &c.num, &a.num);       /* x2-x1 */
                if (!err) err = csn_sub(&dy, &d.num, &b.num);  /* y2-y1 */
                if (!err) err = csn_hypot(&v.num, &dx, &dy);
                val_free(&a); val_free(&b); val_free(&c); val_free(&d);
                if (err) { val_err(&v, 0); return v; }
                v.good = 1;
                return v;
            }
            if (f == FN_FNOT) {
                /* NOT(x): the Zadeh negation 1-x (fuzzy Boolean
                 * algebra, user request 2026-11-15).  The operand
                 * must be in [0,1]; outside is Error 0. */
                Val a = parse_numarg(ps);
                Csn one;
                int err;
                if (!a.good) return a;
                skip_ws(&ps->p);
                if (*ps->p != ')') { val_err(&v, 3); val_free(&a); return v; }
                ps->p++;
                if (csn_cmp(&a.num, &cs_fuzzy_zero) < 0 ||
                    csn_cmp(&a.num, &cs_fuzzy_one) > 0) {
                    val_free(&a); val_err(&v, 0); return v;
                }
                csn_from_int(&one, 1);
                err = csn_sub(&v.num, &one, &a.num);
                val_free(&a);
                if (err) { val_err(&v, 0); return v; }
                v.good = 1;
                return v;
            }
            if (f == FN_FXOR) {
                /* XOR(x,y): the Zadeh exclusive-or
                 * MAX(MIN(x,1-y),MIN(1-x,y)) - exactly two arguments
                 * (2026-11-15) */
                Val a = parse_numarg(ps);
                Val b;
                Csn one, na, nb, m1, m2;
                int err;
                if (!a.good) return a;
                skip_ws(&ps->p);
                if (*ps->p != ',') { val_err(&v, 3); val_free(&a); return v; }
                ps->p++;
                b = parse_numarg(ps);
                if (!b.good) { val_free(&a); return b; }
                skip_ws(&ps->p);
                if (*ps->p != ')') { val_err(&v, 3); val_free(&a); val_free(&b); return v; }
                ps->p++;
                if (csn_cmp(&a.num, &cs_fuzzy_zero) < 0 ||
                    csn_cmp(&a.num, &cs_fuzzy_one) > 0 ||
                    csn_cmp(&b.num, &cs_fuzzy_zero) < 0 ||
                    csn_cmp(&b.num, &cs_fuzzy_one) > 0) {
                    val_free(&a); val_free(&b); val_err(&v, 0); return v;
                }
                csn_from_int(&one, 1);
                err = csn_sub(&na, &one, &a.num);
                if (!err) err = csn_sub(&nb, &one, &b.num);
                /* m1 = MIN(x, 1-y), m2 = MIN(1-x, y) (the exact BCD min) */
                if (!err) {
                    if (csn_cmp(&a.num, &nb) < 0) csn_copy(&m1, &a.num);
                    else csn_copy(&m1, &nb);
                    if (csn_cmp(&na, &b.num) < 0) csn_copy(&m2, &na);
                    else csn_copy(&m2, &b.num);
                    if (csn_cmp(&m1, &m2) > 0) csn_copy(&v.num, &m1);
                    else csn_copy(&v.num, &m2);
                }
                val_free(&a); val_free(&b);
                if (err) { val_err(&v, 0); return v; }
                v.good = 1;
                return v;
            }
            if (f == FN_CHR) {
                /* CHR(n): the character with the 7-bit printable code
                 * n (32..127); everything else is Error 0 (user-ext
                 * 2026-11-13) */
                Val a = parse_numarg(ps);
                int cc;
                Csn t;
                if (!a.good) return a;
                skip_ws(&ps->p);
                if (*ps->p != ')') { val_err(&v, 3); val_free(&a); return v; }
                ps->p++;
                if (!csn_integer_part(&a.num, &t)) {
                    val_free(&a); val_err(&v, 0); return v;
                }
                cc = (int) csn_to_long_val(&t);
                val_free(&a);
                if (cc < 32 || cc > 127) { val_err(&v, 0); return v; }
                val_free(&v);
                memset(&v, 0, sizeof(v));
                csn_zero(&v.num);
                v.str = (char *) malloc(2);
                if (!v.str) { val_err(&v, 0); return v; }
                v.str[0] = (char) cc;
                v.str[1] = '\0';
                v.type = VT_STR;
                v.good = 1;
                return v;
            }
            if (f == FN_ORD) {
                /* ORD(s): the 7-bit code of a STRING OF LENGTH 1 with
                 * a printable byte (32..127); a length other than 1,
                 * an out-of-range byte, a Number argument, or an empty
                 * string is an error (user-ext 2026-11-13) */
                Val a = parse_condexpr(ps);
                int blen;
                unsigned char c0;
                if (!a.good) return a;
                skip_ws(&ps->p);
                if (*ps->p != ')') { val_err(&v, 3); val_free(&a); return v; }
                ps->p++;
                if (a.type == VT_REF) {
                    int de = val_deref(&a);
                    if (de >= 0) { val_free(&a); val_err(&v, de); return v; }
                }
                if (a.type != VT_STR || !a.str) {
                    val_free(&a); val_err(&v, 6); return v;
                }
                blen = (int) strlen(a.str);
                c0 = (unsigned char) a.str[0];
                val_free(&a);
                val_free(&v);
                memset(&v, 0, sizeof(v));
                csn_zero(&v.num);
                v.type = VT_NUM;
                if (blen != 1 || c0 < 32 || c0 > 127) {
                    val_err(&v, 0);
                    return v;
                }
                csn_from_int(&v.num, (long) c0);
                v.good = 1;
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
                f == FN_NCR || f == FN_NPR ||
                f == FN_HYPOT || f == FN_ATAN2) {
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
                case FN_HYPOT:
                    err = csn_hypot(&v.num, &a.num, &a2.num);
                    break;
                case FN_ATAN2:
                    err = eng_atan2(&v.num, &a.num, &a2.num);
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
                /* the attempt RESETS the regression state (the user's
                 * report, 2026-11-16): a failed REGR must not leave
                 * the previous result available to SLOPE()/PROJ()/
                 * DEPD() - the state rebuilds from the sums on
                 * success */
                cs_reg_reset();
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
                    Csn sx, sy, mx, my, sxx, sxy, ncs;
                    int i, n = 0, err;
                    csn_zero(&sx); csn_zero(&sy);
                    csn_zero(&sxx); csn_zero(&sxy);
                    csn_zero(&ncs);
                    for (i = 0; i < alen; i++) {
                        int cc = vertical ? c1 : c1 + i;
                        int rr = vertical ? r1 + i : r1;
                        int dc = vertical ? c3 : c3 + i;
                        int dr = vertical ? r3 + i : r3;
                        Cell *x = cs_getcell(cc, rr);
                        Cell *y = cs_getcell(dc, dr);
                        if (!x || !y || x->type != CE_NUM || x->iserr ||
                            y->type != CE_NUM || y->iserr) { val_err(&v, 9); return v; }
                        /* EVERY observation feeds the sums - the old
                         * REGR_MAX 512-cap truncated the regression
                         * silently (the user's report, 2026-11-16) */
                        n++;
                        err = csn_add(&sx, &sx, &x->val); if (err) { val_err(&v, 255); return v; }
                        err = csn_add(&sy, &sy, &y->val); if (err) { val_err(&v, 255); return v; }
                    }
                    if (n == 0) { val_err(&v, 255); return v; }
                    csn_from_int(&ncs, n);
                    if (csn_div(&mx, &sx, &ncs) ||
                        csn_div(&my, &sy, &ncs)) { val_err(&v, 255); return v; }
                    /* pass 2: the centered deviations */
                    csn_zero(&sxx); csn_zero(&sxy);
                    for (i = 0; i < alen; i++) {
                        int cc = vertical ? c1 : c1 + i;
                        int rr = vertical ? r1 + i : r1;
                        int dc = vertical ? c3 : c3 + i;
                        int dr = vertical ? r3 + i : r3;
                        Cell *x = cs_getcell(cc, rr);
                        Cell *y = cs_getcell(dc, dr);
                        Csn dx, dy, px;
                        if (!x || !y || x->type != CE_NUM || x->iserr ||
                            y->type != CE_NUM || y->iserr) { val_err(&v, 9); return v; }
                        err = csn_sub(&dx, &x->val, &mx);
                        if (err) { val_err(&v, 255); return v; }
                        err = csn_sub(&dy, &y->val, &my);
                        if (err) { val_err(&v, 255); return v; }
                        err = csn_mul(&px, &dx, &dx);
                        if (err) { val_err(&v, 255); return v; }
                        err = csn_add(&sxx, &sxx, &px); if (err) { val_err(&v, 255); return v; }
                        err = csn_mul(&px, &dx, &dy);
                        if (err) { val_err(&v, 255); return v; }
                        err = csn_add(&sxy, &sxy, &px); if (err) { val_err(&v, 255); return v; }
                    }
                    /* slope = Sxy / Sxx; icept = my - slope*mx */
                    if (csn_is_zero(&sxx)) { val_err(&v, 9); return v; }
                    err = csn_div(&g_reg_slope, &sxy, &sxx);
                    if (err) { val_err(&v, 255); return v; }
                    err = csn_mul(&v.num, &g_reg_slope, &mx);
                    if (err) { val_err(&v, 255); return v; }
                    err = csn_sub(&g_reg_icept, &my, &v.num);
                    if (err) { val_err(&v, 255); return v; }
                    csn_copy(&g_reg_meany, &my);
                    g_reg_ok = 1;
                    val_free(&v);
                    csn_copy(&v.num, &g_reg_meany); v.good = 1;
                    return v;
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
            /* CONV(value, from, to): the unit conversion
             * (engineering batch D, 2026-11-15).  Exact rationals:
             * canonical = value*num1/den1, result = canonical*den2/
             * num2.  Temperature stages through Celsius (the
             * non-multiplicative dimension).  Unknown unit or
             * dimension mismatch -> Error 0; a non-string unit
             * argument -> Error 6. */
            if (f == FN_CONV) {
                Val a;
                char un1[24], un2[24];
                int d1, d2, r;
                Csn num1, den1, num2, den2, canon, t;
                int err;
                a = parse_numarg(ps);
                if (!a.good) return a;
                skip_ws(&ps->p);
                if (*ps->p != ',') { val_err(&v, 3); val_free(&a); return v; }
                ps->p++;
                r = conv_unit_arg(ps, un1, (int) sizeof(un1));
                if (r < 0) { val_free(&a); val_err(&v, 0); return v; }
                if (r == 0) { val_free(&a); val_err(&v, 6); return v; }
                skip_ws(&ps->p);
                if (*ps->p != ',') {
                    val_err(&v, 3); val_free(&a); return v;
                }
                ps->p++;
                r = conv_unit_arg(ps, un2, (int) sizeof(un2));
                if (r < 0) { val_free(&a); val_err(&v, 0); return v; }
                if (r == 0) { val_free(&a); val_err(&v, 6); return v; }
                skip_ws(&ps->p);
                if (*ps->p != ')') { val_err(&v, 3); val_free(&a); return v; }
                ps->p++;
                val_free(&v);
                csn_zero(&v.num);
                err = 0;
                if (!conv_lookup(un1, &d1, &num1, &den1) ||
                    !conv_lookup(un2, &d2, &num2, &den2))
                    err = 1;                        /* unknown unit */
                else if (d1 != d2)
                    err = 1;                        /* dimensions differ */
                else if (d1 == UDD_TEMP) {
                    /* stage through Celsius: F -> (x-32)*5/9;
                     * K -> x-273.15; back by the inverse */
                    Csn five, nine, tt;
                    csn_from_int(&five, 5L);
                    csn_from_int(&nine, 9L);
                    if (conv_eq(un1, "f")) {
                        csn_from_int(&t, 32L);
                        err = csn_sub(&tt, &a.num, &t);
                        if (!err) err = csn_mul(&canon, &tt, &five);
                        if (!err) err = csn_div(&canon, &canon, &nine);
                    } else if (conv_eq(un1, "k")) {
                        csn_parse("273.15", &tt);
                        err = csn_sub(&canon, &a.num, &tt);
                    } else {
                        csn_copy(&canon, &a.num);
                    }
                    if (!err) {
                        if (conv_eq(un2, "f")) {
                            err = csn_mul(&t, &canon, &nine);
                            if (!err) err = csn_div(&t, &t, &five);
                            if (!err) {
                                Csn tt2;
                                csn_from_int(&tt2, 32L);
                                err = csn_add(&v.num, &t, &tt2);
                            }
                        } else if (conv_eq(un2, "k")) {
                            csn_parse("273.15", &t);
                            err = csn_add(&v.num, &canon, &t);
                        } else {
                            csn_copy(&v.num, &canon);
                        }
                    }
                } else {
                    /* the multiplicative dimensions */
                    err = csn_mul(&canon, &a.num, &num1);
                    if (!err) err = csn_div(&canon, &canon, &den1);
                    if (!err) err = csn_mul(&t, &canon, &den2);
                    if (!err) err = csn_div(&v.num, &t, &num2);
                }
                val_free(&a);
                if (err) { val_err(&v, 0); return v; }
                v.good = 1;
                return v;
            }
            /* ROOT(f, lo, hi): the bisection (engineering batch F,
             * 2026-11-15).  The argument is CAPTURED as text; X is
             * bound per trial.  The bracket must contain a sign
             * change (or an endpoint root); the stop is the relative
             * tolerance scale*1e-24 or 300 passes; the answer is the
             * interval's midpoint. */
            if (f == FN_ROOT) {
                char fbuf[ROOT_MAX];
                const char *q;
                int flen, iter, werr = 0;
                Val lo, hi;
                Csn a, b, fa, fb, m, fm, two;
                csn_zero(&a); csn_zero(&b);
                csn_zero(&fa); csn_zero(&fb);
                csn_zero(&m); csn_zero(&fm);
                skip_ws(&ps->p);
                q = root_arg_end(ps->p, ',');
                if (!q) { val_err(&v, 3); return v; }
                flen = (int) (q - ps->p);
                if (flen >= ROOT_MAX) { val_err(&v, 0); return v; }
                memcpy(fbuf, ps->p, (size_t) flen);
                fbuf[flen] = '\0';
                ps->p = q + 1;              /* past the ',' */
                skip_ws(&ps->p);
                lo = parse_numarg(ps);
                if (!lo.good) return lo;
                skip_ws(&ps->p);
                if (*ps->p != ',') { val_err(&v, 3); val_free(&lo); return v; }
                ps->p++;
                hi = parse_numarg(ps);
                if (!hi.good) { val_free(&lo); return hi; }
                skip_ws(&ps->p);
                if (*ps->p != ')') {
                    val_err(&v, 3); val_free(&lo); val_free(&hi);
                    return v;
                }
                ps->p++;
                val_free(&v);
                csn_zero(&v.num);
                /* the normalized bracket: a < b */
                if (csn_cmp(&lo.num, &hi.num) > 0) {
                    csn_copy(&a, &hi.num);
                    csn_copy(&b, &lo.num);
                } else {
                    csn_copy(&a, &lo.num);
                    csn_copy(&b, &hi.num);
                }
                val_free(&lo);
                val_free(&hi);
                if (csn_cmp(&a, &b) == 0) {
                    /* a degenerate "bracket": a root only if f = 0 */
                    if (root_eval(fbuf, &a, &fa)) { val_err(&v, 0); return v; }
                    if (csn_is_zero(&fa)) {
                        csn_copy(&v.num, &a);
                        v.good = 1;
                        return v;
                    }
                    val_err(&v, 0); return v;
                }
                if (root_eval(fbuf, &a, &fa) || root_eval(fbuf, &b, &fb)) {
                    val_err(&v, 0); return v;
                }
                /* the endpoint roots are returned directly */
                if (csn_is_zero(&fa)) {
                    csn_copy(&v.num, &a);
                    v.good = 1;
                    return v;
                }
                if (csn_is_zero(&fb)) {
                    csn_copy(&v.num, &b);
                    v.good = 1;
                    return v;
                }
                if (fa.sign == fb.sign) {
                    val_err(&v, 0); return v;   /* no sign change */
                }
                csn_from_int(&two, 2L);
                for (iter = 0; iter < ROOT_PASSES; iter++) {
                    Csn diff, sc, tol;
                    if (csn_add(&m, &a, &b) ||
                        csn_div(&m, &m, &two)) { werr = 1; break; }
                    if (root_eval(fbuf, &m, &fm)) { werr = 1; break; }
                    if (csn_is_zero(&fm)) break;    /* the exact root */
                    if (fm.sign == fa.sign) {
                        csn_copy(&a, &m);
                        csn_copy(&fa, &fm);
                    } else {
                        csn_copy(&b, &m);
                        csn_copy(&fb, &fm);
                    }
                    /* the relative tolerance: |b-a| <= max(|a|,|b|)*1e-24 */
                    if (csn_sub(&diff, &b, &a)) { werr = 1; break; }
                    csn_abs(&sc, &b);
                    csn_abs(&fm, &a);
                    if (csn_cmp(&fm, &sc) > 0) csn_copy(&sc, &fm);
                    if (csn_scale10(&tol, &sc, -24)) { werr = 1; break; }
                    if (csn_cmp(&diff, &tol) <= 0) break;
                }
                if (werr) { val_err(&v, 0); return v; }
                /* the interval's midpoint (also the cap exit) */
                if (csn_add(&m, &a, &b) || csn_div(&m, &m, &two)) {
                    val_err(&v, 0); return v;
                }
                csn_copy(&v.num, &m);
                v.good = 1;
                return v;
            }
            /* DIFF(xr, yr, t): the slope of the piecewise-linear
             * interpolant at t (engineering batch E, 2026-11-15).
             * x strictly increasing in walk order; t at an interior
             * knot takes the segment to the RIGHT, at the last knot
             * the last segment; outside the span -> Error 0. */
            if (f == FN_DIFF) {
                struct diff_acc da;
                int c1, r1, d1, e1, sw, sh, werr, wr;
                Csn t;
                memset(&da, 0, sizeof(da));
                csn_zero(&da.t); csn_zero(&da.slope);
                csn_zero(&da.px); csn_zero(&da.py);
                csn_zero(&da.qx); csn_zero(&da.qy);
                wr = pair_parse(ps, 1, &t, &werr,
                                &c1, &r1, &d1, &e1, &sw, &sh);
                if (wr != 0) {
                    if (werr >= 0) { val_err(&v, werr); return v; }
                    val_err(&v, 0); return v;
                }
                csn_copy(&da.t, &t);
                if (pair_walk(c1, r1, d1, e1, sw, sh, 0, &werr,
                              &da, diff_visit)) {
                    val_err(&v, 0); return v;
                }
                val_free(&v);
                csn_zero(&v.num);
                if (da.n < 2) { val_err(&v, 0); return v; }
                if (!da.found) {
                    /* t at the LAST knot: the last segment */
                    Csn dx, dy;
                    if (csn_cmp(&t, &da.px) != 0) {
                        val_err(&v, 0); return v;
                    }
                    if (csn_sub(&dx, &da.px, &da.qx) ||
                        csn_sub(&dy, &da.py, &da.qy) ||
                        csn_div(&da.slope, &dy, &dx)) {
                        val_err(&v, 0); return v;
                    }
                }
                csn_copy(&v.num, &da.slope);
                v.good = 1;
                return v;
            }
            /* SPLINE(xr, yr, t): the natural cubic spline through the
             * table (engineering batch E, 2026-11-15).  x strictly
             * increasing; t inside the knot span; the knots through
             * the pair walk (holes are errors); M by the Thomas
             * algorithm; n = 2 is the straight line. */
            if (f == FN_SPLINE) {
                struct spline_tab st;
                int c1, r1, d1, e1, sw, sh, werr, wr;
                Csn t;
                Csn *h = NULL, *cpr = NULL, *dpr = NULL;
                long n, m, i, j, seg;
                int err = 0;
                memset(&st, 0, sizeof(st));
                wr = pair_parse(ps, 1, &t, &werr,
                                &c1, &r1, &d1, &e1, &sw, &sh);
                if (wr != 0) {
                    if (werr >= 0) { val_err(&v, werr); return v; }
                    val_err(&v, 0); return v;
                }
                /* the counting pass, then the EXACT allocation
                 * (the user's memory report, 2026-11-16) */
                st.cap = 0;
                st.n = 0;
                if (pair_walk(c1, r1, d1, e1, sw, sh, 0, &werr,
                              &st, spline_count_visit)) {
                    val_err(&v, 0); return v;
                }
                st.cap = (int) st.n;
                st.n = 0;
                st.xs = st.n || st.cap
                    ? (Csn *) malloc((size_t) st.cap * sizeof(Csn))
                    : NULL;
                st.ys = st.cap
                    ? (Csn *) malloc((size_t) st.cap * sizeof(Csn))
                    : NULL;
                if (st.cap && (!st.xs || !st.ys)) {
                    free(st.xs); free(st.ys);
                    val_err(&v, 0); return v;
                }
                if (pair_walk(c1, r1, d1, e1, sw, sh, 0, &werr,
                              &st, spline_visit)) {
                    free(st.xs); free(st.ys);
                    val_err(&v, 0); return v;
                }
                val_free(&v);
                csn_zero(&v.num);
                n = st.n;
                if (n < 2) err = 1;                     /* no table */
                for (i = 0; !err && i < n - 1; i++)
                    if (csn_cmp(&st.xs[i + 1], &st.xs[i]) <= 0)
                        err = 1;                        /* not growing */
                if (!err && (csn_cmp(&t, &st.xs[0]) < 0 ||
                             csn_cmp(&t, &st.xs[n - 1]) > 0))
                    err = 1;                            /* outside */
                if (!err) {
                    h = (Csn *) malloc((size_t)(n - 1) *
                                       sizeof(Csn));
                    if (h) {
                        for (j = 0; j < n - 1; j++)
                            if (csn_sub(&h[j], &st.xs[j + 1],
                                        &st.xs[j])) { err = 1; break; }
                    } else
                        err = 1;
                }
                /* the Thomas solve for the interior M (m = n-2
                 * unknowns; the sub/super are the h values
                 * themselves) */
                m = n - 2;
                if (!err && m > 0) {
                    cpr = (Csn *) malloc((size_t) m * sizeof(Csn));
                    dpr = (Csn *) malloc((size_t) m * sizeof(Csn));
                    if (!cpr || !dpr) err = 1;
                }
                for (i = 0; !err && i < m; i++) {
                    Csn bi, di, s1, s2, den, six;
                    csn_from_int(&six, 6L);
                    csn_from_int(&bi, 2L);
                    /* b_i = 2*(h_i + h_i+1) */
                    if (csn_add(&den, &h[i], &h[i + 1])) { err = 1; break; }
                    if (csn_mul(&bi, &bi, &den)) { err = 1; break; }
                    /* d_i = 6*(s_k - s_k-1), s_j = (y[j+1]-y[j])/h[j] */
                    if (csn_sub(&s1, &st.ys[i + 2], &st.ys[i + 1]) ||
                        csn_div(&s1, &s1, &h[i + 1]) ||
                        csn_sub(&s2, &st.ys[i + 1], &st.ys[i]) ||
                        csn_div(&s2, &s2, &h[i]) ||
                        csn_sub(&di, &s1, &s2) ||
                        csn_mul(&di, &di, &six)) { err = 1; break; }
                    if (i == 0) {
                        if (csn_div(&cpr[0], &h[1], &bi) ||
                            csn_div(&dpr[0], &di, &bi)) { err = 1; break; }
                    } else {
                        if (csn_mul(&den, &h[i], &cpr[i - 1]) ||
                            csn_sub(&den, &bi, &den)) { err = 1; break; }
                        if (csn_is_zero(&den)) { err = 1; break; }
                        if (csn_div(&cpr[i], &h[i + 1], &den) ||
                            csn_mul(&s1, &h[i], &dpr[i - 1]) ||
                            csn_sub(&s1, &di, &s1) ||
                            csn_div(&dpr[i], &s1, &den)) { err = 1; break; }
                    }
                }
                /* the back substitution in place (dpr becomes M) */
                for (i = m - 2; !err && i >= 0; i--) {
                    Csn tt;
                    if (csn_mul(&tt, &cpr[i], &dpr[i + 1]) ||
                        csn_sub(&dpr[i], &dpr[i], &tt)) { err = 1; break; }
                }
                /* the evaluation: find the segment */
                if (!err) {
                    seg = n - 2;                    /* t at the last knot */
                    for (i = 0; i < n - 1; i++)
                        if (csn_cmp(&t, &st.xs[i + 1]) <= 0) {
                            seg = i; break;
                        }
                    {
                        Csn A, B, Mi, Mj, a3, b3, tt, six, one;
                        csn_from_int(&six, 6L);
                        csn_from_int(&one, 1L);
                        if (csn_sub(&A, &st.xs[seg + 1], &t) ||
                            csn_div(&A, &A, &h[seg]) ||
                            csn_sub(&B, &t, &st.xs[seg]) ||
                            csn_div(&B, &B, &h[seg])) err = 1;
                        /* M_seg: 0 at the first knot, else dpr[seg-1];
                         * M_seg+1: 0 at the last, else dpr[seg] */
                        csn_zero(&Mi);
                        csn_zero(&Mj);
                        if (!err && seg > 0)
                            csn_copy(&Mi, &dpr[seg - 1]);
                        if (!err && seg < n - 2)
                            csn_copy(&Mj, &dpr[seg]);
                        if (!err) {
                            /* A^3 - A and B^3 - B */
                            if (csn_mul(&a3, &A, &A) ||
                                csn_mul(&a3, &a3, &A) ||
                                csn_sub(&a3, &a3, &A) ||
                                csn_mul(&b3, &B, &B) ||
                                csn_mul(&b3, &b3, &B) ||
                                csn_sub(&b3, &b3, &B)) err = 1;
                        }
                        if (!err) {
                            if (csn_mul(&a3, &a3, &Mi) ||
                                csn_mul(&b3, &b3, &Mj) ||
                                csn_add(&tt, &a3, &b3) ||
                                csn_mul(&tt, &tt, &h[seg]) ||
                                csn_mul(&tt, &tt, &h[seg]) ||
                                csn_div(&tt, &tt, &six)) err = 1;
                        }
                        if (!err) {
                            if (csn_mul(&A, &A, &st.ys[seg]) ||
                                csn_mul(&B, &B, &st.ys[seg + 1]) ||
                                csn_add(&v.num, &A, &B) ||
                                csn_add(&v.num, &v.num, &tt)) err = 1;
                        }
                    }
                }
                free(st.xs); free(st.ys); free(h);
                free(cpr); free(dpr);
                if (err) { val_err(&v, 0); return v; }
                v.good = 1;
                return v;
            }
            /* CORR/RSQ: the paired statistics (engineering batch C,
             * 2026-11-15).  Two aligned ranges; a pair with no number
             * on either side is SKIPPED (the user's locked rule);
             * r = (n*sxy - sx*sy) / sqrt((n*sxx-sx^2)*(n*syy-sy^2)).
             * Zero variance on either side or fewer than 2 pairs is
             * an error.  RSQ = r*r, computed as num^2/denom. */
            if (f == FN_CORR || f == FN_RSQ) {
                struct corr_mean cm;
                struct corr_cen cc;
                int c1, r1, d1, e1, sw, sh, werr, wr;
                Csn ncs, dd, t;
                int err;
                memset(&cm, 0, sizeof(cm));
                csn_zero(&cm.sx); csn_zero(&cm.sy);
                wr = pair_parse(ps, 0, &t, &werr,
                                &c1, &r1, &d1, &e1, &sw, &sh);
                if (wr != 0) {
                    if (werr >= 0) { val_err(&v, werr); return v; }
                    val_err(&v, 0); return v;
                }
                /* pass 1: the means */
                if (pair_walk(c1, r1, d1, e1, sw, sh, 1, &werr,
                              &cm, corr_mean_visit)) {
                    val_err(&v, 0); return v;
                }
                val_free(&v);
                csn_zero(&v.num);
                if (cm.n < 2) { val_err(&v, 0); return v; }
                csn_from_int(&ncs, cm.n);
                memset(&cc, 0, sizeof(cc));
                csn_zero(&cc.sxy); csn_zero(&cc.sxx); csn_zero(&cc.syy);
                if (csn_div(&cc.mx, &cm.sx, &ncs) ||
                    csn_div(&cc.my, &cm.sy, &ncs)) {
                    val_err(&v, 0); return v;
                }
                /* pass 2: the centered deviations */
                if (pair_walk(c1, r1, d1, e1, sw, sh, 1, &werr,
                              &cc, corr_cen_visit)) {
                    val_err(&v, 0); return v;
                }
                /* r = Sxy / sqrt(Sxx * Syy) - no cancellation */
                err = csn_mul(&dd, &cc.sxx, &cc.syy);
                if (err) { val_err(&v, 0); return v; }
                if (csn_is_zero(&dd)) { val_err(&v, 0); return v; }
                if (f == FN_RSQ) {
                    err = csn_mul(&t, &cc.sxy, &cc.sxy);
                    if (!err) err = csn_div(&v.num, &t, &dd);
                } else {
                    err = csn_sqrt(&t, &dd);
                    if (!err) err = csn_div(&v.num, &cc.sxy, &t);
                }
                if (err) { val_err(&v, 0); return v; }
                v.good = 1;
                return v;
            }
            /* INTEG: the trapezoidal rule over a paired table
             * (engineering batch C, 2026-11-15).  x strictly
             * increasing in walk order; a hole in the table is an
             * ERROR (the table IS the function - the user's locked
             * rule, unlike the statistics' skip). */
            if (f == FN_INTEG) {
                struct integ_acc ia;
                int c1, r1, d1, e1, sw, sh, werr, wr;
                Csn t;
                memset(&ia, 0, sizeof(ia));
                csn_zero(&ia.total);
                csn_zero(&ia.px); csn_zero(&ia.py);
                wr = pair_parse(ps, 0, &t, &werr,
                                &c1, &r1, &d1, &e1, &sw, &sh);
                if (wr != 0) {
                    if (werr >= 0) { val_err(&v, werr); return v; }
                    val_err(&v, 0); return v;
                }
                if (pair_walk(c1, r1, d1, e1, sw, sh, 0, &werr,
                              &ia, integ_visit)) {
                    val_err(&v, 0); return v;
                }
                val_free(&v);
                csn_zero(&v.num);
                if (ia.n < 2) { val_err(&v, 0); return v; }
                csn_copy(&v.num, &ia.total);
                v.good = 1;
                return v;
            }
            /* RAND(min,max) / RANDF(min,max) / SEED(x) (engineering
             * batch G, 2026-11-15).  RAND: a uniform INTEGER on
             * [min,max] - the bounds truncate OUTWARD (floor/ceil)
             * so the interval stays covered, min > max swaps.  RANDF:
             * a uniform real on [min,max) (the max is excluded by
             * construction - flagged).  SEED: sets the state, returns
             * the value; entries calling any of the three are
             * volatile-flagged at commit. */
            if (f == FN_RAND || f == FN_RANDF) {
                Val a, b;
                Csn mnl, mxl, n, r, diff;
                int err;
                a = parse_numarg(ps);
                if (!a.good) return a;
                skip_ws(&ps->p);
                if (*ps->p != ',') { val_err(&v, 3); val_free(&a); return v; }
                ps->p++;
                b = parse_numarg(ps);
                if (!b.good) { val_free(&a); return b; }
                skip_ws(&ps->p);
                if (*ps->p != ')') {
                    val_err(&v, 3); val_free(&a); val_free(&b);
                    return v;
                }
                ps->p++;
                val_free(&v);
                csn_zero(&v.num);
                if (!g_lcg_seeded) lcg_seed();
                err = 0;
                if (f == FN_RAND) {
                    /* truncate outward: floor(min), ceil(max) */
                    if (csn_floor(&mnl, &a.num) ||
                        csn_ceil(&mxl, &b.num)) err = 1;
                    if (!err && csn_cmp(&mnl, &mxl) > 0) {
                        csn_copy(&r, &mnl);
                        csn_copy(&mnl, &mxl);
                        csn_copy(&mxl, &r);
                    }
                    if (!err) err = csn_sub(&n, &mxl, &mnl);
                    if (!err) {
                        Csn one;
                        csn_from_int(&one, 1L);
                        if (csn_add(&n, &n, &one)) err = 1;
                    }
                    /* N == 1 draws r = 0 -> the fixed value */
                    if (!err) {
                        if (lcg_draw(&n, &r)) err = 1;
                        if (!err && csn_add(&v.num, &mnl, &r)) err = 1;
                    }
                } else {
                    /* RANDF: the raw bounds, [min, max) */
                    if (csn_cmp(&a.num, &b.num) > 0) {
                        csn_copy(&r, &a.num);
                        csn_copy(&mnl, &b.num);
                        csn_copy(&mxl, &r);
                    } else {
                        csn_copy(&mnl, &a.num);
                        csn_copy(&mxl, &b.num);
                    }
                    if (csn_cmp(&mnl, &mxl) == 0) {
                        csn_copy(&v.num, &mnl);
                    } else {
                        Csn frac, hi;
                        if (csn_sub(&diff, &mxl, &mnl)) err = 1;
                        if (!err) {
                            lcg_update();
                            if (csn_scale10(&hi, &g_lcg, -20)) err = 1;
                            if (!err && csn_floor(&hi, &hi)) err = 1;
                            /* frac = hi20 * 10^-20, in [0,1) - the
                             * HIGH digits, like the integer draw */
                            if (!err &&
                                csn_scale10(&frac, &hi, -20)) err = 1;
                        }
                        if (!err && csn_mul(&r, &diff, &frac)) err = 1;
                        if (!err && csn_add(&v.num, &mnl, &r)) err = 1;
                    }
                }
                val_free(&a); val_free(&b);
                if (err) { val_err(&v, 0); return v; }
                v.good = 1;
                return v;
            }
            if (f == FN_SEED) {
                Val a = parse_numarg(ps);
                Csn p40;
                if (!a.good) return a;
                skip_ws(&ps->p);
                if (*ps->p != ')') { val_err(&v, 3); val_free(&a); return v; }
                ps->p++;
                val_free(&v);
                csn_copy(&v.num, &a.num);           /* returns the value */
                csn_parse(LCG_P40, &p40);
                if (!csn_mod(&g_lcg, &a.num, &p40) &&
                    g_lcg.sign) {
                    Csn tt;
                    csn_copy(&tt, &g_lcg);
                    csn_sub(&g_lcg, &tt, &p40);     /* negative state
                                                       wraps positive */
                    csn_neg(&g_lcg, &g_lcg);
                }
                g_lcg_seeded = 1;
                val_free(&a);
                v.good = 1;
                return v;
            }
            /* PCT/QUART: the PERCENTILE.INC family (engineering
             * batch B, 2026-11-15).  One range/scalar argument
             * collected into the bounded buffer, then the p / q
             * argument.  Empty or p out of [0,1] is an error (unlike
             * MED's empty -> 0: a rank of nothing is undefined). */
            if (f == FN_PCT || f == FN_QUART) {
                Csn *mbuf = NULL;
                int mcap = 16, mcnt = 0, e2, err = 0;
                Csn p, zero, one, lo, fr, t, ncs;
                long loi, i, j;
                mbuf = (Csn *) malloc((size_t) mcap * sizeof(Csn));
                if (!mbuf) { val_err(&v, 0); return v; }
                /* the WHOLE argument list is collected (ranges and
                 * scalars mix); the LAST argument is the percentile -
                 * that resolves PCT(1,2,3,4,0.5): data 1,2,3, p 0.5 */
                e2 = parse_med_collect(ps, &mbuf, &mcap, &mcnt);
                if (e2 >= 0) { free(mbuf); val_err(&v, e2); return v; }
                skip_ws(&ps->p);
                if (*ps->p != ')') {
                    free(mbuf); val_err(&v, 3); return v;
                }
                ps->p++;
                val_free(&v);
                csn_zero(&v.num);
                if (mcnt == 0) {
                    free(mbuf); val_err(&v, 3); return v;
                }
                p = mbuf[--mcnt];
                /* QUART's q: an exact integer 0..4 -> p = q/4 */
                if (f == FN_QUART) {
                    Csn qi;
                    if (!csn_integer_part(&p, &qi) ||
                        csn_cmp(&qi, &p) != 0) {
                        free(mbuf);
                        val_err(&v, 0); return v;
                    }
                    csn_from_int(&t, 4L);
                    err = csn_div(&p, &qi, &t);
                }
                csn_zero(&zero);
                csn_from_int(&one, 1L);
                if (!err && (csn_cmp(&p, &zero) < 0 ||
                             csn_cmp(&p, &one) > 0))
                    err = 1;
                /* sort ascending (the MED insertion sort) */
                for (i = 1; !err && i < mcnt; i++) {
                    t = mbuf[i];
                    for (j = i - 1;
                         j >= 0 && csn_cmp(&mbuf[j], &t) > 0; j--)
                        mbuf[j + 1] = mbuf[j];
                    mbuf[j + 1] = t;
                }
                if (!err && mcnt == 0)
                    err = 1;            /* no numbers to rank */
                if (!err) {
                    /* rank = p*(n-1); lo = floor(rank);
                     * result = v[lo] + fr*(v[lo+1]-v[lo]) */
                    csn_from_int(&ncs, (long) mcnt - 1);
                    err = csn_mul(&t, &p, &ncs);
                    if (!err) err = csn_floor(&lo, &t);
                    if (!err) err = csn_sub(&fr, &t, &lo);
                    if (!err) {
                        loi = csn_to_long_val(&lo);
                        if (loi >= mcnt - 1 || csn_is_zero(&fr))
                            csn_copy(&v.num, &mbuf[loi]);
                        else {
                            err = csn_sub(&t, &mbuf[loi + 1],
                                          &mbuf[loi]);
                            if (!err) err = csn_mul(&t, &t, &fr);
                            if (!err)
                                err = csn_add(&v.num, &mbuf[loi], &t);
                        }
                    }
                }
                free(mbuf);
                if (err) { val_err(&v, 0); return v; }
                v.good = 1;
                return v;
            }
            /* SUM/CNT/AVG/MAX/MIN stream; MED uses a bounded buffer
             * (no value array for the streaming four - 2026-11-14) */
            if (f == FN_SUM || f == FN_CNT || f == FN_AVG ||
                f == FN_MAX || f == FN_MIN || f == FN_MED ||
                f == FN_FAND || f == FN_FOR ||
                f == FN_SUMSQ || f == FN_RMS ||
                f == FN_SUMABS || f == FN_SUMPOS || f == FN_SUMNEG ||
                f == FN_MAXABS || f == FN_MINABS ||
                f == FN_VAR || f == FN_VARP ||
                f == FN_STDEV || f == FN_STDEVP) {
                CsAgg ag;
                Csn *mbuf = NULL;
                int mcap = 16, mcnt = 0, e2;
                if (f == FN_MED) {
                    mbuf = (Csn *) malloc((size_t) mcap * sizeof(Csn));
                    if (!mbuf) { val_err(&v, 0); return v; }
                    e2 = parse_med_collect(ps, &mbuf, &mcap, &mcnt);
                } else {
                    memset(&ag, 0, sizeof(ag));
                    csn_zero(&ag.sum);
                    e2 = parse_stream(ps, f, &ag);
                }
                if (e2 >= 0) {
                    if (mbuf) free(mbuf);
                    val_err(&v, e2);
                    return v;
                }
                skip_ws(&ps->p);
                if (*ps->p != ')') {
                    if (mbuf) free(mbuf);
                    val_err(&v, 3);
                    return v;
                }
                ps->p++;
                val_free(&v);
                v.good = 1;
                if (f == FN_MED) {
                    /* the median (user-ext 2026-11-13): the numbers
                     * sorted ascending; odd count -> the middle
                     * element, even count -> the average of the two
                     * middle elements (Excel-style); empty -> 0 like
                     * AVG */
                    int i, j;
                    Csn t;
                    for (i = 1; i < mcnt; i++) {
                        t = mbuf[i];
                        for (j = i - 1; j >= 0 && csn_cmp(&mbuf[j], &t) > 0; j--)
                            mbuf[j + 1] = mbuf[j];
                        mbuf[j + 1] = t;
                    }
                    if (mcnt == 0) {
                        csn_zero(&v.num);
                    } else if (mcnt % 2 == 1) {
                        csn_copy(&v.num, &mbuf[mcnt / 2]);
                    } else {
                        Csn two, sum;
                        int err;
                        err = csn_add(&sum, &mbuf[mcnt / 2 - 1],
                                      &mbuf[mcnt / 2]);
                        if (err) { val_err(&v, 0); free(mbuf); return v; }
                        csn_from_int(&two, 2);
                        err = csn_div(&v.num, &sum, &two);
                        if (err) { val_err(&v, 0); free(mbuf); return v; }
                    }
                    free(mbuf);
                    return v;
                }
                /* the variance family and RMS have NO empty form
                 * (the user's edge table: n = 0 -> Error 0; VARP/
                 * STDEVP at n = 1 are 0 below) - unlike SUM/AVG's 0 */
                if ((f == FN_RMS || f == FN_VAR || f == FN_VARP ||
                     f == FN_STDEV || f == FN_STDEVP) && ag.cnt == 0) {
                    val_err(&v, 0);
                    return v;
                }
                if (ag.cnt == 0) {
                    csn_zero(&v.num);
                    return v;
                }
                if (f == FN_FAND || f == FN_FOR) {
                    csn_copy(&v.num, &ag.ext);
                } else if (f == FN_SUMSQ) {
                    csn_copy(&v.num, &ag.sumsq);
                } else if (f == FN_RMS) {
                    Csn ncs;
                    int derr;
                    csn_from_int(&ncs, ag.cnt);
                    derr = csn_div(&v.num, &ag.sumsq, &ncs);
                    if (!derr) derr = csn_sqrt(&v.num, &v.num);
                    if (derr) { val_err(&v, 0); return v; }
                } else if (f == FN_VAR || f == FN_STDEV) {
                    Csn ncs;
                    int derr;
                    if (ag.cnt < 2) { val_err(&v, 0); return v; }
                    csn_from_int(&ncs, ag.cnt - 1);
                    derr = csn_div(&v.num, &ag.m2, &ncs);
                    if (!derr && f == FN_STDEV)
                        derr = csn_sqrt(&v.num, &v.num);
                    if (derr) { val_err(&v, 0); return v; }
                } else if (f == FN_VARP || f == FN_STDEVP) {
                    Csn ncs;
                    int derr;
                    csn_from_int(&ncs, ag.cnt);
                    derr = csn_div(&v.num, &ag.m2, &ncs);
                    if (!derr && f == FN_STDEVP)
                        derr = csn_sqrt(&v.num, &v.num);
                    if (derr) { val_err(&v, 0); return v; }
                } else if (f == FN_SUM) {
                    csn_copy(&v.num, &ag.sum);
                } else if (f == FN_SUMABS || f == FN_SUMPOS ||
                           f == FN_SUMNEG) {
                    csn_copy(&v.num, &ag.sum);
                } else if (f == FN_CNT) {
                    csn_from_int(&v.num, ag.cnt);
                } else if (f == FN_AVG) {
                    Csn ncs;
                    int derr;
                    csn_from_int(&ncs, ag.cnt);
                    derr = csn_div(&v.num, &ag.sum, &ncs);
                    if (derr) { val_err(&v, 0); return v; }
                } else {
                    csn_copy(&v.num, &ag.ext);
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
        if (t.type == VT_STR) {
            if (neg) { val_free(&t); val_err(&t, 6); return t; }
            return t;      /* a leading '+' affirms: +CHR(65) is "A" */
        }
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
        if (a.type == VT_STR || b.type == VT_STR ||
            a.type == VT_RANGE || b.type == VT_RANGE) {
            val_free(&a); val_free(&b); val_err(&a, 6); return a;
        }
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

static Val parse_concat(P *ps);

/* string concatenation with '&' (user-ext 2026-11-13): String & String
 * only - mixing a Number in is Error 6 (the strict rule).  Sits
 * between the additive and the comparison levels. */
static Val parse_concat(P *ps)
{
    Val a;
    memset(&a, 0, sizeof(a));
    a = parse_add(ps);
    if (!a.good) return a;
    /* no eager dereference: a Reference stays raw for '>' (which sits
     * ABOVE this level) - the dereference happens at the operation */
    for (;;) {
        const char *p = ps->p;
        Val b;
        int e;
        memset(&b, 0, sizeof(b));
        skip_ws(&p);
        if (*p == ':') break;
        if (*p != '&') break;
        ps->p = p + 1;
        b = parse_add(ps);
        if (!b.good) { val_free(&a); val_free(&b); return b; }
        /* both operands are consumed here: deref (option 3) */
        e = val_deref(&a);
        if (e >= 0) { val_free(&a); val_free(&b); val_err(&a, e); return a; }
        e = val_deref(&b);
        if (e >= 0) { val_free(&a); val_free(&b); val_err(&a, e); return a; }
        if (a.type == VT_STR && b.type == VT_STR) {
            char *cc = (char *) malloc(strlen(a.str) + strlen(b.str) + 1);
            Val rv;
            if (!cc) { val_free(&a); val_free(&b); val_err(&a, 0); return a; }
            strcpy(cc, a.str);
            strcat(cc, b.str);
            val_free(&a);
            val_free(&b);
            memset(&rv, 0, sizeof(rv));
            rv.str = cc;
            rv.type = VT_STR;
            rv.good = 1;
            a = rv;
            continue;
        }
        val_free(&a); val_free(&b);
        val_err(&a, 6);
        return a;
    }
    return a;
}

static Val parse_cmp(P *ps)
{
    Val a;
    memset(&a, 0, sizeof(a));
    a = parse_concat(ps);
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
        } else if (*p == '>') {
            if (p[1] == '=') op = 6;       /* >= (the mirror of <=, the
                                            * user's threshold tests
                                            * reached for it, 2026-11-15) */
            else op = 5;                   /* >  */
        }
        else if (*p == '=') op = 3;
        else return a;
        ps->p = p + ((op == 2 || op == 4 || op == 6) ? 2 : 1);
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
        /* STRING vs STRING: the six operators via the byte order
         * (user-ext 2026-11-13) - the same collating sequence the
         * sort's text keys use; case-sensitive 7-bit ASCII */
        if (a.type == VT_STR && b.type == VT_STR) {
            int cv = strcmp(a.str, b.str);
            switch (op) {
            case 1: r = (cv < 0); break;
            case 2: r = (cv <= 0); break;
            case 3: r = (cv == 0); break;
            case 4: r = (cv != 0); break;
            case 5: r = (cv > 0); break;
            case 6: r = (cv >= 0); break;
            default: r = (cv > 0); break;
            }
            csn_from_int(&a.num, r ? 1 : 0);
            a.type = VT_BOOL;
            val_free(&b);
            return a;
        }
        /* MIXED string/number (through any path) is an error */
        if (a.type == VT_STR || b.type == VT_STR) { val_free(&a); val_free(&b); val_err(&a, 6); return a; }
        if (a.type == VT_RANGE || b.type == VT_RANGE) { val_free(&a); val_free(&b); val_err(&a, 6); return a; }
        r = csn_cmp(&a.num, &b.num);
        switch (op) {
        case 1: r = (r < 0); break;
        case 2: r = (r <= 0); break;
        case 3: r = (r == 0); break;
        case 4: r = (r != 0); break;
        case 5: r = (r > 0); break;
        case 6: r = (r >= 0); break;
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
extern int cs_recalc_target_on;
extern unsigned char cs_recalc_target_col, cs_recalc_target_row;

/* poll for an ESC keystroke (screen.c, non-blocking); 1 = ESC seen */
extern int cs_poll_escape(void);

/* execute SET(cell,value): the value is fully evaluated first, then
 * the target is written (SET(A1,A1+1) uses A1's previous value) */
/* 1 = the text is a pure number literal: its re-evaluation yields the
 * same constant every pass, so SET may replace the entry with the
 * written value.  Anything else (an expression, a reference, a
 * conditional) is a formula: SET into that cell updates the VALUE and
 * keeps the entry text, so a state machine whose counter targets its
 * own cell keeps its program (2026-11-13). */
static int num_literal_p(const char *s)
{
    int dig = 0;
    while (*s == ' ') s++;
    if (*s == '+' || *s == '-') s++;
    while (*s >= '0' && *s <= '9') { s++; dig = 1; }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') { s++; dig = 1; }
    }
    if (!dig) return 0;
    if (*s == 'e' || *s == 'E') {
        s++;
        if (*s == '+' || *s == '-') s++;
        if (!(*s >= '0' && *s <= '9')) return 0;
        while (*s >= '0' && *s <= '9') s++;
    }
    while (*s == ' ') s++;
    return *s == '\0';
}

static void exec_set_stmt(const char *args, int *err)
{
    const char *w;
    P sub;
    Val tgt, v;
    char just, oldjust;
    Cell *cell;
    int e;
    CellType oldtype;
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
    cell = cs_alloc_cell(tgt.ref.col, tgt.ref.row);
    if (!cell) { cs_msg("MEMORY IS TOO LOW"); val_free(&tgt); *err = 0; return; }
    skip_ws(&sub.p);
    if (*sub.p != ',') {
        val_free(&tgt); *err = 7;
        cs_release_if_empty(tgt.ref.col, tgt.ref.row);
        return;
    }
    sub.p++;

    /* the value: fully evaluated BEFORE the target is modified */
    v = parse_condexpr(&sub);
    if (!v.good) {
        *err = v.err;                   /* 0 = the value-range class */
        val_free(&v);
        cs_release_if_empty(tgt.ref.col, tgt.ref.row);
        return;
    }
    while (*sub.p == ' ') sub.p++;
    if (*sub.p) { val_free(&v); val_free(&tgt); *err = 7;
        cs_release_if_empty(tgt.ref.col, tgt.ref.row); return; }
    e = val_deref(&v);                  /* option 3: a value derefs */
    if (e >= 0) { val_free(&v); val_free(&tgt); *err = e;
        cs_release_if_empty(tgt.ref.col, tgt.ref.row); return; }
    if (v.type == VT_RANGE) { val_free(&v); val_free(&tgt); *err = 6;
        cs_release_if_empty(tgt.ref.col, tgt.ref.row); return; }

    /* the entry-pool budget was checked by cs_alloc_cell (NULL =>
     * MEMORY IS TOO LOW at the target resolution) */

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
    } else if (cell->type == CE_NUM && cell->cont &&
               !num_literal_p(cell->cont)) {
        /* numeric into a FORMULA cell: the value changes, the entry
         * text stays - the formula must survive a SET into its own
         * cell (self-targeting counters, 2026-11-13) */
        csn_copy(&cell->val, &v.num);
        cell->iserr = 0;
        if (cell->strres) { free(cell->strres); cell->strres = NULL; }
    } else {
        /* numeric into an empty/text/literal cell: the cell becomes a
         * plain value (no reference text) */
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

/* execute RECALC() / RECALC(cell): both mark the recalculation
 * request; WITH an argument the cell is the one-shot VOLATILE target
 * - its first visit in the settling passes redraws it (the explicit
 * re-roll), everything else retains */
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
        cs_recalc_target_col = (unsigned char) a.ref.col;
        cs_recalc_target_row = (unsigned char) a.ref.row;
        cs_recalc_target_on = 1;
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
            /* the truth test: a STRING condition is truthy when
             * nonempty; a NUMERIC/BOOL condition passes the single
             * fuzzy Boolean-interpretation threshold
             * (FUZZY_TRUE_THRESHOLD = 0.7 - the ONLY truth-test site;
             * comparisons produce exact 0/1 and are unaffected) */
            if (cond.type == VT_STR)
                ctrue = (cond.str && cond.str[0]);
            else {
                cs_fuzzy_consts();
                ctrue = csn_cmp(&cond.num, &cs_fuzzy_true) >= 0;
            }
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
 * TYPED value (a Reference or Range stays intact for the caller).
 * dry = 1 parses WITHOUT running side-effect statements (the
 * syntax-check path - the old unconditional dry = 0 made the
 * "parse-only" check EXECUTE SETs, the user's 2026-11-16 report) */
static int cs_eval_typed(const char *s, Val *out, int dry)
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
    ps.dry = dry;
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
    int e = cs_eval_typed(s, &v, 0);
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
    int e = cs_eval_typed(s, &v, 1);        /* DRY: no side effects */
    val_free(&v);
    return e;
}

/* evaluate an expression to a NUMBER for the factorial entries
 * (user-ext 2026-11-13): 0 = ok with the value in *out (References
 * coerce), else the error code and *out zeroed */
int cs_eval_fact_source(const char *s, Csn *out)
{
    Val v;
    int e;
    /* the factorial entry shows ?n? until its source is a real
     * number: empty references stay errors here (the user's decision
     * (b), 2026-11-13) */
    cs_empty_strict = 1;
    e = cs_eval_typed(s, &v, 0);
    if (e) { cs_empty_strict = 0; csn_zero(out); return e; }
    {
        int d = val_deref(&v);
        cs_empty_strict = 0;
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
    e = cs_eval_typed(s, &v, 0);
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
    int e = cs_eval_typed(s, &v, 0);
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
