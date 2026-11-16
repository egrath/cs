#!/usr/bin/env python3
"""pty_driver.py - the READING pty driver for CalcStar benchmarks.

THE LESSON THIS ENCODES (2026-11-16): a pty driver that never reads the
master during its waits DEADLOCKS the app - the paint output fills the
pty buffer, the app blocks in write(), stops reading input, and
everything flushes at EOF in one giant burst.  Every wait here pumps
the master (pump()); never sleep bare.

Usage: python3 tests/pty_driver.py [BINDIR] [RAWOUT] [--moves N] [--seed]
  BINDIR   directory with build/calcstar (default: the tree root)
  RAWOUT   where to write the summary line (default: stdout)
  --moves  the number of unbatched cursor moves to benchmark (400)
  --seed   seed a 7x11 numeric grid first (77 cells, the populated case)

The benchmark: 400 single ^X moves, one per 20 ms (the queue drains
between keys, so each key paints immediately), on a loaded sheet;
reports the child's ru_utime/ru_stime (wait4) - the CPU cost of the
paints.  A quick byte count per interaction is available in bench.exp.
"""
import os, pty, sys, time, select, argparse

HERE = os.path.dirname(os.path.abspath(__file__))

def pump(fd, t):
    """read the master while waiting t seconds - THE deadlock guard."""
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.01)
        if r:
            try:
                if not os.read(fd, 65536):
                    return False
            except OSError:
                return False
    return True

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('bindir', nargs='?', default=os.path.dirname(HERE))
    ap.add_argument('rawout', nargs='?', default=None)
    ap.add_argument('--moves', type=int, default=400)
    ap.add_argument('--seed', action='store_true')
    args = ap.parse_args()
    pid, fd = pty.fork()
    if pid == 0:
        os.chdir(args.bindir)
        os.execv('./build/calcstar', ['./build/calcstar'])
        os._exit(1)
    pump(fd, 2)
    if args.seed:
        # a 7x11 grid of numbers: value \r, then arrow right; ^X at the
        # row end after 6 arrows left (a commit KEEPS the cursor)
        for r_ in range(11):
            for c_ in range(7):
                os.write(fd, b'%d\r' % (r_ * 7 + c_ + 1))
                pump(fd, 0.003)
                if c_ < 6:
                    os.write(fd, b'\x1b[C')
                    pump(fd, 0.003)
                elif r_ < 10:
                    os.write(fd, b'\x1b[D\x1b[D\x1b[D\x1b[D\x1b[D\x1b[D\x18')
                    pump(fd, 0.006)
        pump(fd, 2)
    for i in range(args.moves):
        os.write(fd, b'\x18')
        pump(fd, 0.02)
    pump(fd, 3)
    os.write(fd, b';Q')
    pump(fd, 0.8)
    os.write(fd, b'Y')
    t0 = time.time()
    while time.time() - t0 < 15:
        r, _, _ = select.select([fd], [], [], 0.3)
        if r:
            try:
                if not os.read(fd, 65536):
                    break
            except OSError:
                break
    _, status, ru = os.wait4(pid, 0)
    line = "user=%.4f sys=%.4f status=%d\n" % (ru.ru_utime, ru.ru_stime, status)
    if args.rawout:
        open(args.rawout, 'w').write(line)
    print(line, end='')
    return 0 if status == 0 else 1

if __name__ == '__main__':
    sys.exit(main())
