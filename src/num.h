/* num.h - CalcStar's BCD arithmetic engine (pure integer/byte ops).
 *
 * CalcStar ran on machines without a floating point unit and insisted
 * on decimal precision.  Its numbers are stored and computed as
 * decimal-digit (BCD) values.  This engine reproduces that: no float
 * or double is used anywhere in the program.
 *
 * Representation: a value is
 *        value = sign * (0.d1 d2 ... dn) * 10^exp10
 * with up to CSN_DIGS decimal digits, digit 1 the most significant
 * nonzero one (d1 != 0 unless the value is zero).  Arithmetic truncates
 * (never rounds) anything beyond CSN_DIGS digits, exactly as the
 * original cut numbers off (the manual: "Numbers are ... truncated").
 *
 * Exponent range is +/-CSN_EMAX.  Results beyond that are reported as
 * CSN_ER_OVERFLOW (the manual's "math op error - numeric overflow",
 * surfaced as evaluation Error 0 / ?n?) or, on the low end, collapse
 * to zero.
 */
#ifndef CS_NUM_H
#define CS_NUM_H

#define CSN_DIGS 22   /* mantissa digits: 14 calculation + 8 guard   */
#define CSN_EMAX 999  /* exponent range: value = 0.d1d2... * 10^exp  */

typedef struct {
    int sign;                   /* 0 = positive (or zero), 1 = negative */
    int exp10;                  /* value = 0.nd * 10^exp10               */
    int ndig;                   /* significant digits, 0 = value is zero */
    unsigned char nd[CSN_DIGS]; /* digits, most significant first        */
} Csn;

/* error codes returned by the arithmetic functions */
#define CSN_OK          0
#define CSN_ER_OVERFLOW 1   /* magnitude out of range           */
#define CSN_ER_DIVZERO  2   /* divide by zero                   */
#define CSN_ER_DOMAIN   3   /* function of illegal argument     */
#define CSN_ER_SYNTAX   4   /* csn_parse: malformed number text */

void csn_zero(Csn *r);
void csn_copy(Csn *r, const Csn *a);
void csn_from_int(Csn *r, long n);            /* exact small integer */
void csn_canon(const Csn *v, char *buf, int bufn); /* 14-digit test form */
/* parse a decimal number: -digits[.digits][E[+-]digits] (exact).  The
 * typed digits are stored verbatim (truncated to mantissa capacity).
 * Returns CSN_OK, CSN_ER_SYNTAX (malformed) or CSN_ER_OVERFLOW. */
int  csn_parse(const char *s, Csn *r);
int  csn_is_zero(const Csn *a);
int  csn_cmp(const Csn *a, const Csn *b);
int  csn_add(Csn *r, const Csn *a, const Csn *b);
int  csn_sub(Csn *r, const Csn *a, const Csn *b);
int  csn_mul(Csn *r, const Csn *a, const Csn *b);
int  csn_div(Csn *r, const Csn *a, const Csn *b);
int  csn_neg(Csn *r, const Csn *a);
void csn_abs(Csn *r, const Csn *a);
int  csn_scale10(Csn *r, const Csn *a, int k); /* multiply by exact 10^k */
void csn_fmt_fixed(const Csn *v, int prec, char *buf, int bufn);
void csn_fmt_sci(const Csn *v, int frac, char *buf, int bufn);
int  csn_sqrt(Csn *r, const Csn *a);
int  csn_exp(Csn *r, const Csn *a);
int  csn_ln(Csn *r, const Csn *a);
int  csn_log10(Csn *r, const Csn *a);
int  csn_pow(Csn *r, const Csn *a, const Csn *b);   /* a ** b             */
int  csn_sin(Csn *r, const Csn *deg);   /* trigonometry: degrees in/out  */
int  csn_cos(Csn *r, const Csn *deg);
int  csn_tan(Csn *r, const Csn *deg);
int  csn_asin(Csn *r, const Csn *x);
int  csn_acos(Csn *r, const Csn *x);
int  csn_atan(Csn *r, const Csn *x);
void csn_pi(Csn *r);                 /* pi at maximum internal precision */
int  csn_sign(Csn *r, const Csn *a);          /* -1 / 0 / +1             */
int  csn_mod(Csn *r, const Csn *a, const Csn *b);  /* a mod b            */
int  csn_degrees(Csn *r, const Csn *rad);     /* rad -> deg              */
int  csn_radians(Csn *r, const Csn *deg);     /* deg -> rad              */
int  csn_fact(Csn *r, const Csn *n);          /* n!  (integer n)         */
int  csn_comb(Csn *r, const Csn *n, const Csn *k);
int  csn_perm(Csn *r, const Csn *n, const Csn *k);
int  csn_gcd(Csn *r, const Csn *a, const Csn *b);
int  csn_lcm(Csn *r, const Csn *a, const Csn *b);
int  csn_round(Csn *r, const Csn *x, const Csn *d);  /* round to d decimals */
int  csn_trunc(Csn *r, const Csn *x, const Csn *d);  /* truncate to d decs  */

#endif /* CS_NUM_H */
