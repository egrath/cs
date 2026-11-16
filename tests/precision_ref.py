#!/usr/bin/env python3
"""precision_ref.py - high-precision reference for the BCD engine.

Generates the same (function, input, value) table as tests/precision_ref.c
using Python's decimal module at 150 digits (independent arithmetic), and
- when given the engine's dump as an argument - reports every relative
deviation above the 2e-29 tolerance.

    python3 tests/precision_ref.py > /tmp/py.txt          # reference only
    cc -std=c89 -I src tests/precision_ref.c src/num.c -o /tmp/pref
    /tmp/pref > /tmp/eng.txt
    python3 tests/precision_ref.py /tmp/eng.txt           # full comparison

All functions are honest to 30 significant digits in the engine; a
relative deviation up to ~7e-30 is the expected round30/snap granularity.
"""
import sys
from decimal import Decimal as D, getcontext
getcontext().prec = 150

PI = D("3.14159265358979323846264338327950288419716939937510582097494459230781640628620899")

def d2r(x): return x * PI / 180
def r2d(x): return x * 180 / PI

def sin_r(x):
    s = D(0); term = x; n = 0; x2 = x*x
    while True:
        s += term if n % 2 == 0 else -term
        n += 1
        term = term * x2 / ((2*n)*(2*n+1))
        if abs(term) < D("1e-58"): break
    return s

def cos_r(x):
    s = D(0); term = D(1); n = 0; x2 = x*x
    while True:
        s += term if n % 2 == 0 else -term
        n += 1
        term = term * x2 / ((2*n-1)*(2*n))
        if abs(term) < D("1e-58"): break
    return s

def tan_r(x): return sin_r(x)/cos_r(x)

def atan_r(x):
    n = 0; t = x
    while abs(t) >= D("0.5"):
        t = t / (1 + (1+t*t).sqrt()); n += 1
    s = D(0); term = t; k = 0; t2 = t*t
    while True:
        s += term if k % 2 == 0 else -term
        k += 1
        term = term * t2 * (2*k-1) / (2*k+1)
        if abs(term) < D("1e-58"): break
    return s * (2 ** n)

def asin_r(x): return 2 * atan_r(x / (1 + (1-x*x).sqrt()))
def acos_r(x): return PI/2 - asin_r(x)

def ln10_series():
    z = D(9)/D(11); z2 = z*z
    s = D(0); term = z; k = 0
    while True:
        s += term / (2*k+1)
        k += 1
        term = term * z2
        if abs(term/(2*k+1)) < D("1e-58"): break
    return 2*s

def exp_r(x):
    ln10 = ln10_series()
    n = 0; rem = x
    while rem > ln10/2:
        rem -= ln10; n += 1
    s = D(0); term = D(1); k = 1
    while True:
        s += term
        term = term * rem / k
        k += 1
        if abs(term) < D("1e-58") and k > 12: break
    return s * (D(10) ** n)

def ln_r(x):
    ln10 = ln10_series()
    n = 0; m = x
    while m > D(2):
        m = m / 10; n += 1
    z = (m-1)/(m+1)
    s = D(0); term = z; k = 0; z2 = z*z
    while True:
        s += term / (2*k+1)
        k += 1
        term = term * z2
        if abs(term/(2*k+1)) < D("1e-58"): break
    return 2*s + n*ln10

def log10_r(x): return ln_r(x)/ln_r(D(10))

def wrap_sin(x):
    return sin_r(x)            # engine trig is in radians

def wrap_cos(x):
    return cos_r(x)

def wrap_tan(x): return tan_r(x)
def wrap_atan(x): return atan_r(x)
def wrap_asin(x):
    if x == 1: return PI/2
    if x == -1: return -PI/2
    return asin_r(x)
def wrap_acos(x): return acos_r(x)

VALS = ["9","30","45","60","90","0.5","1","0.9999","-0.9999","0.99999",
        "0.9999976","0.156434465040230869010105319626",
        "0.999974999687492187255850829757","0.00005",
        "0.000050000000000000000000000001","2","10","0.001",
        "2.718281828459045235360287471352662497757","100","0.7",
        "0.123456789012345678901234567890","89.59485428146715022946",
        "0.017455","141.417820659208293429514135246638"]

FNS = {
    "sqrt":  lambda v: v.sqrt() if v >= 0 else None,
    "exp":   exp_r,
    "ln":    lambda v: ln_r(v) if v > 0 else None,
    "sin":   wrap_sin, "cos": wrap_cos, "tan": wrap_tan,
    "atan":  wrap_atan, "asin": wrap_asin, "acos": wrap_acos,
}

def dump():
    for fn, f in FNS.items():
        for vs in VALS:
            v = D(vs)
            try:
                r = f(v)
            except Exception:
                continue
            if r is None: continue
            if abs(r) > D("1e999"): continue
            print("%s %s %s" % (fn, vs,
                  format(r.quantize(D(1).scaleb(-38)), 'f')))

def compare(engfile):
    def load(path):
        d = {}
        for ln in open(path):
            p = ln.split()
            if len(p) >= 3: d[(p[0], p[1])] = p[2]
        return d
    # engine table from stdin-style file; reference fresh
    import io, contextlib
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        dump()
    py = {}
    for ln in buf.getvalue().splitlines():
        p = ln.split()
        py[(p[0], p[1])] = p[2]
    eng = load(engfile)
    worst = {}
    bad = 0
    for k in sorted(py):
        if k not in eng: continue
        a, b = D(eng[k]), D(py[k])
        scale = max(abs(a), abs(b), D('1e-30'))
        rel = abs(a-b)/scale
        fn = k[0]
        if fn not in worst or rel > worst[fn][0]: worst[fn] = (rel, k, eng[k], py[k])
        if rel > D('2e-29'):
            bad += 1
            print("DIFF %s %s\n  eng %s\n  py  %s  rel %.3g" % (k[0], k[1], eng[k], py[k], rel))
    print("worst relative deviation per function (tolerance 2e-29):")
    for fn, (rel, k, e, p) in sorted(worst.items()):
        flag = " <-- OVER" if rel > D('2e-29') else ""
        print("  %-5s %.3g  (%s)%s" % (fn, rel, k[1], flag))
    print("total over-tolerance diffs:", bad)
    return bad

if __name__ == "__main__":
    if len(sys.argv) > 1:
        sys.exit(1 if compare(sys.argv[1]) else 0)
    dump()
