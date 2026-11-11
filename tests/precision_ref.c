/* precision_ref.c - differential harness: dumps every transcendental
 * function at 38 decimals for a fixed input list, for comparison
 * against an independent high-precision reference (tests/precision_ref.py).
 *
 * Build:   cc -std=c89 -I src tests/precision_ref.c src/num.c -o /tmp/pref
 * Run:     /tmp/pref > /tmp/eng.txt
 * Compare: python3 tests/precision_ref.py /tmp/eng.txt
 *
 * Expected agreement: relative deviation <= ~7e-30 everywhere (the
 * round30/snap granularity of the 40-digit engine).  Anything above
 * ~2e-29 means the engine regressed - see AGENTS.md "Precision-era
 * reference strings" for the canonical values.
 */
#include <stdio.h>
#include <string.h>
#include "num.h"

static void show(const char *fn, const char *in, const Csn *v)
{
    char buf[160];
    csn_fmt_fixed(v, 38, buf, (int) sizeof(buf));
    printf("%s %s %s\n", fn, in, buf);
}

static void un(const char *fn, const char *in, int (*f)(Csn *, const Csn *))
{
    Csn a, b;
    csn_parse(in, &a);
    if (f(&b, &a) == 0) show(fn, in, &b);
    else printf("%s %s ERR\n", fn, in);
}

int main(void)
{
    static const char *vals[] = {
        "9", "30", "45", "60", "90", "0.5", "1", "0.9999", "-0.9999",
        "0.99999", "0.9999976", "0.156434465040230869010105319626",
        "0.999974999687492187255850829757", "0.00005",
        "0.000050000000000000000000000001", "2", "10", "0.001",
        "2.718281828459045235360287471352662497757", "100", "0.7",
        "0.123456789012345678901234567890", "89.59485428146715022946",
        "0.017455", "141.417820659208293429514135246638"
    };
    int i;
    for (i = 0; i < 25; i++) {
        const char *s = vals[i];
        un("sqrt", s, csn_sqrt);
        un("exp",  s, csn_exp);
        un("ln",   s, csn_ln);
        un("sin",  s, csn_sin);
        un("cos",  s, csn_cos);
        un("tan",  s, csn_tan);
        un("atan", s, csn_atan);
        un("asin", s, csn_asin);
        un("acos", s, csn_acos);
    }
    return 0;
}
