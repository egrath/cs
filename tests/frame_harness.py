#!/usr/bin/env python3
"""frame_harness.py - the frame-identity regression harness.

Runs a scripted interaction sequence against TWO CalcStar builds (the
tree's binary and a REFERENCE build - typically a worktree of the
pre-change commit), renders every cumulative checkpoint through
pty_render.py and compares the final frames.  Any diff is a paint or
invalidation regression: the rendered SCREENS must match even though
the paint STREAMS may differ (ncurses diffing makes the bytes differ).

Usage: python3 tests/frame_harness.py [REFDIR] [--upto N]
  REFDIR   a directory with a build/calcstar to compare against
           (default: /tmp/cs_master - the standing reference worktree)
  --upto N run only checkpoints 0..N (default: all 16; the full run is
           ~35 min - run it in the background)

The 15 interaction segments (goto+commit, scrolls, far goto, format,
lock, extended toggle, recalc, copy, help, delete, ESC) are cumulative:
checkpoint k = segments 0..k, ending with ;Q + N (cancel, so the sheet
state stays for the frame).

The two builds run IN PARALLEL per checkpoint (each spawns its own
expect).  Depends on: tests/pty_render.py, expect.  Scratch captures
go to tests/.scratch/ (gitignored).
"""
import os, subprocess, sys, argparse

HERE = os.path.dirname(os.path.abspath(__file__))

SEGS = [
    [],                                      # 0: idle
    [";G", "B3\r", "33.00\r", "\x1b"],       # 1: goto + commit
    ["\x18"] * 24,                           # 2: down x24 -> v-scroll
    ["\x1b[C"] * 18,                         # 3: right x18 -> h-scroll
    ["\x1b[D"] * 18, ["\x1b[A"] * 24,        # 4: back left + up
    [";G", "Z30\r", "hello\r", "\x1b"],      # 5: far goto + text commit
    [";F", "W", "A", "14\r", "\x1b"],        # 6: format width
    [";F", "P", "6\r", "\x1b"],              # 7: format precision
    ["=B", "\x1b"],                          # 8: lock
    ["\x02", "\x02"],                        # 9: ^B data toggle twice
    [";R", "A\r"],                           # 10: recalc all
    [";C", "A1>A2\r", "A4>A5\r", "R", "D"],  # 11: copy
    [";H", "\r", "\r"],                      # 12: help pages
    [";D", "R", "\r", "y\r"],                # 13: delete row
    ["\x1b"],                                # 14: esc
]

def esc(k):
    out = []
    for ch in k:
        if ch == '\\': out.append('\\\\')
        elif ch == '"': out.append('\\"')
        elif ch == '$': out.append('\\$')
        elif ch == '[': out.append('\\[')
        elif ord(ch) < 32: out.append('\\x%02x' % ord(ch))
        else: out.append(ch)
    return ''.join(out)

def run(keys, bindir, rawpath, expfile):
    """run the given build with the given keys, render the final frame."""
    if os.path.exists(rawpath): os.remove(rawpath)
    if os.path.exists(rawpath + '.txt'): os.remove(rawpath + '.txt')
    L = ["#!/usr/bin/expect -f", "set timeout 30",
         "log_file -noappend " + rawpath, "cd " + bindir,
         "spawn ./build/calcstar", "sleep 2"]
    # each segment as ONE burst: the app reads keys sequentially from
    # the tty buffer; a drain between segments lets dialogs settle
    for k in keys:
        L.append('send "' + esc(k) + '"')
        L.append("sleep 1.2")
    L += ["sleep 3", "send {;Q}", "sleep 0.8", "send {N}", "sleep 2.5",
          "expect eof", "exit 0"]
    open(expfile, 'w').write("\n".join(L) + "\n")
    subprocess.run(['expect', expfile],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   timeout=200)
    subprocess.run(['python3', os.path.join(HERE, 'pty_render.py'),
                    rawpath, '--size', '80x24'],
                   stdout=open(rawpath + '.txt', 'w'),
                   stderr=subprocess.DEVNULL)
    return open(rawpath + '.txt').read()

def checkpoint(k, refdir, scratch):
    """run checkpoint k against both builds in parallel, compare frames."""
    keys = [x for s in SEGS[:k+1] for x in s]
    rawn = os.path.join(scratch, 'fc_new_%d.raw' % k)
    rawo = os.path.join(scratch, 'fc_old_%d.raw' % k)
    p1 = subprocess.Popen([sys.executable, '-c',
        'import sys; sys.path.insert(0, %r);'
        'import frame_harness as fh;'
        'keys = [x for s in fh.SEGS[:%d+1] for x in s];'
        'fh.run(keys, %r, %r, %r + ".exp")' % (
            HERE, k, os.getcwd(), rawn, rawn)])
    b = run(keys, refdir, rawo, rawo + '.exp')
    p1.wait()
    txt = rawn + '.txt'
    a = open(txt).read() if os.path.exists(txt) else ''
    return a, b

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('refdir', nargs='?', default='/tmp/cs_master')
    ap.add_argument('--upto', type=int, default=None)
    args = ap.parse_args()
    scratch = os.path.join(HERE, '.scratch')
    os.makedirs(scratch, exist_ok=True)
    upto = args.upto if args.upto is not None else len(SEGS) - 1
    fails = 0
    for k in range(upto + 1):
        a, b = checkpoint(k, args.refdir, scratch)
        if a == b:
            print(f"ck {k:2d}: IDENTICAL")
        else:
            fails += 1
            print(f"ck {k:2d}: DIFF")
            la, lb = a.split('\n'), b.split('\n')
            shown = 0
            for j, (x, y) in enumerate(zip(la, lb)):
                if x != y and shown < 4:
                    print(f"   line {j+1} new: {x!r}")
                    print(f"   line {j+1} old: {y!r}")
                    shown += 1
    print("FAILS:", fails)
    return 1 if fails else 0

if __name__ == '__main__':
    sys.exit(main())
