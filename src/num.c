/* num.c - CalcStar's BCD arithmetic engine.
 *
 * Everything here works on decimal digits (BCD), one digit per byte.
 * No floating point is used anywhere; multiply and divide follow the
 * pencil-and-paper algorithms the original spreadsheet had to use.
 * The mantissa carries 40 digits (30 calculation + 10 guard) - an
 * engineering extension beyond the manual's "accurate to 14 decimal
 * places"; the display still shows at most 12 decimals and the manual's
 * truncation rule governs everything the user sees.
 */
#include <string.h>
#include <ctype.h>
#include "num.h"

#define CSN_PARSE_MAX 64     /* digit cap when parsing; beyond: overflow */

/* ================= internal helpers ================================= */

static void mkzero(Csn *r)
{
    r->sign = 0;
    r->exp10 = 0;
    r->ndig = 0;
    memset(r->nd, 0, sizeof(r->nd));
}

/* canonical builder.  The digit string d[0..n-1] is most significant
 * first and represents the exact integer mantissa M; the value equals
 * sign * M * 10^baseExp, i.e. digit d[n-1] sits at place 10^baseExp.
 * Leading zeros are skipped, trailing zeros stripped (both value-
 * preserving), the mantissa is cut to CSN_DIGS digits (truncation per
 * the manual's "numbers are ... truncated"; CSN_DIGS itself is an
 * engineering extension, see num.h) and the exponent is range-checked. */
static int finish(Csn *r, const unsigned char *d, int n, int baseExp, int sign)
{
    int k0 = 0, last, i;

    while (k0 < n && d[k0] == 0) k0++;
    if (k0 >= n) { mkzero(r); return CSN_OK; }
    last = n - k0;
    if (last > CSN_DIGS) last = CSN_DIGS;
    memcpy(r->nd, d + k0, (size_t) last);
    for (i = last; i < CSN_DIGS; i++) r->nd[i] = 0;
    while (last > 0 && r->nd[last - 1] == 0) last--;
    if (last == 0) { mkzero(r); return CSN_OK; }
    r->ndig = last;
    r->exp10 = baseExp + n - k0;
    r->sign = (sign != 0) ? 1 : 0;
    if (r->exp10 > CSN_EMAX) { mkzero(r); return CSN_ER_OVERFLOW; }
    if (r->exp10 < -CSN_EMAX) { mkzero(r); return CSN_OK; }  /* underflow */
    return CSN_OK;
}

/* the decimal digit at place 10^place (0 when the number has no digit
 * there).  Digit i (1-based) carries place exp10 - i. */
static int digit_at(const Csn *v, int place)
{
    int i;
    if (v->ndig == 0) return 0;
    i = v->exp10 - place;
    if (i >= 1 && i <= v->ndig) return v->nd[i - 1];
    return 0;
}

/* compare the magnitudes (normalized values: exponent decides first) */
static int cmp_mag(const Csn *a, const Csn *b)
{
    int k, n;
    if (a->exp10 != b->exp10) return (a->exp10 > b->exp10) ? 1 : -1;
    n = (a->ndig > b->ndig) ? a->ndig : b->ndig;
    for (k = 0; k < n; k++) {
        int da = (k < a->ndig) ? a->nd[k] : 0;
        int db = (k < b->ndig) ? b->nd[k] : 0;
        if (da != db) return (da > db) ? 1 : -1;
    }
    return 0;
}

/* compare absolute values regardless of sign */
static int cmp_abs(const Csn *a, const Csn *b)
{
    int az = a->ndig == 0, bz = b->ndig == 0;
    if (az) return bz ? 0 : -1;    /* zero is the smallest magnitude */
    if (bz) return 1;
    return cmp_mag(a, b);
}

/* r = sign * (|a| + |b|).  Column addition from the lowest relevant
 * place upward; positions below both operands' digits are all zero,
 * so they contribute nothing and no carry can come from there. */
static int mag_add(Csn *r, const Csn *a, const Csn *b, int sign)
{
    unsigned char ls[CSN_DIGS + 8], ms[CSN_DIGS + 8];
    int atop = a->exp10 - 1;
    int btop = b->exp10 - 1;
    int hi = (atop > btop) ? atop : btop;
    int lo, p, k, carry, n;

    lo = hi - (CSN_DIGS + 1);
    /* NOTE: no extension below the window is needed - every operand
     * has at most CSN_DIGS digits, so its lowest digit already lies
     * at or above hi - (CSN_DIGS - 1).  Positions below the window
     * are zero for both operands and cannot carry into it. */

    k = 0; carry = 0;
    for (p = lo; p <= hi + 1; p++, k++) {          /* +1: carry position */
        int s = digit_at(a, p) + digit_at(b, p) + carry;
        ls[k] = (unsigned char) (s % 10);
        carry = s / 10;
    }
    n = k;
    for (k = 0; k < n; k++) ms[n - 1 - k] = ls[k];
    return finish(r, ms, n, lo, sign);
}

/* r = sign * (|a| - |b|); requires cmp_mag(a, b) > 0.
 * a has at most CSN_DIGS digits, so all of a's digits lie inside the
 * kept window and a contributes nothing below it.  A borrow can only
 * originate below the window (from b); if b has any digit below, that
 * chain propagates through the zeros up into the window - modeled by
 * the borrow0 flag. */
static int mag_sub(Csn *r, const Csn *a, const Csn *b, int sign)
{
    unsigned char ls[CSN_DIGS + 8], ms[CSN_DIGS + 8];
    int atop = a->exp10 - 1;
    int hi, lo, p, k, borrow, n;

    hi = atop;
    lo = atop - (CSN_DIGS + 1);
    borrow = (b->ndig != 0 && (b->exp10 - b->ndig) < lo) ? 1 : 0;

    k = 0;
    for (p = lo; p <= hi; p++, k++) {
        int s = digit_at(a, p) - digit_at(b, p) - borrow;
        if (s < 0) { s += 10; borrow = 1; } else borrow = 0;
        ls[k] = (unsigned char) s;
    }
    n = k;
    for (k = 0; k < n; k++) ms[n - 1 - k] = ls[k];
    return finish(r, ms, n, lo, sign);
}

/* keep only the first `keep` significant digits (truncation) */
static void csn_truncate(Csn *r, const Csn *a, int keep)
{
    unsigned char ms[CSN_DIGS + 2];
    int i, n = a->ndig;
    if (n == 0) { mkzero(r); return; }
    if (n > keep) n = keep;
    for (i = 0; i < n; i++) ms[i] = a->nd[i];
    /* the last kept digit (index n) has place a->exp10 - n */
    finish(r, ms, n, a->exp10 - n, a->sign);
}

/* forward decls */
static void csn_truncate(Csn *r, const Csn *a, int keep);
static void trunc_int(Csn *r, const Csn *a);
static void round_at(Csn *r, int ndig);
static int cmp_abs(const Csn *a, const Csn *b);

void csn_zero(Csn *r)          { mkzero(r); }

void csn_copy(Csn *r, const Csn *a)
{
    if (r != a) *r = *a;
}

void csn_from_int(Csn *r, long n)
{
    unsigned char raw[24], ms[24];
    int sign = 0, k = 0, i;
    if (n < 0) { sign = 1; n = -n; }
    if (n == 0) { mkzero(r); return; }
    while (n > 0) { raw[k++] = (unsigned char) (n % 10); n /= 10; }
    /* raw is least-significant first: place of raw[0] is 0 */
    for (i = 0; i < k; i++) ms[i] = raw[k - 1 - i];
    finish(r, ms, k, 0, sign);
}

int csn_is_zero(const Csn *a)  { return a->ndig == 0; }

int csn_cmp(const Csn *a, const Csn *b)
{
    int az = (a->ndig == 0), bz = (b->ndig == 0);
    if (az && bz) return 0;
    if (az) return b->sign ? 1 : -1;
    if (bz) return a->sign ? -1 : 1;
    if (a->sign != b->sign) return a->sign ? -1 : 1;
    {
        int m = cmp_mag(a, b);
        return a->sign ? -m : m;
    }
}

int csn_add(Csn *r, const Csn *a, const Csn *b)
{
    if (a->ndig == 0 && b->ndig == 0) { mkzero(r); return CSN_OK; }
    if (a->ndig == 0) { csn_copy(r, b); return CSN_OK; }
    if (b->ndig == 0) { csn_copy(r, a); return CSN_OK; }
    if (a->sign == b->sign) return mag_add(r, a, b, a->sign);
    {
        int m = cmp_mag(a, b);
        if (m == 0) { mkzero(r); return CSN_OK; }
        if (m > 0) return mag_sub(r, a, b, a->sign);
        return mag_sub(r, b, a, b->sign);
    }
}

int csn_sub(Csn *r, const Csn *a, const Csn *b)
{
    if (b->ndig == 0) { csn_copy(r, a); return CSN_OK; }
    if (a->ndig == 0) { csn_neg(r, b); return CSN_OK; }
    if (a->sign != b->sign) return mag_add(r, a, b, a->sign);
    {
        int m = cmp_mag(a, b);
        if (m == 0) { mkzero(r); return CSN_OK; }
        if (m > 0) return mag_sub(r, a, b, a->sign);
        return mag_sub(r, b, a, 1 - a->sign);
    }
}

/* schoolbook multiplication on digit arrays (both mantissas mirrored
 * to least-significant-first before accumulating) */
int csn_mul(Csn *r, const Csn *a, const Csn *b)
{
    int aa[CSN_DIGS], bb[CSN_DIGS], res[2 * CSN_DIGS + 4];
    unsigned char ms[2 * CSN_DIGS + 8];
    int na = a->ndig, nb = b->ndig;
    int i, j, k, n, top;

    if (na == 0 || nb == 0) { mkzero(r); return CSN_OK; }
    for (i = 0; i < na; i++) aa[i] = a->nd[na - 1 - i];
    for (i = 0; i < nb; i++) bb[i] = b->nd[nb - 1 - i];
    for (i = 0; i < na + nb; i++) res[i] = 0;
    for (i = 0; i < na; i++) {
        if (aa[i] == 0) continue;
        for (j = 0; j < nb; j++) res[i + j] += aa[i] * bb[j];
    }
    for (k = 0; k < na + nb - 1; k++) {   /* carry pass, LSB upward */
        res[k + 1] += res[k] / 10;
        res[k] %= 10;
    }
    /* M = ma * mb has at most na+nb digits, top digit < 10.
     * value = M * 10^(Ea+Eb-na-nb): res[0] (units of M) sits there. */
    n = na + nb;
    top = n - 1;
    while (top > 0 && res[top] == 0) top--;    /* skip leading zeros */
    for (i = 0; i <= top; i++) ms[i] = (unsigned char) res[top - i];
    return finish(r, ms, top + 1,
                  a->exp10 + b->exp10 - na - nb,
                  a->sign ^ b->sign);
}

/* quotient-digit helpers for the streaming long division: the
 * remainder never exceeds 10^(nb+1), so nb+2 digit slots suffice */
static int rem_ge_den(const int *den, const int *rem, int nb)
{
    int i;
    for (i = nb; i >= 0; i--) {
        int dr = (i < nb) ? den[i] : 0;
        if (rem[i] != dr) return rem[i] > dr;
    }
    return 1;   /* equal: rem >= den, one more subtraction */
}

static void rem_sub_den(const int *den, int *rem, int nb)
{
    int i, borrow = 0;
    for (i = 0; i < nb; i++) {
        int s = rem[i] - den[i] - borrow;
        if (s < 0) { s += 10; borrow = 1; } else borrow = 0;
        rem[i] = s;
    }
    rem[nb] -= borrow;   /* rem >= den guaranteed: settles the top */
}

/* classic pencil-and-paper division of the mantissas: quotient digits
 * are consumed most significant first, one per consumed numerator
 * digit, the digit produced at step j sitting at place (na - j) of
 * the mantissa ratio ma_int/mb_int; a zero-padded stream continues
 * into the fraction digits */
int csn_div(Csn *r, const Csn *a, const Csn *b)
{
    int den[CSN_DIGS + 4];       /* least-significant first */
    int rem[CSN_DIGS + 4];
    unsigned char q[3 * CSN_DIGS + 16];
    int na = a->ndig, nb = b->ndig;
    int j, k, jmax, j0 = -1, qlen = 0;
    int sign = a->sign ^ b->sign;

    if (nb == 0) return CSN_ER_DIVZERO;
    if (na == 0) { mkzero(r); return CSN_OK; }
    for (k = 0; k < nb; k++) den[k] = b->nd[nb - 1 - k];
    for (k = 0; k <= nb + 1; k++) rem[k] = 0;

    /* the first nonzero quotient digit appears by step nb+2 and
     * DIGS+2 digits cover the mantissa; plenty of room here */
    jmax = ((na > nb) ? na : nb) + CSN_DIGS + 4;
    for (j = 1; j <= jmax; j++) {
        int nextdig = (j <= na) ? a->nd[j - 1] : 0;
        int qv;
        for (k = nb; k > 0; k--) rem[k] = rem[k - 1];
        rem[0] = nextdig;
        qv = 0;
        while (rem_ge_den(den, rem, nb)) {
            rem_sub_den(den, rem, nb);
            qv++;
        }
        if (qv && j0 < 0) j0 = j;
        q[qlen++] = (unsigned char) qv;
        if (j0 >= 0 && (j - j0 + 1) >= CSN_DIGS + 2) break;
    }
    if (j0 < 0) { mkzero(r); return CSN_OK; }
    {
        unsigned char ms[2 * CSN_DIGS + 8];
        int L = qlen - (j0 - 1);
        if (L > CSN_DIGS) L = CSN_DIGS;
        for (k = 0; k < L; k++) ms[k] = q[j0 - 1 + k];
        /* value = (ma_int/mb_int) * 10^(Ea - Eb - ndig_a + ndig_b);
         * q[j] sits at place (na - j) of the ratio, so the last kept
         * digit q[j0-1+L-1] has place (na - (j0+L-1)) */
        return finish(r, ms, L,
                      na - (j0 + L - 1) + (a->exp10 - b->exp10)
                                         - na + nb,
                      sign);
    }
}

int csn_neg(Csn *r, const Csn *a)
{
    csn_copy(r, a);
    if (r->ndig) r->sign = 1 - r->sign;
    return CSN_OK;
}

void csn_abs(Csn *r, const Csn *a)
{
    csn_copy(r, a);
    r->sign = 0;
}

/* exact power-of-ten shift (the percent operator uses it) */
int csn_scale10(Csn *r, const Csn *a, int k)
{
    csn_copy(r, a);
    if (r->ndig == 0) return CSN_OK;
    r->exp10 += k;
    if (r->exp10 > CSN_EMAX) { mkzero(r); return CSN_ER_OVERFLOW; }
    if (r->exp10 < -CSN_EMAX) { mkzero(r); return CSN_OK; }
    return CSN_OK;
}

/* ================= text conversion ==================================== */

/* grammar: [-+]digits[.digits][E[+-]digits]   (exact, truncated) */
int csn_parse(const char *s, Csn *r)
{
    unsigned char raw[CSN_PARSE_MAX + 4];
    int sign = 0, i = 0, n = 0, dec = 0, sawdig = 0, gotdot = 0;
    int eval = 0, esign = 1, gotE = 0;

    mkzero(r);                       /* errors leave a defined zero */
    while (s[i] == ' ') i++;
    if (s[i] == '+' || s[i] == '-') {
        sign = (s[i] == '-') ? 1 : 0;
        i++;
    }
    for (; s[i]; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            if (gotE) {
                eval = eval * 10 + (c - '0');
            } else {
                raw[n++] = (unsigned char) (c - '0');
                if (gotdot) dec++;
                sawdig = 1;
                if (n > CSN_PARSE_MAX) return CSN_ER_OVERFLOW;
            }
        } else if (c == '.' && !gotdot && !gotE) {
            gotdot = 1;
        } else if ((c == 'E' || c == 'e') && !gotE && sawdig) {
            gotE = 1;
            esign = 1;
            if (s[i + 1] == '+') { esign = 1; i++; }
            else if (s[i + 1] == '-') { esign = -1; i++; }
        } else if (c == ' ' && gotE) {
            break;                    /* trailing blanks end the number */
        } else {
            return CSN_ER_SYNTAX;      /* malformed */
        }
    }
    if (!sawdig) return CSN_ER_SYNTAX;
    if (gotE) eval *= esign;
    /* raw is already most-significant first (raw[n-1] sits at place
     * eval - dec) */
    return finish(r, raw, n, eval - dec, sign);
}

/* 14 significant digits, e.g. "+1.41421356237309E+0"; zero -> "0".
 * Used by the test suite for exact string comparison. */
void csn_canon(const Csn *v, char *buf, int bufn)
{
    int i, p;
    if (bufn < 32) { buf[0] = '\0'; return; }
    if (v->ndig == 0) { strcpy(buf, "0"); return; }
    p = 0;
    buf[p++] = v->sign ? '-' : '+';
    for (i = 0; i < 14; i++) {
        int d = (i < v->ndig) ? v->nd[i] : 0;
        if (i == 1) buf[p++] = '.';
        buf[p++] = (char) ('0' + d);
    }
    buf[p++] = 'E';
    if (v->exp10 < 0) buf[p++] = '-'; else buf[p++] = '+';
    {
        int e = (v->exp10 < 0) ? -v->exp10 : v->exp10;
        int d3[6], nd = 0;
        if (e == 0) d3[nd++] = 0;
        while (e > 0) { d3[nd++] = e % 10; e /= 10; }
        for (i = nd - 1; i >= 0; i--) buf[p++] = (char) ('0' + d3[i]);
    }
    buf[p] = '\0';
}

/* fixed-point TRUNCATED to prec decimals: "33.00", "-1.99", "0.00".
 * Mantissa digits are cut, never rounded.  If the integer part does
 * not fit bufn, an empty string is produced and the caller (which
 * knows the display width) switches to scientific form instead. */
void csn_fmt_fixed(const Csn *v, int prec, char *buf, int bufn)
{
    int lead = v->exp10 - 1;     /* place of digit 1 */
    int p = 0, i, allzero = 1, ipart, kk;

    if (bufn < 4) { buf[0] = '\0'; return; }
    if (v->ndig == 0) {
        if (prec <= 0) { strcpy(buf, "0"); return; }
        if (prec + 3 < bufn) {
            buf[0] = '0'; buf[1] = '.';
            for (i = 0; i < prec; i++) buf[2 + i] = '0';
            buf[2 + prec] = '\0';
        } else buf[0] = '\0';
        return;
    }
    if (v->sign) buf[p++] = '-';
    ipart = (lead >= 0) ? lead + 1 : 0;
    if (ipart > bufn - 4 - prec) { buf[0] = '\0'; return; }   /* too long */
    if (ipart == 0) {
        buf[p++] = '0';
    } else {
        for (i = lead; i >= 0; i--) {
            int d = digit_at(v, i);
            if (d != 0) allzero = 0;
            buf[p++] = (char) ('0' + d);
        }
    }
    if (prec > 0) {
        buf[p++] = '.';
        for (kk = 1; kk <= prec; kk++) {
            int d = digit_at(v, -kk);
            if (d != 0) allzero = 0;
            buf[p++] = (char) ('0' + d);
        }
    }
    buf[p] = '\0';
    if (allzero && v->sign) {          /* never print -0.00 */
        memmove(buf, buf + 1, (size_t) p);
    }
}

/* scientific form like "7.35E-6"; mantissa TRUNCATED to frac digits;
 * display convention d1.d2...E+-e with the value (d1.d2...)*10^e, so
 * the shown exponent is exp10-1 (the manual: 7.35E-6 for 0.735E-5) */
void csn_fmt_sci(const Csn *v, int frac, char *buf, int bufn)
{
    int p = 0, i, ee, d3[6], nd = 0, k;
    if (bufn < 10) { buf[0] = '\0'; return; }
    if (v->ndig == 0) { strcpy(buf, "0"); return; }
    if (v->sign) buf[p++] = '-';
    buf[p++] = (char) ('0' + v->nd[0]);
    if (frac > 0) {
        buf[p++] = '.';
        for (i = 1; i <= frac; i++) {
            int d = (i < v->ndig) ? v->nd[i] : 0;
            buf[p++] = (char) ('0' + d);
        }
    }
    buf[p++] = 'E';
    ee = v->exp10 - 1;
    buf[p++] = (ee < 0) ? '-' : '+';
    ee = (ee < 0) ? -ee : ee;
    if (ee == 0) d3[nd++] = 0;
    while (ee > 0) { d3[nd++] = ee % 10; ee /= 10; }
    for (k = nd - 1; k >= 0; k--) buf[p++] = (char) ('0' + d3[k]);
    buf[p] = '\0';
}

/* ============ transcendental constants and functions ================ */

static int    got_const;
static Csn    SR10;        /* sqrt(10)                    */
static Csn    LN10;        /* ln(10)                      */
static Csn    LN10H;       /* ln(10)/2                    */
static Csn    RCG_TH;      /* convergence threshold 1e-40 */
static Csn    HALF;        /* 0.5 (sqrt initial estimate) */

/* sqrt of the raw mantissas (used by ensure_const before ln exists):
 * exact same Newton algorithm as csn_sqrt but without the constant
 * hook; declared here, defined below */
static int sqrt_core(Csn *r, const Csn *a, int snap);

static void ensure_const(void)
{
    Csn ten, nine, eleven, two, z, term, sq, s, odd;
    if (got_const) return;
    csn_from_int(&ten, 10);
    csn_from_int(&nine, 9);
    csn_from_int(&eleven, 11);
    csn_from_int(&two, 2);
    csn_parse("1e-40", &RCG_TH);
    csn_parse("0.5", &HALF);

    sqrt_core(&SR10, &ten, 0);   /* internal constant: no 30-digit snap */
    /* ln(10) = 2 * sum z^(2n+1)/(2n+1), z = 9/11, computed directly so
     * nothing depends on an already-computed ln. */
    csn_div(&z, &nine, &eleven);
    mkzero(&s);
    csn_copy(&term, &z);
    csn_from_int(&odd, 1);
    for (;;) {
        Csn t;
        csn_div(&t, &term, &odd);
        csn_add(&s, &s, &t);
        csn_mul(&sq, &term, &z);
        csn_mul(&term, &sq, &z);
        csn_add(&odd, &odd, &two);
        if (csn_cmp(&term, &RCG_TH) < 0) break;
    }
    csn_mul(&LN10, &s, &two);      /* ln10 = 2 * s */
    csn_div(&LN10H, &LN10, &two);
    got_const = 1;
}

/* sqrt(x), x >= 0: Newton iteration on (y + x/y)/2.  With snap != 0 a
 * final snap guarantees the result is the greatest 30-digit value whose
 * square does not exceed x, so display truncation can never show a
 * digit above the true value (engineering extension: the snap point
 * moved from the original's 15 digits to the round30 working
 * precision).  Internal callers (atan halving, asin/acos, the SR10
 * constant) pass snap = 0: the raw Newton iterate is honest to ~1e-38
 * and carries no 30-digit granularity, which the cancellation-prone
 * ln reduction and the inverse-trig chains would otherwise amplify. */
static int sqrt_core(Csn *r, const Csn *a, int snap)
{
    Csn x, y, ynext, d, two, ulp, ysq, ypad, tmpy;
    int i;

    if (a->ndig == 0) { mkzero(r); return CSN_OK; }
    if (a->sign) return CSN_ER_DOMAIN;
    csn_from_int(&two, 2);
    csn_copy(&x, a);

    /* y0 = 0.5 * 10^ceil(est): never more than a few iterations */
    csn_scale10(&y, &HALF, (a->exp10 + 1) / 2 + 1);
    if (csn_is_zero(&y)) csn_scale10(&y, &HALF, 1);

    for (i = 0; i < 40; i++) {
        int done;
        Csn thr;
        csn_div(&ynext, &x, &y);
        if (csn_is_zero(&ynext) && !csn_is_zero(&x)) break;
        csn_add(&ynext, &ynext, &y);
        csn_div(&ynext, &ynext, &two);
        csn_sub(&d, &ynext, &y);
        if (d.sign) csn_neg(&d, &d);
        /* RELATIVE convergence test: |ynext - y| < |ynext| * 1e-40.
         * An absolute threshold (the old 1e-40) exits after ONE step
         * for tiny arguments - sqrt(1e-998) stopped at 2.5e-498
         * because every Newton delta near 1e-498 is "converged" by an
         * absolute yardstick.  ynext > 0 here (x > 0, y > 0). */
        csn_scale10(&thr, &ynext, -40);
        done = (csn_cmp(&d, &thr) < 0);
        csn_copy(&y, &ynext);
        if (done) break;
    }

    /* snap so that y*y <= x < (y+ulp)^2 at the 30th digit */
    if (!snap) { csn_copy(r, &y); return CSN_OK; }
    csn_truncate(&tmpy, &y, 30);
    csn_copy(&y, &tmpy);
    mkzero(&ulp);
    ulp.nd[0] = 1; ulp.ndig = 1; ulp.exp10 = y.exp10 - 29;
    for (i = 0; i < 4; i++) {          /* grow while (y+ulp)^2 <= x */
        csn_add(&ypad, &y, &ulp);
        csn_mul(&ysq, &ypad, &ypad);
        if (csn_cmp(&ysq, &x) <= 0) csn_copy(&y, &ypad);
        else break;
    }
    for (i = 0; i < 4; i++) {          /* shrink while y^2 > x */
        csn_mul(&ysq, &y, &y);
        if (csn_cmp(&ysq, &x) > 0) {
            csn_sub(&tmpy, &y, &ulp);
            csn_copy(&y, &tmpy);
        } else break;
    }
    csn_copy(r, &y);
    return CSN_OK;
}

int csn_sqrt(Csn *r, const Csn *a)
{
    if (a->ndig == 0) { mkzero(r); return CSN_OK; }
    if (a->sign) return CSN_ER_DOMAIN;
    ensure_const();
    return sqrt_core(r, a, 1);
}

/* e^a: range-reduce by whole ln10 steps (n counted), then a Taylor
 * series in the residual (|rem| < ln10/2 < 1.16).  e^x = e^rem * 10^n.
 * Only mul/div of small numbers, so truncation noise stays at the
 * 40th digit - far below the 30-digit working accuracy. */
int csn_exp(Csn *r, const Csn *a)
{
    Csn x, sum, term, sq, t, two, kcs;
    int nre = 0, i, k, neg, err;

    if (a->ndig == 0) { csn_from_int(r, 1); return CSN_OK; }
    ensure_const();
    neg = a->sign;
    csn_abs(&x, a);
    csn_from_int(&two, 2);

    /* x = n*ln10 + rem, with rem <= ln10/2 */
    for (i = 0; i < 1100; i++) {
        if (csn_cmp(&x, &LN10H) <= 0) break;
        csn_sub(&x, &x, &LN10);
        nre++;
        if (nre > CSN_EMAX + CSN_DIGS) return CSN_ER_OVERFLOW;
    }

    /* series: e^rem = sum rem^k / k! */
    csn_from_int(&sum, 1);
    csn_from_int(&term, 1);
    for (k = 1; k <= 120; k++) {
        unsigned char ks[10], ms[10];
        int klen = 0, kk = k;
        while (kk > 0) { ks[klen++] = (unsigned char) (kk % 10); kk /= 10; }
        for (i = 0; i < klen; i++) ms[i] = ks[klen - 1 - i];
        finish(&kcs, ms, klen, 0, 0);          /* kcs = k */
        csn_mul(&sq, &term, &x);
        err = csn_div(&term, &sq, &kcs);
        if (err != CSN_OK) return err;
        if (csn_is_zero(&term)) break;
        csn_abs(&t, &term);
        if (csn_cmp(&t, &RCG_TH) < 0 && k > 12) break;   /* tiny tail term */
        csn_add(&sum, &sum, &term);
    }
    /* e^x = sum * 10^nre */
    if (nre > 0) {
        err = csn_scale10(r, &sum, nre);
        if (err != CSN_OK) return err;
    } else {
        csn_copy(r, &sum);
    }
    if (neg) {
        Csn one;
        int e2;
        csn_from_int(&one, 1);
        e2 = csn_div(r, &one, r);   /* e^-x = 1 / e^x */
        if (e2 != CSN_OK) return e2;
    }
    round_at(r, 30);
    return CSN_OK;
}

/* ln(m) for m in [1,inf) via atanh series ln((1+z)/(1-z)), z = (m-1)/(m+1);
 * returns ln(m) through s_out.  Fast only for m below ~4; the callers
 * reduce first. */
static int atanh_series(Csn *s_out, const Csn *m)
{
    Csn one, mp1, mm1, z, term, sq, two, s, odd;
    int err;
    csn_from_int(&one, 1);
    csn_from_int(&two, 2);
    csn_add(&mp1, m, &one);
    csn_sub(&mm1, m, &one);
    err = csn_div(&z, &mm1, &mp1);
    if (err != CSN_OK) return err;
    mkzero(&s);
    csn_copy(&term, &z);
    csn_from_int(&odd, 1);
    for (;;) {
        Csn t;
        err = csn_div(&t, &term, &odd);
        if (err != CSN_OK) return err;
        csn_add(&s, &s, &t);
        csn_mul(&sq, &term, &z);
        csn_mul(&term, &sq, &z);
        csn_add(&odd, &odd, &two);
        if (csn_cmp(&term, &RCG_TH) < 0) break;
    }
    return csn_mul(s_out, &s, &two);   /* ln(m) = 2 * s */
}

/* ln(x) for x > 0: write x = 0.nd * 10^E, m = x * 10^(1-E) in [1,10),
 * reduce m below sqrt(10) (the exponent picks up a half-step), then
 * ln(x) = ln(m) + K * ln10. */
int csn_ln(Csn *r, const Csn *a)
{
    Csn m, lnm, kp, part, res;
    long k;
    int err;

    if (a->ndig == 0 || a->sign) return CSN_ER_DOMAIN;
    ensure_const();
    csn_scale10(&m, a, 1 - a->exp10);
    if (csn_is_zero(&m)) return CSN_ER_DOMAIN;   /* safety */
    k = a->exp10 - 1;
    if (csn_cmp(&m, &SR10) >= 0) {
        err = csn_div(&m, &m, &SR10);
        if (err != CSN_OK) return err;
        err = atanh_series(&lnm, &m);
        if (err != CSN_OK) return err;
        csn_add(&res, &lnm, &LN10H);   /* the half exponent step */
        /* NOTE: k (whole steps) is unchanged - the half step is
         * already accounted for by adding LN10H */
    } else {
        err = atanh_series(&lnm, &m);
        if (err != CSN_OK) return err;
        csn_copy(&res, &lnm);
    }
    if (k != 0) {
        csn_from_int(&kp, k);
        err = csn_mul(&part, &kp, &LN10);
        if (err != CSN_OK) return err;
        err = csn_add(r, &res, &part);
    } else {
        csn_copy(r, &res);
        err = CSN_OK;
    }
    if (err == CSN_OK) round_at(r, 30);
    return err;
}

/* log10(x) directly: result = (E-1) + log10(m), m in [1,10).  The
 * integer part is exact; only the small fraction ln(m)/ln10 is
 * truncated, so integral powers of ten come out exactly. */
int csn_log10(Csn *r, const Csn *a)
{
    Csn m, lnm, lgm, kp, res;
    long k;
    int err;

    if (a->ndig == 0 || a->sign) return CSN_ER_DOMAIN;
    ensure_const();
    csn_scale10(&m, a, 1 - a->exp10);
    if (csn_is_zero(&m)) return CSN_ER_DOMAIN;
    k = a->exp10 - 1;
    err = atanh_series(&lnm, &m);
    if (err != CSN_OK) return err;
    err = csn_div(&lgm, &lnm, &LN10);
    if (err != CSN_OK) return err;
    csn_copy(&res, &lgm);
    if (k != 0) {
        csn_from_int(&kp, k);
        err = csn_add(&res, &lgm, &kp);
        if (err != CSN_OK) return err;
    }
    csn_copy(r, &res);
    round_at(r, 30);
    return CSN_OK;
}

/* ==================== powers and trigonometry ======================== */

/* round the result at the `ndig`-th significant digit (half-up).  The
 * manual promises internal accuracy of 14 places; multi-term BCD
 * series carry a small systematic truncation bias, and rounding the
 * final value keeps the working digits honest.  The round point is an
 * engineering extension: 30 honest digits instead of the original 15,
 * so long round-trip chains keep their digits; the INVERSE trig
 * functions (atan/asin/acos) round at 38 - their results sit near
 * pi/2 where tan(x) amplifies the published granularity by 1/x, and
 * their series bias is ~0.03 ulp at 38, so boundary values still snap
 * exactly (ATAN(1) = pi/4).  The cell display still always truncates,
 * per the manual. */
static void round_at(Csn *r, int ndig)
{
    unsigned char ms[CSN_DIGS + 2];
    int i, n, carry;
    if (r->ndig <= ndig) return;    /* nothing beyond the kept digits */
    n = ndig;
    for (i = 0; i < n; i++) ms[i] = r->nd[i];
    if (r->nd[ndig] >= 5) {
        carry = 1;
        for (i = n - 1; i >= 0 && carry; i--) {
            int d = ms[i] + carry;
            if (d == 10) { ms[i] = 0; carry = 1; }
            else { ms[i] = (unsigned char) d; carry = 0; }
        }
        if (carry) {                /* 0.99999... -> 1.0 */
            memmove(ms + 1, ms, (size_t) n);
            ms[0] = 1;
            n++;
        }
    }
    /* ms[0..n-1] = the first n significant digits; digit i carries
     * place exp10 - i, so the last one sits at exp10 - ndig */
    finish(r, ms, n, r->exp10 - ndig, r->sign);
}

/* is the value exactly an integer small enough to handle in a long? */
static int csn_int_val(const Csn *a, long *k)
{
    long v = 0;
    int i, n, sign = 1;
    if (a->ndig == 0) { *k = 0; return 1; }
    if (a->exp10 < a->ndig) return 0;   /* a digit sits below the units */
    if (a->exp10 > 15) return 0;        /* too large for the fast path */
    n = a->exp10;                       /* number of integer digits */
    if (a->sign) sign = -1;
    for (i = 0; i < n; i++)
        v = v * 10 + ((i < a->ndig) ? a->nd[i] : 0);
    *k = v * sign;
    return 1;
}

/* a ** b (manual 4.7 / 14-35): integer exponents by repeated exact
 * multiplication; other exponents via exp(y*ln(x)).  The manual
 * (14-35) notes even the original's results may be off "up to .1". */
int csn_pow(Csn *r, const Csn *a, const Csn *b)
{
    long k;
    int err;

    if (csn_is_zero(b)) { csn_from_int(r, 1); return CSN_OK; }
    if (csn_is_zero(a)) {
        if (b->sign) return CSN_ER_DIVZERO;     /* 0 ** negative */
        mkzero(r);
        return CSN_OK;                          /* 0 ** positive = 0 */
    }
    if (csn_int_val(b, &k)) {
        Csn acc;
        int i, n, neg;
        if (k < 0) {
            Csn pos;
            if (a->sign && !csn_is_zero(a)) {
                /* negative base, negative integer exponent: fine */
            }
            csn_neg(&pos, b);
            err = csn_pow(r, a, &pos);
            if (err != CSN_OK) return err;
            {
                Csn one;
                csn_from_int(&one, 1);
                return csn_div(r, &one, r);
            }
        }
        if (k > 64) {
            /* huge integer exponent: exp path will overflow out */
            Csn t;
            if (!csn_is_zero(a) && a->sign) return CSN_ER_DOMAIN;
            err = csn_ln(&t, a);
            if (err != CSN_OK) return err;
            err = csn_mul(&t, &t, b);
            if (err != CSN_OK) return err;
            return csn_exp(r, &t);
        }
        /* repeated multiplication */
        csn_from_int(&acc, 1);
        n = (int) k;
        neg = a->sign;
        for (i = 0; i < n; i++) {
            err = csn_mul(&acc, &acc, a);
            if (err != CSN_OK) return err;
        }
        /* fix the sign for a negative base: odd k keeps the sign */
        csn_copy(r, &acc);
        if (!csn_is_zero(r) && neg && (n % 2 == 1)) r->sign = 1;
        return CSN_OK;
    }
    /* fractional exponent: needs a nonnegative base */
    if (a->sign) return CSN_ER_DOMAIN;
    err = csn_ln(r, a);
    if (err != CSN_OK) return err;
    err = csn_mul(r, r, b);
    if (err != CSN_OK) return err;
    err = csn_exp(r, r);
    if (err != CSN_OK) return err;
    round_at(r, 30);
    return CSN_OK;
}

/* -------------- trigonometry (degrees, in and out) ------------------- */

static int ser_sin_rad(Csn *r, const Csn *rad)
{
    Csn sq, term, sum, odd, th;
    int k, err;
    /* sin(x) = x - x^3/3! + x^5/5! - ... ; each step multiplies by
     * x^2 and negates, so the sign alternates (|x| <= pi/2 here) */
    err = csn_mul(&sq, rad, rad);            /* x^2 (positive) */
    if (err != CSN_OK) return err;
    csn_copy(&sum, rad);
    err = csn_mul(&term, rad, rad);          /* term starts at x^2 */
    if (err != CSN_OK) return err;
    err = csn_mul(&term, &term, rad);        /* then x^3, sign added in loop */
    if (err != CSN_OK) return err;
    csn_parse("1e-40", &th);
    for (k = 1; k <= 40; k++) {
        csn_from_int(&odd, (2 * k) * (2 * k + 1));
        err = csn_div(&term, &term, &odd);   /* /(2k)(2k+1) */
        if (err != CSN_OK) return err;
        csn_neg(&term, &term);               /* alternate sign */
        if (cmp_abs(&term, &th) < 0) break;
        err = csn_add(&sum, &sum, &term);
        if (err != CSN_OK) return err;
        err = csn_mul(&term, &term, &sq);    /* x^2 for the next term */
        if (err != CSN_OK) return err;
    }
    csn_copy(r, &sum);
    return CSN_OK;
}

static int ser_cos_rad(Csn *r, const Csn *rad)
{
    Csn sq, term, sum, odd, th;
    int k, err;
    /* cos(x) = 1 - x^2/2! + x^4/4! - ... ; |x| <= pi/2 here */
    err = csn_mul(&sq, rad, rad);
    if (err != CSN_OK) return err;
    csn_from_int(&sum, 1);
    csn_from_int(&term, 1);
    csn_parse("1e-40", &th);
    for (k = 1; k <= 40; k++) {
        csn_from_int(&odd, (2 * k - 1) * (2 * k));
        err = csn_mul(&term, &term, &sq);
        if (err != CSN_OK) return err;
        err = csn_div(&term, &term, &odd);
        if (err != CSN_OK) return err;
        csn_neg(&term, &term);
        if (cmp_abs(&term, &th) < 0) break;
        err = csn_add(&sum, &sum, &term);
        if (err != CSN_OK) return err;
    }
    csn_copy(r, &sum);
    return CSN_OK;
}

/* reduce y := y - k*m so that 0 <= y < m (any magnitudes; chunked) */static void mod_pos(Csn *y, const Csn *m)
{
    Csn chunk;
    int t;
    while (csn_cmp(y, m) >= 0) {
        t = y->exp10 - m->exp10 - 1;
        if (t < 0) break;
        csn_scale10(&chunk, m, t);
        /* shrink the chunk until <= y */
        while (csn_cmp(&chunk, y) > 0)
            csn_scale10(&chunk, &chunk, -1);
        if (csn_is_zero(&chunk)) break;
        while (csn_cmp(y, &chunk) >= 0)
            csn_sub(y, y, &chunk);
    }
}

static Csn PI, D2R;                 /* pi and pi/180 (DEG/RAD converters) */
static Csn PIH, PI2;                /* pi/2 and 2*pi (angle reduction) */
static int   got_trig;

static void ensure_trig_set(void)
{
    Csn two_tmp;
    if (got_trig) return;
    ensure_const();
    csn_parse("3.14159265358979323846264338327950288419716939937510582097494459", &PI);
    csn_from_int(&D2R, 180);
    csn_div(&D2R, &PI, &D2R);          /* pi/180 */
    csn_from_int(&two_tmp, 2);
    csn_copy(&PIH, &PI);
    csn_div(&PIH, &PIH, &two_tmp);     /* pi/2 */
    csn_add(&PI2, &PI, &PI);           /* 2*pi (exact doubling) */
    got_trig = 1;
}

/* sin/cos at a reduced angle: rad in [0,pi/2) */
static int ser_at90(Csn *r, const Csn *rad, int want_cos)
{
    return want_cos ? ser_cos_rad(r, rad) : ser_sin_rad(r, rad);
}

/* trigonometry in RADIANS (user decision 2026-11; the trig set is not
 * documented in the manuals, and the standard precision suites assume
 * radians - the earlier degree convention was a flagged assumption).
 * DEG()/RAD()/DEGREES()/RADIANS() convert for degree-based work. */

int csn_sin(Csn *r, const Csn *rad)
{
    Csn y;
    int neg = 0, err;

    ensure_trig_set();
    csn_copy(&y, rad);
    if (y.sign && !csn_is_zero(&y)) { csn_neg(&y, &y); neg = 1; }
    mod_pos(&y, &PI2);              /* y in [0,2*pi) */
    if (csn_cmp(&y, &PI) >= 0) {
        csn_sub(&y, &y, &PI);
        neg = !neg;                 /* sin(x + pi) = -sin(x) */
    }
    if (csn_cmp(&y, &PIH) >= 0) {
        csn_sub(&y, &y, &PIH);      /* sin(pi/2 + a) = cos(a) */
        err = ser_at90(r, &y, 1);
    } else {
        err = ser_at90(r, &y, 0);
    }
    if (err != CSN_OK) return err;
    round_at(r, 30);
    if (neg && !csn_is_zero(r)) csn_neg(r, r);
    return CSN_OK;
}

int csn_cos(Csn *r, const Csn *rad)
{
    Csn y;
    int neg = 0, err;

    ensure_trig_set();
    csn_copy(&y, rad);
    /* cos is an EVEN function: a negative input flips nothing (unlike
     * sin).  Only the pi / pi/2 reductions may negate. */
    if (y.sign && !csn_is_zero(&y)) csn_neg(&y, &y);
    mod_pos(&y, &PI2);              /* y in [0,2*pi) */
    if (csn_cmp(&y, &PI) >= 0) {
        csn_sub(&y, &y, &PI);
        neg = !neg;                 /* cos(x + pi) = -cos(x) */
    }
    if (csn_cmp(&y, &PIH) >= 0) {
        csn_sub(&y, &y, &PIH);      /* cos(pi/2 + a) = -sin(a) */
        neg = !neg; err = ser_at90(r, &y, 0);
    } else {
        err = ser_at90(r, &y, 1);
    }
    if (err != CSN_OK) return err;
    round_at(r, 30);
    if (neg && !csn_is_zero(r)) csn_neg(r, r);
    return CSN_OK;
}

int csn_tan(Csn *r, const Csn *rad)
{
    Csn s, c;
    int err;

    ensure_trig_set();
    err = csn_sin(&s, rad);
    if (err != CSN_OK) return err;
    err = csn_cos(&c, rad);
    if (err != CSN_OK) return err;
    if (csn_is_zero(&c)) return CSN_ER_DIVZERO;
    err = csn_div(r, &s, &c);
    if (err != CSN_OK) return err;
    round_at(r, 30);
    return CSN_OK;
}

int csn_asin(Csn *r, const Csn *x)
{
    Csn one, xsq, omx, t, ax, s;
    int err;

    ensure_trig_set();
    csn_from_int(&one, 1);
    csn_abs(&ax, x);
    if (csn_cmp(&ax, &one) > 0) return CSN_ER_DOMAIN;
    err = csn_mul(&xsq, x, x);
    if (err != CSN_OK) return err;
    err = csn_sub(&omx, &one, &xsq);
    if (err != CSN_OK) return err;
    err = sqrt_core(&s, &omx, 0);   /* unsnapped: precision, not display */
    if (err != CSN_OK) return err;
    if (csn_is_zero(&s)) {         /* x = +1 or -1: pi/2 / -pi/2 */
        csn_copy(r, &PIH);
        if (x->sign) csn_neg(r, r);
        return CSN_OK;
    }
    err = csn_div(&t, x, &s);
    if (err != CSN_OK) return err;
    err = csn_atan(r, &t);
    if (err != CSN_OK) return err;
    round_at(r, 38);    /* inverse trig: publish past the tan blow-up */
    return CSN_OK;
}

int csn_acos(Csn *r, const Csn *x)
{
    Csn one, two, omx, ax, y, as;
    int err;

    ensure_trig_set();
    csn_from_int(&one, 1);
    csn_from_int(&two, 2);
    csn_abs(&ax, x);
    if (csn_cmp(&ax, &one) > 0) return CSN_ER_DOMAIN;
    /* acos(x) = 2*asin(sqrt((1-x)/2))  (exact identity, x in [-1,1]).
     * The atan form "pi/2 - asin(x)" subtracts two nearly equal values
     * for x near 1 and loses the resolution of the small remainder.
     * The half-angle form keeps (1-x)'s digits. */
    err = csn_sub(&omx, &one, x);
    if (err != CSN_OK) return err;
    err = csn_div(&omx, &omx, &two);
    if (err != CSN_OK) return err;
    err = sqrt_core(&y, &omx, 0);   /* unsnapped: precision, not display */
    if (err != CSN_OK) return err;
    err = csn_asin(&as, &y);
    if (err != CSN_OK) return err;
    err = csn_add(r, &as, &as);
    if (err != CSN_OK) return err;
    round_at(r, 38);    /* inverse trig: publish past the tan blow-up */
    return CSN_OK;
}

int csn_atan(Csn *r, const Csn *x)
{
    Csn t, s, term, sq, th, one, half, inv, res;
    int k, n, err, neg, comp;

    ensure_trig_set();
    csn_from_int(&one, 1);
    csn_parse("0.5", &half);
    neg = (x->sign && !csn_is_zero(x)) ? 1 : 0;
    csn_abs(&t, x);                 /* atan(-x) = -atan(x) */
    /* argument reduction, step 1: the complement identity
     *    atan(x) = pi/2 - atan(1/x)   for x > 1
     * One division puts the argument below 1, and for large x the tiny
     * atan(1/x) is resolved by a 2-term series (x^3/3 already ~1e-25
     * for x ~ 1e8) instead of the halving chain - 1/x carries its full
     * 40 digits, so the pi/2-deficit keeps them after the subtraction. */
    comp = (csn_cmp(&t, &one) > 0);
    if (comp) {
        err = csn_div(&inv, &one, &t);
        if (err != CSN_OK) return err;
        csn_copy(&t, &inv);
    }
    /* argument reduction, step 2: the halving identity
     *    atan(x) = 2*atan(x/(1+sqrt(1+x^2)))
     * halve while |t| >= 0.5 so the series always converges FAST:
     * stopping only below 1 (possible with |t| ~ 0.997 for x just
     * above 1) leaves terms contracting by t^2 ~ 0.995 -> 200 terms
     * are NOT enough and the result drifts.  Each halving is an exact
     * identity, so repeating it is harmless precision-wise */
    {
        Csn half;
        csn_parse("0.5", &half);
        for (n = 0; n < 16 && cmp_abs(&t, &half) >= 0; n++) {
            Csn d, t2, sq;
            err = csn_mul(&t2, &t, &t);
            if (err != CSN_OK) return err;
            err = csn_add(&sq, &one, &t2);
            if (err != CSN_OK) return err;
            err = sqrt_core(&d, &sq, 0);   /* unsnapped (halving identity) */
            if (err != CSN_OK) return err;
            err = csn_add(&d, &d, &one);
            if (err != CSN_OK) return err;
            err = csn_div(&t, &t, &d);
            if (err != CSN_OK) return err;
        }
    }
    /* atan(t) = t - t^3/3 + t^5/5 - ...  (|t| <= 1) */
    err = csn_mul(&sq, &t, &t);
    if (err != CSN_OK) return err;
    csn_copy(&s, &t);
    csn_copy(&term, &t);
    csn_parse("1e-40", &th);
    {
        Csn prev, odd;
        csn_from_int(&prev, 1);      /* prev odd: (2k-1) of the last step */
        for (k = 1; k <= 200; k++) {
            err = csn_mul(&term, &term, &sq);        /* t^2 */
            if (err != CSN_OK) return err;
            err = csn_mul(&term, &term, &prev);      /* *(2k-1) */
            if (err != CSN_OK) return err;
            csn_from_int(&odd, 2 * k + 1);
            err = csn_div(&term, &term, &odd);       /* /(2k+1) */
            if (err != CSN_OK) return err;
            csn_neg(&term, &term);
            csn_copy(&prev, &odd);
            if (cmp_abs(&term, &th) < 0) break;
            err = csn_add(&s, &s, &term);
            if (err != CSN_OK) return err;
        }
    }
    /* undo the halvings: multiply by 2^n (exact doublings) */
    for (k = 0; k < n; k++) {
        err = csn_add(&s, &s, &s);
        if (err != CSN_OK) return err;
    }
    if (comp) {
        err = csn_sub(&res, &PIH, &s);   /* pi/2 - atan(1/x) */
        if (err != CSN_OK) return err;
        csn_copy(&s, &res);
    }
    csn_copy(r, &s);
    round_at(r, 38);    /* inverse trig: publish past the tan blow-up */
    if (neg && !csn_is_zero(r)) csn_neg(r, r);
    return CSN_OK;
}

/* pi at maximum internal precision (the same constant the trig
 * functions use for D2R, so everything stays self-consistent) */
void csn_pi(Csn *r)
{
    ensure_trig_set();
    csn_copy(r, &PI);
}

/* ===== Batch A: light algebra (sign, mod, deg/rad, fact/combin, gcd,
 * ===== lcm, round/trunc)
 */

int csn_sign(Csn *r, const Csn *a)
{
    if (csn_is_zero(a)) { mkzero(r); return CSN_OK; }
    mkzero(r);
    r->nd[0] = 1;
    r->ndig = 1;
    r->exp10 = 1;               /* value 1 = 0.1*10^1 */
    r->sign = a->sign;
    return CSN_OK;
}

int csn_mod(Csn *r, const Csn *a, const Csn *b)
{
    Csn q, pr;
    int err;

    if (csn_is_zero(b)) return CSN_ER_DIVZERO;
    if (csn_is_zero(a)) { mkzero(r); return CSN_OK; }
    err = csn_div(&q, a, b);
    if (err != CSN_OK) return err;
    trunc_int(&q, &q);                /* quotient -> integer part */
    err = csn_mul(&pr, &q, b);        /* b * int(trunc(a/b)) */
    if (err != CSN_OK) return err;
    err = csn_sub(r, a, &pr);         /* a - b*trunc(a/b) */
    if (err != CSN_OK) return err;
    return CSN_OK;
}

/* radians -> degrees and back; both derive from the shared PI so every
 * conversion is self-consistent.  Boundary-exact values (DEGREES(PI())
 * = 180) come out clean through round30. */
static Csn RAD2DEG;             /* 180/pi, prepared with ensure_trig */

int csn_degrees(Csn *r, const Csn *rad)
{
    int err;
    ensure_trig_set();
    if (!RAD2DEG.ndig) {               /* prepare 180/pi once */
        Csn d180;
        csn_from_int(&d180, 180);
        err = csn_div(&RAD2DEG, &d180, &PI);
        if (err != CSN_OK) return err;
    }
    err = csn_mul(r, rad, &RAD2DEG);
    if (err != CSN_OK) return err;
    round_at(r, 30);
    return CSN_OK;
}

int csn_radians(Csn *r, const Csn *deg)
{
    int err;
    ensure_trig_set();                 /* D2R = pi/180 */
    err = csn_mul(r, deg, &D2R);
    if (err != CSN_OK) return err;
    round_at(r, 30);
    return CSN_OK;
}

/* ---- factorial / combinatorics: integer path only ------------------- *
 * n >= 25 overflows the 22-digit mantissa (25! = 1.55e25 -> the value
 * digits overflow regardless of exponent), so the branch maps to the
 * value-range error.  Inputs that are not exact integers also fail. */

#define FACT_LIM 25

/* exact integer small enough for the long-math combinatorics */
static int int_arg(const Csn *a, long *k)
{
    if (!csn_int_val(a, k)) return 0;
    if (*k > 10000000L || *k < -10000000L) return 0;
    return 1;
}

/* the Gamma function via the Lanczos approximation (g = 7, 9 terms)
 * for the FRACTIONAL factorial (user-ext 2026-11-13): FACT(x) =
 * Gamma(x+1) for x > 0.  The coefficients are given to 16+ digits, so
 * the result carries ~15 correct digits - a documented limit (the
 * display's 14 digits are safe).  The argument z must be > 0.5. */
static const char *lanc_p[9] = {
    "0.99999999999980993",
    "676.5203681218851",
    "-1259.1392167224028",
    "771.32342877765313",
    "-176.61502916214059",
    "12.507343278686905",
    "-0.13857109526572012",
    "0.0000099843695780195716",
    "0.00000015056327351493116"
};

static int gamma_lanczos(Csn *r, const Csn *z)
{
    Csn one, two, half, seven5, zz, A, t, negt, ex, pw, s2pi, two_pi, pi, sum, term;
    int i, err;

    csn_from_int(&one, 1);
    csn_from_int(&two, 2);
    csn_zero(&half);
    if (csn_parse("0.5", &half) != CSN_OK) return CSN_ER_SYNTAX;
    csn_zero(&seven5);
    if (csn_parse("7.5", &seven5) != CSN_OK) return CSN_ER_SYNTAX;

    /* zz = z - 1 */
    err = csn_sub(&zz, z, &one);
    if (err) return err;
    /* A = p0 + sum p_k / (zz + k) */
    if (csn_parse(lanc_p[0], &A) != CSN_OK) return CSN_ER_SYNTAX;
    for (i = 1; i <= 8; i++) {
        Csn kk, den;
        csn_from_int(&kk, i);
        err = csn_add(&den, &zz, &kk);
        if (err) return err;
        if (csn_parse(lanc_p[i], &term) != CSN_OK) return CSN_ER_SYNTAX;
        err = csn_div(&term, &term, &den);
        if (err) return err;
        err = csn_add(&A, &A, &term);
        if (err) return err;
    }
    /* t = zz + 7.5 */
    err = csn_add(&t, &zz, &seven5);
    if (err) return err;
    /* Gamma = sqrt(2*pi) * t^(zz+0.5) * exp(-t) * A */
    err = csn_add(&sum, &zz, &half);
    if (err) return err;
    err = csn_pow(&pw, &t, &sum);
    if (err) return err;
    err = csn_neg(&negt, &t);
    if (err) return err;
    err = csn_exp(&ex, &negt);
    if (err) return err;
    csn_pi(&pi);
    if (err) return err;
    err = csn_mul(&two_pi, &two, &pi);
    if (err) return err;
    err = csn_sqrt(&s2pi, &two_pi);
    if (err) return err;
    err = csn_mul(r, &s2pi, &pw);
    if (err) return err;
    err = csn_mul(r, r, &ex);
    if (err) return err;
    err = csn_mul(r, r, &A);
    if (err) return err;
    return CSN_OK;
}

int csn_fact(Csn *r, const Csn *n)
{
    long k, i;
    Csn acc, iv, one, z;
    int err;
    if (csn_is_zero(n)) { csn_from_int(r, 1); return CSN_OK; }
    if (n->sign) return CSN_ER_DOMAIN;
    if (!int_arg(n, &k)) {
        /* the FRACTIONAL factorial: FACT(x) = Gamma(x+1) for x > 0
         * (user-ext 2026-11-13, the Lanczos approximation) */
        csn_from_int(&one, 1);
        err = csn_add(&z, n, &one);
        if (err) return err;
        return gamma_lanczos(r, &z);
    }
    if (k >= FACT_LIM) return CSN_ER_DOMAIN;
    csn_from_int(&acc, 1);
    for (i = 2; i <= k; i++) {
        int err;
        csn_from_int(&iv, i);
        err = csn_mul(&acc, &acc, &iv);
        if (err != CSN_OK) return err;
    }
    if (!csn_is_zero(&acc)) { }
    csn_copy(r, &acc);
    return CSN_OK;
}

int csn_comb(Csn *r, const Csn *n, const Csn *k)
{
    long kn, kk, i;
    Csn acc, iv;
    if (csn_is_zero(n)) {
        /* 0C0 = 1, 0Ck>0 = 0 */
        if (csn_is_zero(k)) { csn_from_int(r, 1); return CSN_OK; }
        if (k->sign) { mkzero(r); return CSN_OK; }
        return CSN_ER_DOMAIN;
    }
    if (!int_arg(n, &kn) || !int_arg(k, &kk)) return CSN_ER_DOMAIN;
    if (kk < 0 || kk > kn) { mkzero(r); return CSN_OK; }   /* nCk = 0 */
    if (kn >= FACT_LIM || kk >= FACT_LIM) return CSN_ER_DOMAIN;
    /* nCk = n!/(k!(n-k)!) computed as a falling product, keeps small */
    csn_from_int(&acc, 1);
    for (i = 1; i <= kk; i++) {
        int err;
        csn_from_int(&iv, kn - kk + i);
        err = csn_mul(&acc, &acc, &iv);
        if (err != CSN_OK) return err;
        csn_from_int(&iv, i);
        err = csn_div(&acc, &acc, &iv);
        if (err != CSN_OK) return err;
    }
    csn_copy(r, &acc);
    return CSN_OK;
}

int csn_perm(Csn *r, const Csn *n, const Csn *k)
{
    long kn, kk, i;
    Csn acc, iv;
    if (csn_is_zero(n)) {
        if (csn_is_zero(k)) { csn_from_int(r, 1); return CSN_OK; }
        if (k->sign) { mkzero(r); return CSN_OK; }
        return CSN_ER_DOMAIN;
    }
    if (!int_arg(n, &kn) || !int_arg(k, &kk)) return CSN_ER_DOMAIN;
    if (kk < 0 || kk > kn) { mkzero(r); return CSN_OK; }
    if (kn >= FACT_LIM || kk >= FACT_LIM) return CSN_ER_DOMAIN;
    csn_from_int(&acc, 1);
    for (i = 0; i < kk; i++) {
        int err;
        csn_from_int(&iv, kn - i);
        err = csn_mul(&acc, &acc, &iv);
        if (err != CSN_OK) return err;
    }
    csn_copy(r, &acc);
    return CSN_OK;
}

/* Euclid on the integer path; wide inputs (>~12 digits) would overflow
 * the long-math walk, so they map to the value-range error */
static long long_gcd(long a, long b)
{
    while (b) { long t = a % b; a = b; b = t; }
    return a < 0 ? -a : a;
}

static int gcd_err(const Csn *a)
{
    /* csn_int_val caps at 15 integer digits; longs hold 18 - use 12 to
     * keep LCM products safely inside the mantissa */
    long k;
    if (!csn_int_val(a, &k)) return 1;
    if (a->exp10 > 12) return 1;
    return 0;
}

int csn_gcd(Csn *r, const Csn *a, const Csn *b)
{
    long ka, kb;
    if (csn_is_zero(a) && csn_is_zero(b)) return CSN_ER_DOMAIN;
    if (gcd_err(a) || gcd_err(b) || !int_arg(a, &ka) || !int_arg(b, &kb))
        return CSN_ER_DOMAIN;
    {
        long g = long_gcd(ka, kb);
        csn_from_int(r, g ? g : 1);
    }
    return CSN_OK;
}

int csn_lcm(Csn *r, const Csn *a, const Csn *b)
{
    long ka, kb, g, prod, l;
    Csn lv;
    if (csn_is_zero(a) || csn_is_zero(b)) { mkzero(r); return CSN_OK; }
    if (gcd_err(a) || gcd_err(b) || !int_arg(a, &ka) || !int_arg(b, &kb))
        return CSN_ER_DOMAIN;
    g = long_gcd(ka, kb);
    if (g == 0) return CSN_ER_DOMAIN;
    prod = (ka / g) * kb;              /* may exceed long: map to overflow */
    if (prod < 0) return CSN_ER_OVERFLOW;
    l = prod;
    if (l > 999999999999L || l < -999999999999L) return CSN_ER_OVERFLOW;
    csn_from_int(&lv, l < 0 ? -l : l);
    csn_copy(r, &lv);
    return CSN_OK;
}

/* truncate to the integer part (drop every digit below place 0);
 * 20.49 -> 20, -1.9 -> -1, 0.9 -> 0 */
static void trunc_int(Csn *r, const Csn *a)
{
    unsigned char ms[CSN_DIGS + 2];
    int i, n;
    if (a->ndig == 0) { mkzero(r); return; }
    if (a->exp10 <= 0) { mkzero(r); return; }     /* |a| < 1 */
    n = a->exp10;
    if (n > CSN_DIGS) n = CSN_DIGS;
    for (i = 0; i < n; i++) ms[i] = a->nd[i];
    finish(r, ms, n, 0, a->sign);
}

/* round / truncate to d decimal places; d may be negative (tens,
 * hundreds, ...).  These are user-called functions - the manual's
 * "truncated, never rounded" rule is about the DISPLAY, the calculator
 * rounding here is explicit and requested.  d outside +-16 is a value
 * range error like the Format command. */
static int dec_arg(const Csn *a, int *d)   /* the digit-count argument */
{
    long k;
    if (!csn_int_val(a, &k)) return 0;
    if (k > 16 || k < -16) return 0;
    *d = (int) k;
    return 1;
}

/* x * 10^shift, life-safe through scale10 (which zeroes on underflow) */
int csn_trunc(Csn *r, const Csn *x, const Csn *d)
{
    Csn scaled;
    int dd, err;
    if (!dec_arg(d, &dd)) return CSN_ER_DOMAIN;
    if (dd == 0) { trunc_int(r, x); return CSN_OK; }
    err = csn_scale10(&scaled, x, dd);
    if (err != CSN_OK) return err;
    trunc_int(&scaled, &scaled);
    err = csn_scale10(r, &scaled, -dd);
    if (err != CSN_OK) return err;
    return CSN_OK;
}

int csn_round(Csn *r, const Csn *x, const Csn *d)
{
    Csn scaled, acc;
    int dd, err;

    if (!dec_arg(d, &dd)) return CSN_ER_DOMAIN;
    ensure_const();                    /* HALF */

    if (dd == 0) {
        /* nearest integer: add 0.5 to the magnitude, then truncate */
        csn_abs(&scaled, x);
        err = csn_add(&acc, &scaled, &HALF);
        if (err != CSN_OK) return err;
        trunc_int(&acc, &acc);
        if (x->sign) csn_neg(&acc, &acc);
        csn_copy(r, &acc);
        return CSN_OK;
    }
    /* scale up, round |scaled| to the nearest integer, scale back */
    err = csn_scale10(&scaled, x, dd);
    if (err != CSN_OK) return err;
    csn_abs(&acc, &scaled);
    err = csn_add(&acc, &acc, &HALF);
    if (err != CSN_OK) return err;
    trunc_int(&acc, &acc);
    if (scaled.sign) csn_neg(&acc, &acc);
    err = csn_scale10(r, &acc, -dd);
    if (err != CSN_OK) return err;
    return CSN_OK;
}
