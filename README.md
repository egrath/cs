# CalcStar (C89/curses)

A faithful console clone of MicroPro International's **CalcStar**, the 1981
spreadsheet (CS-3510-1, October 1981), written in pure ANSI C89 using the
curses library. Behavior, layout, and keystrokes follow the original
User's Manuals. Primary reference: `notes/CalcStar_1982.txt`
(First issue November 8, 1982 — the newer, cleaner OCR of the User's
Manual); the older 1981 transcription (`docs/manual.pdf`,
`notes/CalcStar_1981.txt`) is kept as a co-primary source — its
error-message appendix is more exhaustive and less garbled, so cross-check
both manuals; where they disagree prefer the clearer reading and note it.

## BCD arithmetic (no floating point)

Like the original CalcStar program on machines that had no FPU, all numeric
work is **BCD decimal arithmetic** (`src/num.c`/`src/num.h`): every value is
carry-side decimal digits, `value = 0.d1d2... * 10^exp` with up to **40
digits** (30 calculation digits + 10 guard digits — an engineering
extension; the original carried 22 and the manual promises "accurate to 14
decimal places"); the exponent range is ±999.  Add, subtract,
multiply (schoolbook digit multiplication), divide (pencil-and-paper long
division) and every system function work on digits:

- `SUM AVG CNT MIN MAX` and the regression set are exact through the
  integer part; conditionals/comparisons use decimal compares.
- `**` power (manual 4.7/14-35, worked example `(28**2)+(52**2)`): integer
  exponents by repeated exact multiplication, fractional ones via
  `exp(y*ln(x))`; right-associative and tighter than `* /`
  (the manual's example `10*LOG(56/(12*10**-3))` = 36.69 dB confirms).
- `SQRT EXP LN LOG` are implemented **in BCD**: Newton's iteration for
  square roots with a final snap to the largest representable value whose
  square does not exceed the argument; series (e^rem = 1 + r/1! + r²/2!)
  for the exponential with range reduction by whole `ln(10)` steps; the
  natural log through `ln((1+z)/(1-z))` with `z = (m-1)/(m+1)` (one
  `sqrt(10)` half-step of the exponent); `LOG` computed directly, so
  integral powers of ten come out exactly (`LOG(0.001) = -3`).
- **Trigonometry in RADIANS, in and out** (SIN COS TAN ASIN ACOS ATAN —
  plus `PI()` at the full 40-digit internal precision; the trig set is not
  documented in the manual, reconstructed from the original's feature
  set): Maclaurin series of the reduced angle (≤ π/2 after mod-2π folding),
  `atan` via the complement identity `atan(x) = π/2 − atan(1/x)` for
  |x| > 1 plus the half-angle identity `atan(x) = 2*atan(x/(1+sqrt(1+x²)))`,
  and `asin/acos` through atan.  All series results are **rounded at the
  30th digit** so boundary-exact values (SIN(PI()/6) = 0.5) come out clean;
  the **inverse trigonometric family publishes at the 38th digit** so
  `tan(atan(x))` round trips keep the argument's digits; the cell display
  still truncates, per the manual.  **Degree converters:** `DEG(x)` /
  `DEGREES(x)` (radians → degrees) and `RAD(x)` / `RADIANS(x)` (degrees →
  radians).
- The Evaluate (`?`) calculator prints **24 decimal places** (manual 4.1/4.2
  pinned 12; user-extended to match the worksheet), and a leading `+`/`-`
  is required for functions in entries (manual: "if a + or a — is not
  placed in front of the function... the entry will be read as a text
  entry").
- Column precision prompt and error: user-extended from the manual's
  Appendix C range to `precision(0 - 24)` / `Prec. must be 0 - 24`
  (inside the 30-digit honest core).

### Precision at a glance

- **Internal:** 40-digit decimal (BCD) mantissa; every function is honest
  to **30 significant digits** (`round30`), and pure arithmetic truncates
  at 40 — no floating point anywhere.  The display always **truncates**
  (never rounds), per the manual.
- **Visible:** up to **24 decimals** per column (`;F`, then `P`, then the
  precision), the `?`-calculator likewise.  Set `;F P 24` and a 63-wide
  column, and type (all verified from the live program):

  | entry | cell shows |
  |---|---|
  | `+asin(acos(atan(tan(cos(sin(9))))))` | `0.424777960769379715387930` (exactly 3π − 9) |
  | `+sqrt(2)` | `1.414213562373095048801688` |
  | `+pi()` | `3.141592653589793238462643` |
  | `+sin(pi()/6)` | `0.500000000000000000000000` |

  `+2**0.5?` answers in the status bar with
  `=1.414213562373095048801688`.  Inverse-trig round trips keep the
  argument: `+tan(atan(123456789.123456789))` returns
  `123456789.123456789000000000000` (the published 38-digit inverse-trig
  floor), and tiny arguments pass through exactly: `+sin(1e-20)` →
  `0.000000000000000000010000`.
- **Comparisons see all honest digits:** two values that were equal at 15
  digits but differ beyond now compare unequal (`=` in a conditional, and
  copy/delete dependency scans).
- **Differential harness:** `tests/precision_ref.c` + `tests/precision_ref.py`
  dump every transcendental function at 38 decimals and compare against a
  150-digit Python-decimal reference (`python3 tests/precision_ref.py
  /tmp/eng.txt`); the engine agrees to ≤ ~7e-30 relative everywhere.
- **Form mode (`;F ... F`, manual 11.3) and the `;A` auto course**: a flagged
  cell shows `(form mode set)` after the type text - e.g.
  `type: (empty, but allocated) (form mode set)` on an empty stop cell,
  `type: numeric (form mode set)` once data is in. The flag survives
  commits, `;D E`, copy, and save/load (CSD3); it is reset only by
  `;D A` / clear. `;A` walks the flags in L-R/T-B order and announces
  itself: `Form Control Mode` on the bottom row at every stop, and
  `Auto complete` when the course ends (the original's wording). RETURN
  without typing keeps the stored value and jumps to the next stop; ESC
  aborts with `Auto aborted`. `;` inside the course is the command
  lead-in (opens the command menu, never enters the cell; the pending
  field text survives the menu). While a stop is being edited the type
  line shows the live interpretation (`numeric`/`text: ...`) plus the
  form suffix, and the status row's `current:` tracks the stop cell, as
  with normal entry.
- **Conditional text (reconstruction beyond the manual's numeric examples)**:
  branch values may be string literals (`+A1<A2:"a1 is smaller":"a2 is
  smaller"`) or references to TEXT cells; string-vs-string comparisons use
  plain string ordering, while string-vs-number conditions give `?n?` (the
  six operators stay as documented).  During entry the `"` key is a literal
  quote character — the TEXT/NUMERIC toggle lives on `^` — and a committed
  conditional string shows in the window, the raw formula on the contents
  line.
- **Light algebra (deliberate modern extension, not in the 1982 manuals;**
  integer caps preserve BCD precision — inputs beyond them give `?n?`):
  `SIGN`, `MOD` (sign-of-dividend, i.e. truncated division), `POWER(v,e)`,
  `FACT(n)` (`n >= 25` overflows), `COMBIN(n,k)` / `PERMUT(n,k)` (with the
  falling-product so nothing spills), `GCD`/`LCM` (integer path to 12
  digits), `DEGREES`/`RADIANS` (both derived from the same 22-digit `PI()`).
  Function-index note: the grammar cannot see a function name containing
  digits (the scanner stops at the first digit), hence no `LOG10` spelling —
  the manual's `LOG` is already the common logarithm.
- **Round/truncate functions**: `ROUND(x,d)` (half-up with correct sign
  handling) and `TRUNC(x,d)` — `d` within −16..+16 (negative `d` means
  tens/hundreds), else `?n?`. These are user-called functions; the display
  itself still only ever truncates (the manual rule). `AVERAGE` exists as
  an alias of the manual's `AVG`.
- Digits beyond the 18 mantissa digits are **truncated, never rounded**
  (the manual: "Numbers are not rounded but are truncated (cut off)").
- Nothing over the exponent range triggers the manual's `math op error -
  numeric overflow` (evaluation Error 0, shown as `?n?`); values that
  underflow collapse to zero.
- **Inline comments** (manual II.18): everything from a `\` to the end of an
  entry is non-evaluative: `+SUM(A1>A5) \This is a comment`.  The comment is
  kept in the cell contents (shown on the contents line) but never shown in
  the worksheet, never printed, and never evaluated; backslashes inside
  `"..."` strings do not start a comment.  Comment text is invisible to the
  reference scanners (copy relative-shift, insert/delete shifts, and the
  delete dependency check), so "A1" inside a comment is not a reference.
- Cells and the save format (`CSD2` — numbers stored as sign, exponent,
  digit bytes) hold the BCD values as-is.  Files produced by older builds
  (`CSD1`, IEEE doubles) are refused with the manual's `READ ERROR`.

## Build & run

```
make            # cc -std=c89 -pedantic -Wall ... -lcurses -lm
                # -> per-file objects and the executable land in build/
make clean      # removes build/
make test       # evaluator + save/load self test (./build/calcstar --test)
./build/calcstar
./build/calcstar NAME load NAME.csd at startup (also accepts NAME.csd /
                      NAME.CSD typed with an extension)
```

`--test` runs the self-test suite (manual examples: `140%E4`, `120%18.53333`,
`SUM(L4>L11)` = 2247.04, regression `REGR/SLOPE/PROJ/DEPD`, conditionals,
division-by-zero `?n?`, display truncation, file round-trip) and exits with a
nonzero status on any failure. An interactive session needs a real terminal;
launch it under a tty (or drive it with a pty driver such as `expect`).
If the file given on the command line does not exist, the usual
`write error NAME.csd not found` message appears on the status row.

Save-file naming: a bare name gets the lowercase `.csd` appended
(`pt01 -> pt01.csd`); a typed `.csd`/`.CSD`/`.Csd` tail is kept exactly as
entered and never doubled (`pt02.csd` saves `pt02.csd`, not
`pt02.csd.csd`). Load, Merge and the existing-file password probe use the
same rule. In the `;S` dialog the `P)artial or A)all` question must be
answered: any other key re-prompts (originally - pressing RETURN does
nothing), ESC cancels.

## Screen layout (verbatim clone of a genuine 80x24 CalcStar screen)

```
 -Cursor Movement- |        -Commands-  ; followed by             | -Misc-
  <CR> Right       |A Auto   F Format  M Merge  R Recalc * Extend |@ Curs Pos
 ^S Left  ^D Right |C Copy   H Help    O Order  S Save   = Lock   |? Evaluate
 ^E Up    ^X Down  |D Delete I Insert  P Print  W What   ? Space  |^ Data Togl
 ^Z Col A next row |E Edge   L Load    Q Quit   G or <TAB> Goto   |<ESC>Cancel
Col>|A         |B         |C         |D         |E         |F         |
Row+--------------------------------------------------------------------------
  1| ... cell contents ... (> and < flank the current cell)
  2|
   +--------------------------------------------------------------------------
[filename  ]  cursor:    B3      current:    B3     L-R
current||       type: numeric
data   ||   contents: 33.00
                 edit:        (cell entry line)
  Command >      (prompts / messages live on the very last row)
```

The 80x24 layout above is the minimum classic screen. On larger terminals the
extra space is used for the **worksheet**: more rows below row 10 and more
columns to the right (the chrome — menu, headers, border, cursor/type/contents/
edit/prompt rows — stays the same size). Verified: 100x35 shows 21 worksheet
rows and columns A–H, 132x40 shows 26 rows and A–K, and the extended (`;*`)
window grows accordingly. On a classic 80x24 terminal the screen is byte-for-
byte identical to the original capture.

**Resizing while running is supported**: when the terminal window is resized,
ncurses' `KEY_RESIZE` triggers a full repaint at the new size. This works from
the idle screen, in the middle of typing a cell entry (typed text is kept and
the cursor stays on the edit line), and inside any dialog prompt (`Goto>`,
Save, Lock, password, help pages, print pager). A pending `Goto>` prompt is
redrawn at the new size and still accepts its input.

Details matched to the original capture:

- no `*` frame; the freshly printed 5 menu rows are reproduced byte for byte;
- column bars sit at x=4,15,26,... (11-wide blocks: 10 content + bar), data
  rows carry a 3-char right-justified row label plus `|` and no column bars;
- the current cell is flanked by `>` / `<` painted on the separator columns;
- type/contents values align at x=22, labels right-aligned to x=20;
- everything is clamped to 80 columns even on wider terminals, and the
  terminal cursor is kept visible and blinking (`curs_set(2)`, falls back
  to `curs_set(1)`), sitting on the entry line while typing and on the
  current cell otherwise.

`;*` (Extend) switches to the 15-row display; the menu is dropped, the
column/row headers move up and the whole layout still fits 24 lines.

## Keys

| key        | action                                            |
|------------|---------------------------------------------------|
| `^E ^X ^S ^D` | cursor up / down / left / right |
| arrows      | same movement (cursor keys on terminals that send them) |
| `^Z`       | column A, next row                                |
| RETURN     | commit entry, cursor stays (move right when idle) |
| TAB        | Goto  (`Goto> coord`)                             |
| `;`        | command menu (see below)                          |
| `=`        | Lock: `R)ow C)ol B)oth` — pin rows/cols; rows show `label*` |
| `^B`       | Data toggle (formula/value view)                  |
| ESC        | cancel entry / hide prompts                       |
| `"`        | toggle text/numeric entry mode                    |
| `@`        | insert the coordinate of the cell the cursor is on |
| digits `0-9` `+` `-` `(` `.` | start a numeric entry (formula) |
| letters    | text entry: `/R` `/C` `/L` justify, `/=_` repeat, `/page` |
| `!expr!`   | input formula (shows `In!` until values exist)    |
| trailing `?` | calculator: evaluate without storing (e.g. `120%18.53333?` → 22.24) |

## Commands (type `;` then a letter)

```
A Auto    F Format    M Merge     R Recalc  | * Extend  @ Curs Pos
C Copy    H Help      O Order     S Save    | = Lock    ? Evaluate(space)
D Delete  I Insert    P Print     W What    | <ESC> Cancel
E Edge    L Load      Q Quit      G/TAB Goto
```

- `;R` recalculates: the worksheet is **not** recalculated automatically when
  an entry is committed (dependent cells keep their old values, just like
  the original). `;R` prompts `Recalculate - A)ll or E)ntry`.
- Cycles are not a hang: a formula that references itself (e.g. `+B2` entered
  in B2) evaluates against the current stored values and — if that fails —
  displays `Error 0-> +B2  hit space` on the status row. The application
  never locks up while that message is showing: any key press clears the
  message and is processed normally (Space just returns to the `edit: `
  line without doing anything else).
- Command prompts and messages appear on the **last screen row** (row 24),
  below the `edit:` entry line; the `;` prompt reads `Command > `.
- The terminal cursor always rests on the `edit: ` field — at the idle
  position after a message or when idle, after the typed text while
  entering data. It never rides the `>` `<` cell markers and never parks
  in the status bar; only prompts that expect input there (`Command > `,
  `Goto>`, dialogs) hold the cursor themselves.
- System messages (errors, `Form Control Mode`, `Auto complete`,
  `Auto aborted`, `SAVED`, ...) are painted **inverse video** as in the
  original; input prompts stay non-inverse.
- `;?` reports `Room for N entries` (2000-entry emulated pool); a passive
  `MEMORY GETTING LOW` appears on the right of the screen as the pool fills.
- `;O` toggles recalculation order left-right / top-bottom.
- `;=` locks rows (`*` markers: locked rows show `  1*` instead of `  1|` in
  the row label, locked columns show `*` bars in the column header).
- Delete checks dependencies first: `ERROR: would delete ref(s) at ...` with
  clear-all requiring `verify Y/N`.
- Print writes reports to files (`NAME.PRN`), to the `PRINTER:` device
  (written to `CALCSTAR.PRN`), to `>file` as comma-delimited export, or to
  `DISPLAY` on the console. `/page` text rows force page breaks; titles and
  `Fix ordinates` are prompted per the manual.

## Formulas

Start with `+` (or a digit/`(./ `-`). Functions: `SUM AVG CNT MIN MAX ABS EXP
LOG LN SQRT` and the regression set `REGR SLOPE PROJ DEPD`, evaluated left-
to-right by default (`;O` toggles).

- Ranges are written `A1>A5`; single refs `A1`.
- `140%E4` loads 140% × E4 — the manual's keyword-table example (0.028 with
  E4 = .02); `50%` = 0.5; `120%18.53333?` = 22.239996.
- Cond expression `cond:t:f`, e.g. `1<2:5:7`; empty branches are 0 (`1<2::9`).
- `!` after a reference locks it during relative copy (`A1!` stays A1).
- REGR uses the "last evaluated regression" semantics of the original:
  `REGR(range,DEPSTART-coord)` records the regression (returning the mean of
  the dependent range, per manual example `+regr (B2>G2,B4)`), then
  `SLOPE()`, `PROJ(x)`, and `DEPD(y)` work with the most recent REGR
  (`;R A` re-evaluates).
- Errors display as `?n?` (Error 0, value range/div-zero) or `Error n` per
  the manual's error table (1 coordinate, 2 range, 3 paren, 4 unknown fn,
  6 terminal, 7 trailing, 8 number, 9 regression, 12 depth, 255 illegal
  REGR value). Internal math carries 40 digits (30 honest through `round30`);
  display truncates at the column precision (up to 24 decimals) rather than
  rounding (manual-faithful).

## File format (.CSD)

Custom documented format ("CSD1"), written by `src/fileio.c`: optional
password (hidden `*` echo on load), column widths/precisions, cell records
with contents, values and repeat patterns. Layout:

```
magic   4 bytes  "CSD1"
colw    127 sets of: width byte, precision byte (0 = default 10 / 2)
pwlen   1 byte   password length, 0 = none (stored in plain text)
pw      pwlen bytes  password (asked for on load)
count   2 bytes  number of cell records
records until count/EOF:
  col   1 byte, row  2 bytes (little endian)
  type  1 byte   2=text, 3=numeric
  just  1 byte   'L','R','C','W'
  prec+1 1 byte   per-entry precision, 0 = column default
  formf 1 byte   form flag
  ncont 2 bytes + bytes  contents as typed
  numeric only:
      [8-byte double, stored value]
      [nstr 2] [bytes] string result (formula returning text)
  nrep  2 bytes + bytes  repeat pattern (just 'W')
```

Password: stored as typed and prompted for on load.

## Deviations from the original

- Print to a real printer is not attached to a device; `PRINTER` writes
  `CALCSTAR.PRN` for spooling. All print output is pure ASCII (identical
  to valid UTF-8: the entry path admits printable ASCII only) — files
  only, no transcoding path.
- `;P` file destinations follow the manual's extension rule (14-14); the
  default output name is `print.txt` (RETURN takes it, `.TXT` kept):
  a typed name keeps its extension; a bare name gets `.TXT` (text
  report) or `.DTA` (`>name` data records) appended. Prompt literals are
  the manual's (`top left corner :`, `bottom right corner :` with the
  engine-displayed farthest bottom-right default, `Form length :`  editor shows CONTINUOUS, accepts digits only
  (backspace falls back to CONTINUOUS, out-of-range answers get
  `bad form length` (1..66) and re-ask), `Stop on each page (y,n)?`, `Printer width : 132`,
  repeating `Title > `, printing starts on an empty title). `Fix
  ordinates (y,n)?` is asked only when A1 was not the origin. After the
  report the flow ends with `-** End of report` + space (the single-row
  screen shows the ending that a physical printer prints on paper).
- File format is the documented "CSD1" above, not a byte-identical clone of
  the original binary format.
- `^B` implements the manual's data toggle as formula/value view.
- No recalculation flicker; the screen repaints atomically via curses.
- No sample files or `csdump` utility are included (by design).
- 1982-disk/terminal-specific error paths have no curses analog and are
  deliberately skipped: `couldn't read TERMCAP.SYS`, `delete character is
  underscore`, `FATAL ERROR: not on disk`, `write error I/O`,
  `write error Close`, `can't open` (disk-space semantics; the string is
  used for the print output file), `OUT OF MEMORY` (unified into
  `MEMORY IS TOO LOW`), and `DATA IS PROBABLY DAMAGED`. `math op error
  numeric overflow` / `divide by zero` surface as the manual's evaluation
  code 0 (`?n?` in the cell + sticky `Error 0-> ...`); `ERROR ->
  <expression>?` is covered by the same sticky line.

## Source map

| file          | role                                                   |
|---------------|--------------------------------------------------------|
| `src/cs.h`    | shared types, globals, prototypes                      |
| `src/cells.c` | cell store, coordinate parse/format, ref shifting      |
| `src/parser.c`| expression evaluator (cond, %, refs, ranges, functions)|
| `src/eval.c`  | commit-entry, recalculation (L-R / T-B)                |
| `src/screen.c`| renderer + all prompts (WordStar-style status lines)   |
| `src/command.c`| `;` command set                                       |
| `src/fileio.c`| save/load/merge, password dialog                       |
| `src/print.c` | Print command, titles, segments, page breaks           |
| `src/help.c`  | two help pages                                          |
| `src/main.c`  | key loop, entry engine, `--test` mode                   |
