#!/usr/bin/env python3
"""pty_render_selftest.py - the renderer's own regression gate.

Feeds pty_render.py synthetic CSI sequences and asserts the rendered
text.  THE LESSON (2026-11-16): twice a CORRECT app paint looked like a
screen ghost because the renderer lacked ED params (ESC[J 0), EL 1
(ESC[1K) or DCH (ESC[P) - and a hand-mangled renderer silently lost
paints.  Every new ncurses optimization the app's output shows must be
implemented AND pinned here.

Run: python3 tests/pty_render_selftest.py
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

CASES = [
    # (name, bytes, WxH, expected lines)
    ("plain text + CRLF",
     "hello\r\nworld", "20x3", ["hello", "world", ""]),

    ("ED 2 resets the screen (cursor stays)",
     "old\x1b[2Jnew", "20x3", ["   new", "", ""]),

    ("ED 0 erases cursor->end",
     "abc\x1b[1;2H\x1b[J", "20x2", ["a"]),

    ("ED 1 erases start->cursor (inclusive)",
     "abc\x1b[1;3H\x1b[1J", "20x2", []),

    ("EL 0 erases cursor->eol",
     "abcd\x1b[1;2H\x1b[K", "20x2", ["a"]),

    ("EL 1 erases start->cursor",
     "abcd\x1b[1;3H\x1b[1K", "20x2", ["   d"]),

    ("EL 2 erases the whole line (cursor stays)",
     "abcd\x1b[2Kxy", "20x2", ["    xy", ""]),

    ("DCH deletes at the cursor, shifts left",
     "abcdefgh\x1b[1;3H\x1b[2P", "20x2", ["abefgh", ""]),

    ("ICH inserts blanks",
     "abcdefgh\x1b[1;3H\x1b[3@", "20x2", ["ab   cdefgh", ""]),

    ("ECH erases without shifting",
     "abcdefgh\x1b[1;3H\x1b[2X", "20x2", ["ab  efgh", ""]),

    ("VPA/VCHA positioning",
     "\x1b[2;5Hxy", "20x3", ["", "    xy"]),

    ("tab expansion",
     "a\tb", "20x2", ["a       b"]),

    ("backspace",
     "abc\x08x", "20x2", ["abx"]),

    ("the app's cursor marks over a cell field",
     "\x1b[10;49H     12.00\x1b[10;48H\x1b[0;7m>\x1b[10;59H\x1b[0;7m<",
     "80x12", [""] * 9 + [" " * 47 + ">     12.00<"]),
]

def main():
    fails = 0
    for name, data, size, want in CASES:
        with tempfile.NamedTemporaryFile(suffix='.raw', delete=False) as f:
            f.write(data.encode('latin-1'))
            raw = f.name
        out = subprocess.run(
            [sys.executable, os.path.join(HERE, 'pty_render.py'), raw,
             '--size', size],
            capture_output=True, text=True).stdout
        os.unlink(raw)
        lines = out.split('\n')
        while lines and lines[-1] == '':
            lines.pop()
        want2 = [l.rstrip() for l in want]
        while want2 and want2[-1] == '':
            want2.pop()
        if lines == want2:
            print("ok  %-38s" % name)
        else:
            fails += 1
            print("FAIL %-38s" % name)
            print("   want:", want2)
            print("   got: ", lines)
    print("FAILS:", fails)
    return 1 if fails else 0

if __name__ == '__main__':
    sys.exit(main())
