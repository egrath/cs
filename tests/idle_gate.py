#!/usr/bin/env python3
"""idle_gate.py - the idle-screen byte-exactness gate.

Renders the capture from tests/idle_capture.exp through pty_render.py
and compares it against the authentic 80x24 reference capture
(tests/cs1_reference.txt, byte-for-byte after tab expansion and
trailing-space normalization; a trailing empty line difference is the
capture artifact, normalized).

Usage: python3 tests/idle_gate.py [RAWSPEC]
  RAWSPEC  the raw pty capture (default: run tests/idle_capture.exp
           itself into tests/.scratch/idleA.raw first)
"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))

def norm(p):
    lines = [l.rstrip('\n').expandtabs(8).rstrip() for l in open(p)]
    while lines and lines[-1] == '':
        lines.pop()
    return lines

def main():
    scratch = os.path.join(HERE, '.scratch')
    os.makedirs(scratch, exist_ok=True)
    if len(sys.argv) > 1:
        raw = sys.argv[1]
    else:
        raw = os.path.join(scratch, 'idleA.raw')
        if os.path.exists(raw):
            os.remove(raw)
        subprocess.run(['expect', os.path.join(HERE, 'idle_capture.exp')],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=120)
        if not os.path.exists(raw):
            print("idle gate: NO CAPTURE (expect failed)")
            return 1
    rendered = os.path.join(scratch, 'idleA.txt')
    subprocess.run(['python3', os.path.join(HERE, 'pty_render.py'), raw,
                    '--size', '80x24'],
                   stdout=open(rendered, 'w'), stderr=subprocess.DEVNULL)
    a = norm(os.path.join(HERE, 'cs1_reference.txt'))
    b = norm(rendered)
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            print("MISMATCH line", i + 1)
            print("want:", repr(x))
            print("got: ", repr(y))
            return 1
    if len(a) != len(b):
        print("length %d vs %d" % (len(a), len(b)))
        return 1
    print("MATCH")
    return 0

if __name__ == '__main__':
    sys.exit(main())
