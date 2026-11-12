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

typedef struct {
    Csn num;
    char  *str;    /* malloc'd when isstr */
    int    isstr;
    int    isbool;
    int    err;
    int    good;
} Val;

typedef struct {
    const char *p;
    int depth;
    int nocond;      /* inside a conditional branch: ':' is a terminator */
} P;

static void val_free(Val *v)
{
    if (v->str) { free(v->str); v->str = NULL; }
}

static void val_err(Val *v, int code)
{
    val_free(v);
    v->str = NULL; v->isstr = 0; v->isbool = 0;
    v->err = code; v->good = 0;
    csn_zero(&v->num);
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

static void cellnum(Csn *out, int col, int row, int *err)
{
    Cell *c = cs_cell(col, row);
    *err = 0;
    csn_zero(out);
    if (!c) { *err = 1; return; }
    if (c->type == CE_NUM && !c->iserr) { csn_copy(out, &c->val); return; }
    *err = 1;                      /* empty/allocated/text/error cells */
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
    v->isbool = 0; v->isstr = 0;
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
    v->good = 1; v->isstr = 1; v->isbool = 0;
    return q;
}

/* ---- function arguments --------------------------------------------- */
typedef enum { AK_VAL, AK_RANGE } ArgKind;

typedef enum {
    FN_SUM, FN_CNT, FN_AVG, FN_MAX, FN_MIN,
    FN_SQRT, FN_LOG, FN_LN, FN_ABS, FN_EXP,
    FN_REGR, FN_PROJ, FN_DEPD, FN_SLOPE, FN_UNKNOWN,
    FN_SIN, FN_COS, FN_TAN, FN_ASIN, FN_ACOS, FN_ATAN, FN_PI,
    FN_SIGN, FN_MOD, FN_POWER, FN_LOG10, FN_DEGREES, FN_RADIANS,
    FN_DEG, FN_RAD,
    FN_FACT, FN_COMBIN, FN_PERMUT, FN_GCD, FN_LCM, FN_ROUND, FN_TRUNC
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
    return FN_UNKNOWN;
}

static Val parse_condexpr(P *ps);   /* forward */

static Val parse_arg(P *ps, ArgKind *kind,
                     int *c1, int *r1, int *c2, int *r2)
{
    const char *p = ps->p;
    int l, d;
    while (*p == ' ') p++;
    if (is_coord_ahead(p, &l, &d)) {
        int a1, b1, ee = 0;
        const char *q = parse_coord_tok(p, &a1, &b1, &ee, NULL);
        if (!q) { Val v; memset(&v, 0, sizeof(v)); val_err(&v, ee); return v; }
        {
            const char *q2 = q;
            while (*q2 == ' ') q2++;
            if (*q2 == '>') {
                int a2, b2, ee2 = 0;
                const char *q3;
                const char *s = q2 + 1;
                while (*s == ' ') s++;
                q3 = parse_coord_tok(s, &a2, &b2, &ee2, NULL);
                if (!q3) { Val v; memset(&v, 0, sizeof(v)); val_err(&v, ee2); return v; }
                *c1 = a1; *r1 = b1; *c2 = a2; *r2 = b2;
                cs_range_norm(c1, r1, c2, r2);
                if (*c1 != *c2 && *r1 != *r2) { Val v; memset(&v, 0, sizeof(v)); val_err(&v, 2); return v; }
                *kind = AK_RANGE;
                ps->p = q3;
                { Val v; memset(&v, 0, sizeof(v)); v.good = 1; return v; }
            }
        }
        /* single coordinate as a value: numeric cells yield their
         * number; TEXT cells yield their content as a string \"cell
         * text\", so conditionals can produce/reference text */
        {
            Val v;
            Cell *cl;
            memset(&v, 0, sizeof(v));
            cl = cs_cell(a1, b1);
            if (cl && cl->type == CE_TEXT && cl->cont && !cl->iserr) {
                v.str = cs_strdup(cl->cont);
                if (!v.str) { val_err(&v, 0); return v; }
                v.isstr = 1;
                v.good = 1;
            } else {
                int err;
                cellnum(&v.num, a1, b1, &err);
                if (err) { val_err(&v, err); return v; }
                v.good = 1;
            }
            *c1 = a1; *r1 = b1;
            *kind = AK_VAL;
            ps->p = q;
            return v;
        }
    }
    /* full expression */
    {
        Val t = parse_condexpr(ps);
        if (t.good && t.isstr) { val_free(&t); val_err(&t, 6); return t; }
        *kind = AK_VAL;
        return t;
    }
}

static void skip_ws(const char **pp)
{
    while (**pp == ' ') (*pp)++;
}

static Val parse_list(P *ps, Csn *list, int *cnt)
{
    int n = 0;
    Val rv;
    memset(&rv, 0, sizeof(rv));
    rv.good = 1;
    for (;;) {
        ArgKind kind;
        int c1 = 0, r1 = 0, c2 = 0, r2 = 0;
        Val v = parse_arg(ps, &kind, &c1, &r1, &c2, &r2);
        if (!v.good) { val_free(&v); return v; }   /* parse_arg error paths
                                                      leave *kind unset */
        if (kind == AK_RANGE) {
            int c, r;
            for (r = r1; r <= r2; r++)
                for (c = c1; c <= c2; c++) {
                    Cell *cl = cs_cell(c, r);
                    if (!cl) continue;
                    if (cl->type == CE_NUM && !cl->iserr) {
                        if (n < LISTMAX) csn_copy(&list[n++], &cl->val);
                    } else if (cl->iserr) { val_err(&rv, 0); return rv; }
                }
        } else {
            if (v.isstr) continue;         /* text is ignored in arithmetic */
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
            {
                Cell *cl;
                val_free(&v);
                cl = cs_cell(a1, b1);
                if (cl && cl->type == CE_TEXT && cl->cont && !cl->iserr) {
                    /* TEXT cell yields its content as a string value */
                    v.str = cs_strdup(cl->cont);
                    if (!v.str) { val_err(&v, 0); return v; }
                    v.isstr = 1;
                    v.good = 1;
                } else {
                    int err;
                    cellnum(&v.num, a1, b1, &err);
                    if (err) { val_err(&v, err); return v; }
                    v.good = 1;
                }
                ps->p = q;
                return v;
            }
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
            if (f == FN_PROJ || f == FN_DEPD) {
                ArgKind kind;
                int c1, r1, c2, r2;
                Val a = parse_arg(ps, &kind, &c1, &r1, &c2, &r2);
                Val t;
                int err;
                if (!a.good) return a;
                val_free(&a);
                skip_ws(&ps->p);
                if (*ps->p != ')') { val_err(&v, 3); return v; }
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
                f == FN_ROUND || f == FN_TRUNC) {
                ArgKind kind;
                int c1, r1, c2, r2;
                Val a2;
                int err;
                Val a = parse_arg(ps, &kind, &c1, &r1, &c2, &r2);
                if (!a.good) return a;
                val_free(&a);
                skip_ws(&ps->p);
                if (*ps->p != ',') { val_err(&v, 3); return v; }
                ps->p++;
                a2 = parse_arg(ps, &kind, &c1, &r1, &c2, &r2);
                if (!a2.good) { val_free(&a2); return a2; }
                val_free(&a2);
                skip_ws(&ps->p);
                if (*ps->p != ')') { val_err(&v, 3); return v; }
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
                    err = csn_comb(&v.num, &a.num, &a2.num);
                    break;
                case FN_PERMUT:
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
                ArgKind k1, k2;
                int c1, r1, c2, r2, c3, r3, c4, r4;
                Val a = parse_arg(ps, &k1, &c1, &r1, &c2, &r2);
                if (!a.good) return a;
                val_free(&a);
                skip_ws(&ps->p);
                if (*ps->p != ',') { val_err(&v, 3); return v; }
                ps->p++;
                a = parse_arg(ps, &k2, &c3, &r3, &c4, &r4);
                if (!a.good) return a;
                val_free(&a);
                skip_ws(&ps->p);
                if (*ps->p != ')') { val_err(&v, 3); return v; }
                ps->p++;
                if (k1 != AK_RANGE) { val_err(&v, 2); return v; }
                if (c1 != c2 && r1 != r2) { val_err(&v, 2); return v; }   /* straight line */
                if (k2 == AK_RANGE) { val_err(&v, 2); return v; }
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
                f == FN_SIGN || f == FN_DEGREES ||
                f == FN_DEG || f == FN_RADIANS ||
                f == FN_RAD || f == FN_FACT) {
                ArgKind kind;
                int c1, r1, c2, r2, err;
                Val a = parse_arg(ps, &kind, &c1, &r1, &c2, &r2);
                if (!a.good) return a;
                val_free(&a);
                skip_ws(&ps->p);
                if (*ps->p != ')') { val_err(&v, 3); return v; }
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
        ps->p = p + 1;
        t = parse_unary(ps);
        if (!t.good) return t;
        if (t.isstr) { val_free(&t); val_err(&t, 6); return t; }
        if (neg) csn_neg(&t.num, &t.num);
        return t;
    }
    v = parse_primary(ps);
    if (!v.good) return v;
    /* '**' power (manual 4.7): right-associative, binds tighter than
     * '*' and '/' so that 12*10**-3 is 12*(10**-3) */
    for (;;) {
        const char *p2 = ps->p;
        skip_ws(&p2);
        if (*p2 == '*' && p2[1] == '*') {
            Val b;
            int err;
            ps->p = p2 + 2;
            b = parse_unary(ps);
            if (!b.good) { val_free(&v); val_free(&b); return b; }
            if (v.isstr || b.isstr) { val_free(&v); val_free(&b); val_err(&v, 6); return v; }
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
            int err;
            if (v.isstr) { val_free(&v); val_err(&v, 6); return v; }
            err = csn_scale10(&v.num, &v.num, -2);   /* v / 100 exactly */
            if (err) { val_err(&v, 0); return v; }
            v.isbool = 0;
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
                    if (v.isstr || b.isstr) { val_free(&v); val_free(&b); val_err(&v, 6); return v; }
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
    for (;;) {
        const char *p = ps->p;
        Val b;
        int op, err;
        memset(&b, 0, sizeof(b));
        skip_ws(&p);
        if (*p == ':' && ps->nocond) break;
        if (*p != '*' && *p != '/') break;
        op = (*p == '*') ? 0 : 1;
        ps->p = p + 1;
        b = parse_unary(ps);
        if (!b.good) { val_free(&a); val_free(&b); return b; }
        if (a.isstr || b.isstr) { val_free(&a); val_free(&b); val_err(&a, 6); return a; }
        if (a.isbool && b.isbool && op == 0) {
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
    for (;;) {
        const char *p = ps->p;
        Val b;
        int op, err;
        memset(&b, 0, sizeof(b));
        skip_ws(&p);
        if (*p == ':' && ps->nocond) break;
        if (*p != '+' && *p != '-') break;
        op = *p;
        if (p[1] == '\0') { val_free(&a); val_err(&a, 7); ps->p = p + 1; return a; }
        ps->p = p + 1;
        b = parse_mul(ps);
        if (!b.good) { val_free(&a); val_free(&b); return b; }
        if (op == '+') {
            if (a.isbool && b.isbool) {
                /* logical OR */
                csn_from_int(&a.num,
                             (!csn_is_zero(&a.num) || !csn_is_zero(&b.num)) ? 1 : 0);
            } else {
                err = csn_add(&a.num, &a.num, &b.num);
                if (err) { val_free(&a); val_free(&b); val_err(&a, 0); return a; }
                a.isbool = 0;
            }
        } else {
            err = csn_sub(&a.num, &a.num, &b.num);
            if (err) { val_free(&a); val_free(&b); val_err(&a, 0); return a; }
            a.isbool = 0;
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
        int op, r;
        memset(&b, 0, sizeof(b));
        skip_ws(&p);
        if (*p == ':' && ps->nocond) return a;
        if (*p == '<') {
            if (p[1] == '=') op = 2;       /* <= */
            else if (p[1] == '>') op = 4;  /* <> */
            else op = 1;                   /* <  */
        } else if (*p == '>') op = 5;
        else if (*p == '=') op = 3;
        else return a;
        ps->p = p + ((op == 2 || op == 4) ? 2 : (op == 5 ? 1 : 1));
        b = parse_add(ps);
        if (!b.good) { val_free(&a); val_free(&b); return b; }
        if (a.isstr || b.isstr) { val_free(&a); val_free(&b); val_err(&a, 6); return a; }
        r = csn_cmp(&a.num, &b.num);
        switch (op) {
        case 1: r = (r < 0); break;
        case 2: r = (r <= 0); break;
        case 3: r = (r == 0); break;
        case 4: r = (r != 0); break;
        default: r = (r > 0); break;
        }
        csn_from_int(&a.num, r ? 1 : 0);
        a.isbool = 1;
        val_free(&b);
        return a;
    }
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
        if (*p == ':' && !ps->nocond) {
            Val t, f;
            int ctrue;
            const char *q;
            memset(&t, 0, sizeof(t));
            ps->nocond = 1;
            ps->p = p + 1;
            skip_ws(&ps->p);
            q = ps->p;
            if (*q == ':' || *q == '\0') {
                t.good = 1;   /* empty true-branch assigns 0 */
                csn_zero(&t.num);
            } else {
                t = parse_condexpr(ps);
            }
            if (!t.good) { ps->nocond = 0; val_free(&t); val_free(&cond); return t; }
            skip_ws(&ps->p);
            if (*ps->p == ':') {
                ps->p++;
                f = parse_condexpr(ps);
                if (!f.good) { ps->nocond = 0; val_free(&f); val_free(&t); val_free(&cond); return f; }
            } else {
                memset(&f, 0, sizeof(f));
                f.good = 1;                 /* missing else -> 0 */
                csn_zero(&f.num);
                f.isbool = 0;
            }
            ps->nocond = 0;
            ctrue = cond.isstr ? (cond.str && cond.str[0])
                               : !csn_is_zero(&cond.num);
            if (ctrue) { val_free(&f); val_free(&cond); return t; }
            val_free(&t); val_free(&cond);
            return f;
        }
    }
    return cond;
}

int cs_eval_formula(const char *s, Csn *val, char **str)
{
    P ps;
    Val v;
    const char *p;
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

    memset(&v, 0, sizeof(v));
    csn_zero(&v.num);
    ps.p = buf;
    ps.depth = 0;
    ps.nocond = 0;
    v = parse_condexpr(&ps);
    if (!v.good) {
        int e = v.err;
        val_free(&v);
        csn_zero(val);
        return e ? e : -1;    /* -1 == CalcStar "Error 0" (value-range) */
    }
    p = ps.p;
    while (*p == ' ') p++;
    if (*p) { val_free(&v); csn_zero(val); return 7; }        /* trailing junk */
    if (v.isstr) {
        csn_zero(val);
        if (str) *str = v.str; else free(v.str);
        return 0;
    }
    val_free(&v);
    csn_copy(val, &v.num);
    if (str) *str = NULL;
    return 0;
}
