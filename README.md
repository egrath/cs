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

## Side effects and iteration (user-extended 2026-11, not in the manuals)

CalcStar formulas can now *execute*, making the spreadsheet itself the
computation engine (cells = memory, conditionals = branches, iterated
recalculation = loops).  Flagged extensions:

- **Side-effect functions** — produce no value and are only valid as
  statements inside conditional branches:
  - `SET(cell, value)` — assigns `value` to `cell`, fully evaluated
    *before* the write (`SET(A1,A1+1)` increments the previous value).
    A numeric expression makes a numeric cell; a quoted string makes
    text, with `\r` / `\l` / `\c` inside the string choosing the
    justification (`SET(A1,"\rtest")` = right-aligned "test").
  - `RECALC()` / `RECALC(cell)` — request recalculation passes.
- **Extended conditionals**: a branch may be a statement list,
  semicolon-separated — zero or more side-effect calls plus exactly one
  value expression (its result is the branch value), executed in written
  order; only the selected branch runs:

  ```text
  (A1<A2):SET(A1,A1+1);"increasing";RECALC():"finished"
  ```

- **Iteration**: `;R` (and an entry commit / the `?` calculator) run one
  whole-sheet pass; when a `RECALC()` executed, passes repeat until a
  pass changes no stored value (the spreadsheet stabilized), the
  **1000-pass limit**, or the user presses **ESC**
  (`User interrupted execution`; the limit shows
  `Recalculation limit reached`).  While iterating the status line shows
  `Calculating ... (pass N)`.

  ```text
  A11: 5          (a plain value)
  A12: +(A11>0):SET(A11,A11-1);RECALC();0:"finished"

  ;R A   ->  A11 counts 5..0, A12 shows 0 then "finished"
  ```

- Side effects as operands, top-level statements, or branches without a
  value are formula errors (Error 7 / Error 4).  Nested conditionals in
  a branch need parentheses; an unparenthesized `:` always belongs to
  the outermost conditional.  `;` inside an entry types a statement
  separator (the `;` command menu stays available when idle).

## The expression language: references, values and ranges
## (user-extended 2026-11, branch ref-val — not in the manuals)

The evaluator has a real type system: Number, String, Boolean,
**Reference** and **Range**.  The rule that changes everything:

> **A bare cell identifier is a REFERENCE value, not a number.**
> `A1`, `B5`, `AA20` all evaluate to references.  The contents of a
> cell are retrieved explicitly with `VAL()`.

References are *lightweight* — they name a cell, they do not read it.
Wherever a value is consumed, a reference **coerces** to that value
(option 3, chosen for compatibility): a numeric cell yields its
number, a TEXT cell yields its content as a string, and an empty or
error cell raises **Error 1** — never a silent zero.

### VAL(cell) — the typed value access

```text
VAL(A1)                 the number stored in A1
VAL(REF(2,5))           the number in B5
VAL(A1)+VAL(B1)         sum of two cells
VAL(A1)*10              scale a cell value
SQRT(VAL(A1))           any numeric function accepts VAL(...)
VAL(REF(1+1,2+3))       fully dynamic: the value of B5
```

`VAL` needs exactly one cell reference.  `VAL` is **typed**: a numeric
cell yields its number, a TEXT cell yields its content as a string,
and an empty, allocated or error cell is **Error 1** — decide in a
conditional instead:

```text
VAL(A1)>0:"filled":"empty"
```

### Strings, characters and `&` (user-extended 2026-11-13)

String values exist inside formulas: a quoted literal, `VAL` of a text
cell, the result of a conditional, and the two character functions:

```text
CHR(n)                  the character with code n (32..127 only;
                        0, negative, fractional or out of range -> ?n?)
ORD(s)                  the code of a string of length EXACTLY 1
                        (another length, a non-printable byte, or a
                        number argument -> ?n?)
CHR(72)&CHR(73)         "HI"        & concatenates two strings
VAL(A2)&"!"             appends to a text cell's content
("a"<"b"):"yes":"no"    the six comparisons work on strings
                        (byte order, case-sensitive)
```

A formula whose result is a string **is a text value**: the window
shows it left-justified, the type line reads `text: left justified`,
`VAL` and a bare reference yield the string, the range functions skip
it like text, and `;R` re-evaluates it like any formula (a numeric
result flips the cell back to numeric).  The string result is saved
in the `.csd` file: a loaded sheet shows the string immediately, no
`;R` needed.

The **strict mixing rule**: a string and a number in ONE operation is
Error 6 (`"abc"+1`, `VAL(A2)*2` over a text cell, `"abc"=5`, `"a"&7`)
— through every path, including direct function arguments
(`SUM("abc",1)` is an error).  The one documented exception stays:
text INSIDE a referenced range is ignored by `SUM`/`CNT`/`AVG`/
`MAX`/`MIN`/`MED`.

Grammar note: inequality is spelled `<>` (the manual's operator).
`!` after a value is the factorial postfix (`5!`), never a `!=`.

### REF(col,row) — construct a reference

Coordinates are **one-based** numbers:

```text
REF(1,1)                is A1
REF(2,5)                is B5
REF(3,10)               is C10
REF(1+1,2+3)            is B5 (expressions allowed)
REF(A1,5)               column taken from A1's value (coercion)
REF(VAL(B1)+1,VAL(B2))  fully dynamic addressing
```

Both arguments must be **integers**.  Rejected with an error:
fractional coordinates (`REF(1,1.5)` → Error 0), zero or negative
(`REF(0,1)` → Error 1), outside the sheet (columns 1..127 = A..DW,
rows 1..255 — `REF(128,1)`, `REF(1,256)` → Error 1), wrong types
(`REF("x",1)` → Error 6), wrong argument counts (Error 3).

### Empty referenced cells are zero

A bare reference to an empty (or allocated-empty) cell yields **0** in
arithmetic — the manual's rule, and what makes relative copies across
empty regions produce numbers instead of errors.  The strict "never
silently 0" contract lives in `VAL()` (`VAL(empty)` is still an
error), in the factorial entry's `?n?`-until-filled prefix, and error
cells always stay errors.

### ROW() and COL() — the current position

`ROW()` returns a row number, `COL()` a column number, both
1-indexed.  **In a stored formula they answer the cell's own
position** — `+ROW()` in B7 is 7, survives `;R`, and a copy of it
down a column yields 2, 3, 4, ...  Asked context-free (the
`?`-calculator, the dialogs) they answer the current cursor
position.  Arguments are an error.

### COLIDX(x) — the column index

The 1-based column number (what `REF`'s first argument expects).  The
argument is a column designator — a bare `A`..`DW` word, a quoted
designator, or any reference:

```text
COLIDX("A")             1
COLIDX("AB")            28
COLIDX("DW")            127
COLIDX(AB)              the bare designator (COLIDX only)
COLIDX(REF(2,3))        2
COLIDX(A1)              1   (the row is ignored)
REF(COLIDX("AB")+1,5)   AC5 - arithmetic on the index
```

Unknown designators (`"DX"`, `"ZZ"`, `"A1"`, empty) are Error 1,
wrong types Error 6, a missing argument Error 3.

### Dynamic ranges: `>` between references

`>` builds a **Range** when both operands are references; between
numbers it compares as always:

```text
A1>A5                   the range A1..A5
REF(1,1)>REF(1,5)       the same range, built dynamically
A1>REF(1,5)             mixed spellings work
REF(1,1)>A5
B1>B3                   a horizontal range
```

All four spellings feed the range functions identically.  Reversed
ranges normalize (`A5>A1` ≡ `A1>A5`).  A range is a **rectangle** —
`A1>E5` addresses the whole block (so `SUM(A1>B5)` sums the 5x2
rectangle); only `REGR` demands a one-dimensional series (Error 2).
A chain like `A1>A5>A9` is Error 6.

### Range functions

`SUM`, `CNT`, `AVG`, `MAX`, `MIN` take any mix of ranges, numbers and
references through the same evaluator; text entries inside a range are
skipped, empty cells are skipped, an error cell inside a range is
Error 0:

```text
SUM(A1>A5)                      plain
SUM(REF(1,1)>REF(1,10))         dynamic
AVG(REF(2,1)>REF(2,10))         average of B1..B10
CNT(A1>REF(1,10))               mixed spelling
MIN(REF(1,1)>A10)   MAX(A1>REF(1,10))
SUM(A1>A5,B1>B3)                multi-area: both ranges add up
CNT(A1>A5,B1>B3)                -> 8
MED(A3>B6)                      the median (user-ext 2026-11-13)
```

`MED(range)` — the median, with the same value rules as the family
(text and empty entries are skipped; an error cell in the range is
Error 0; multi-area lists and direct numbers are allowed).  The
values sort ascending with the BCD compare: an **odd** count yields
the middle element, an **even** count the average of the two middle
elements (Excel-style), and a range with no numbers gives 0 (like
`AVG`):

```text
MED(A3>B6)          1,5,4,2 in A with text in B3  ->  3
MED(A3>B4,A6)       1,5,3,2  ->  (2+3)/2 = 2.5
MED(A3>A3)          a single value is its own median
```

The regression set takes a range plus one anchor reference and keeps
its results in `SLOPE()`, `PROJ(x)` and `DEPD(y)`:

```text
REGR(A1>A10,B2)         x = A1..A10, y starts at B2
REGR(REF(1,1)>REF(1,5),B1)      dynamic spelling
```

### Comparisons are not ranges

With references on both sides, `>` builds a range; to compare values,
make the operands numbers with `VAL`:

```text
VAL(A1)>VAL(A5)         ordinary comparison -> Boolean
VAL(A1)<VAL(A5)  VAL(A1)=VAL(A5)
A1=A5                   dereferences both: compares the values
```

In a conditional's condition the same rule applies — a two-reference
`>` would build a range, so write `(VAL(A1)>VAL(A5)):…`.

### SET and RECALC with references (in conditionals)

`SET` assigns to a cell; the first argument must be exactly one
reference (a range is Error 6), the second is any value:

```text
SET(A1,100)                     plain target
SET(REF(2,5),200)               reference built by REF
SET(REF(VAL(B1),VAL(B2)),200)   fully dynamic target: the cell at
                                column B1's value, row B2's value
SET(A2,VAL(B1)*2)               computed value
SET(A1,B2)                      copies B2's number OR its text
SET(A1,"\rTotal")              text, right-justified (\l left, \c center)
```

Side effects live in conditional branches (semicolon-separated; the
branch needs exactly one value expression at the end, and only the
selected branch executes):

```text
(1):SET(REF(C1,C2),VAL(B7));0:1
     always true: B7's value lands in the cell addressed by C1/C2;
     the formula's own cell shows 0 (1 in the false branch)

(VAL(A1)>0):SET(A1,VAL(A1)-1);RECALC();0:"finished"
     a countdown: A1 counts down to 0, then the branch value flips
     the cell to the text "finished"

1:SET(REF(2,1),VAL(A1)*2);RECALC(REF(2,1));0:"ok"
     write and request recalculation of the target
```

`RECALC()` requests recalculation passes; the drivers (entry commit,
`;R`) iterate whole-sheet passes until nothing changes, the
10000-pass limit (raised for long state machines), or ESC.  The
change detection is honest: an SET that writes the *same* value does
not keep the iteration alive, so an always-true SET branch like the
first example settles immediately under `;R A`.  Two formulas
fighting over one cell (one writes 20, the other writes 19) hit the
limit and show `Recalculation limit reached`.

**SET preserves formulas** (2026-11-13): a numeric SET into a cell
whose entry is a formula (an expression, not a plain number literal)
updates the value and keeps the entry text — a state machine whose
counter targets its own cell keeps its program:

```text
1:SET(A1,VAL(A1)+1);RECALC();0:0        committed in A1 itself
                                        the formula survives every pass
```

A SET into a plain number (or an empty/text) cell still rewrites the
cell as a plain value, exactly as before.

### Dynamic ranges in the command dialogs

The coordinate prompts (delete `;D R/C/F` From-To, the copy corners,
Goto, print corners, load position) try the plain coordinate text
first and then evaluate an expression:

```text
;D F   Delete (From>To):  A1>REF(A1,B1)
       with A1=B1=5 this addresses the rectangle A1..E5 and clears it
;D R   Delete rows (From>To):  REF(1,C2)>REF(1,C3)
       deletes the rows of that range (C2, C3 hold the row numbers)
Goto>  REF(2,4)                moves the cursor to B4
```

Plain coordinates, `>`-prefill and `@` keep working (the static text
wins).  Note that the dialog expressions run the full evaluator — a
conditional with side effects typed into a prompt executes.

### Digits beyond the 18 mantissa digits are **truncated, never rounded**
(the manual: "Numbers are not rounded but are truncated (cut off)").
Nothing over the exponent range triggers the manual's `math op error -
numeric overflow` (evaluation Error 0, shown as `?n?`); values that
underflow collapse to zero.  **Inline comments** (manual II.18):
everything from a `\` to the end of an entry is non-evaluative:
`+SUM(A1>A5) \This is a comment` — kept in the contents, invisible to
the sheet, printouts and every reference scanner.  Cells and the save
format (CSD4) hold the BCD values as-is; older magics are refused with
the manual's `READ ERROR`.

### The aggregation functions (SUM/CNT/AVG/MAX/MIN/MED)

`SUM`, `CNT`, `AVG`, `MAX` and `MIN` are **streaming**: they keep only
running totals, no value array — a range of any size (up to the entry
budget) aggregates in constant memory.  `MED` keeps a bounded buffer
of **512 values**; a range beyond that returns an error rather than a
silently truncated median.

### Sorting: the `;#` command (user-extended 2026-11-13)

Reorders the rows of a rectangular range by one key column.  Not shown
in the command panel or the original help screens (they stay
byte-identical to the 1982 interface); documented here.

```text
;#
Range?          A1>A5        (static) or REF(1,1)>REF(1,5) (dynamic)
Key (Column)?   A            (bare designator) or COLIDX(2)/COLIDX(A)
Order (A/D)?    A            (ascending) or D (descending)
```

- The range's **rows** are reordered, each row carrying its cells
  within the range's columns (a single-column range sorts just that
  column).
- Comparison: numbers by value, text by string order, numbers before
  text on mixed keys; **empty keys sort last** in both orders; equal
  keys keep their relative order (stable).
- **Formulas follow the cells**: every formula's references into the
  sorted block are rewritten to the rows where the cells landed —
  `+VAL(A4)` becomes `+VAL(A6)` when row 4's content moved there.
  Frozen `!`-references are not remapped, and references inside
  `\`-comments or `"..."` strings are never touched.  Cached values
  refresh on `;R` as usual (the recalculation stays manual).
- ESC cancels at any of the three prompts; an invalid Order key
  re-prompts.

Example (the key column is A, ascending):

```text
before:  A3=1 Apples   after:  A3=1 Apples
         A4=5 Oranges          A4=2 Tomatoes
         A5=4 Berries          A5=4 Berries
         A6=2 Tomatoes         A6=5 Oranges
```

### The factorial entry: `5!` (user-extended 2026-11-13)

An entry **ending with `!`** whose prefix is a valid expression
becomes a factorial cell: the window shows FACT of the prefix's
value, the **content keeps the entry verbatim**, and the cell
**recomputes on `;R`** (so `A5!` tracks A5).  The rule is a syntax
check — a source that isn't a valid number yet shows `?n?` until it
is, and non-expression entries (`STOP!`, `5!!`) stay text.  Inside
regular formulas `!` keeps the manual's frozen-reference meaning.

```text
5!              displays 120, content stays 5!
A5!             FACT of A5's value; tracks A5 through ;R
REF(2,5)!       FACT of the referenced cell's value
```

### Integer and combinatorial functions (user-extended 2026-11-13)

```text
ABS(-5)         5            (existing)
SGN(-3)         -1    SGN(3) = 1    SGN(0) = 0   (alias of SIGN)
INTG(3.5)       3            the integer part (truncate toward zero)
FRAC(3.5)       0.5          the fractional part, sign-preserving:
FRAC(-3.5)      -0.5         FRAC(x) = x - INTG(x)
NCR(5,2)        10           combinations (alias of COMBIN)
NPR(5,2)        20           permutations (alias of PERMUT)
```

The combinatorial caps preserve BCD precision: `NCR(30,2)` overflows
to `?n?` at n ≥ 25, like `FACT` for integers.

**Fractional factorials**: `FACT(x)` for non-integer x is the Gamma
function — `FACT(x) = Γ(x+1)`, computed with the Lanczos approximation
(g = 7, 9 terms) entirely in BCD arithmetic (the engine's `pow`,
`exp`, `sqrt` and `π`).  `2.2!` in a cell works the same way:

```text
FACT(2.2)       2.4239654799353...   (= Γ(3.2))
FACT(0.5)       0.88622692545275...  (= Γ(1.5) = √π/2)
2.2!            the same, as an entry
```

Precision note: the Lanczos coefficients limit the result to ~15
correct digits (the display's 14 are safe); negative and
overflow-sized arguments give `?n?`; the integer path keeps its cap
(`FACT(25)` → `?n?`).

**INVFACT(y)** — the inverse factorial (user-extended 2026-11-13):

```text
INVFACT(120)             5     (exact integers walk down the factorial)
INVFACT(119.5)           4.9975523876792...
INVFACT(50)              4.4715274582078...
INVFACT(FACT(2.2))       2.2   (the round trip is exact to the display)
INVFACT(1e400)           210.8499606194...
```

Non-integer arguments solve `ln Γ(x+1) = ln y` with NEWTON's method in
log space — the derivative is the analytically computed digamma
function, and the iteration runs until the BCD noise floor is
reached, so the result carries the full display precision.  The
log-space formulation means the whole exponent range works (no huge
intermediates).  Below the Gamma minimum (y < 0.8856...) there is no
solution (`?n?`), and the ascending branch is returned for the
two-sided interval just above it.  `INVFACT(0)` and negative
arguments are `?n?`.

- **Differential harness:** `tests/precision_ref.c` + `tests/precision_ref.py`
  dump every transcendental function at 38 decimals and compare against a
  150-digit Python-decimal reference (`python3 tests/precision_ref.py
  /tmp/eng.txt`); the engine agrees to ≤ ~7e-30 relative everywhere.
- **Form mode (`;F ... F`, manual 11.3) and the `;A` auto course**: a flagged
  cell shows `(form mode set)` after the type text - e.g.
  `type: (empty, but allocated) (form mode set)` on an empty stop cell,
  `type: numeric (form mode set)` once data is in. The flag survives
  commits, `;D E`, copy, and save/load (CSD4); it is reset only by
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
- **The Euler number**: `E()` — the constant at the full 40-digit
  internal precision (the same digit budget as `PI()`), so
  `LN(E())` = 1 and `E()*E()` = 7.3890560989306.  The engine's own
  `EXP(1)` agrees with the constant to ~28 decimals (the constant
  carries more digits than the exp series rounds).
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

## A brainfuck interpreter built from formulas (user-extended 2026-11-13)

There is no brainfuck command — the interpreter is a fixed block of
17 formula cells that you paste once, plus a program row and a data
column.  It is a proof of concept for the string/typed-value batch:
`CHR`/`ORD` do the I/O, `VAL`/`REF` address the tape dynamically, and
the iterated recalculation (`;R A`) is the clock — **one instruction
per pass**, the user watches the values change live.

### Sheet layout

```text
row 1   B1 pc        the program pointer (the column of the current
                     program character; starts at 2 = column B)
        D1 ptr       the tape pointer (the tape row, starts at 1)
        F1 mode      0 run, 1 skip-fwd, 2 skip-back, 3 halted
        H1 depth     the bracket depth while skipping
        J1 out       how many characters have been printed
        K1 in        how many input characters have been read
        L1 len       the program length (a constant you set)
row 3   B3..         the program: ONE character per cell, columns
                     B, C, D, ... (type them as text!)
row 5   B5..R5       the machinery (17 formulas, below)
col A   A1..A250     the tape: seed the cells you will use with 0
col S   S7           the output cell: every `.` APPENDS one
                     character to it (seed it with the empty-string
                     formula `+""` before the run) - you watch the
                     text build up: "H", "He", "Hel", ...
col T   T20..        the input characters, one per cell (read by `,`)
```

Column widths: make the columns you watch wide enough (A, B and S at
least; `;W`/`;F` adjust them).

### The machinery (row 5, columns B..R)

Every formula starts with `+` (numeric entry).  They are evaluated
left to right once per pass; exactly one cell acts per pass.  The
five state cells B5, C5..H5 etc. are never copied — the formulas
address the state cells absolutely.

```text
B5  +(VAL(B1)<=VAL(L1)+1):ORD(VAL(REF(VAL(B1),3))):0
C5  +(VAL(F1)=0)*(VAL(B5)=43):SET(REF(1,VAL(D1)),VAL(REF(1,VAL(D1)))+1);SET(B1,VAL(B1)+1);0:0
D5  +(VAL(F1)=0)*(VAL(B5)=45):SET(REF(1,VAL(D1)),VAL(REF(1,VAL(D1)))-1);SET(B1,VAL(B1)+1);0:0
E5  +(VAL(F1)=0)*(VAL(B5)=62):SET(D1,VAL(D1)+1);SET(B1,VAL(B1)+1);0:0
F5  +(VAL(F1)=0)*(VAL(B5)=60):SET(D1,VAL(D1)-1);SET(B1,VAL(B1)+1);0:0
G5  +(VAL(F1)=0)*(VAL(B5)=46):SET(REF(19,7),VAL(REF(19,7))&CHR(VAL(REF(1,VAL(D1)))));SET(J1,VAL(J1)+1);SET(B1,VAL(B1)+1);0:0
H5  +(VAL(F1)=0)*(VAL(B5)=44):SET(REF(1,VAL(D1)),ORD(VAL(REF(20,VAL(K1)+20))));SET(K1,VAL(K1)+1);SET(B1,VAL(B1)+1);0:0
I5  +(VAL(F1)=0)*(VAL(B5)=91)*(VAL(REF(1,VAL(D1)))=0):SET(F1,1);SET(H1,0);0:0
J5  +(VAL(F1)=0)*(VAL(B5)=91)*(VAL(REF(1,VAL(D1)))<>0):SET(B1,VAL(B1)+1);0:0
K5  +(VAL(F1)=0)*(VAL(B5)=93)*(VAL(REF(1,VAL(D1)))<>0):SET(F1,2);SET(H1,0);0:0
L5  +(VAL(F1)=0)*(VAL(B5)=93)*(VAL(REF(1,VAL(D1)))=0):SET(B1,VAL(B1)+1);0:0
M5  +(VAL(F1)=1)*(VAL(B5)<>93):SET(H1,VAL(H1)+(VAL(B5)=91));SET(B1,VAL(B1)+1);0:0
N5  +(VAL(F1)=1)*(VAL(B5)=93):SET(H1,VAL(H1)-1);SET(B1,VAL(B1)+1);0:0
O5  +(VAL(F1)=1)*(VAL(B5)=93)*(VAL(H1)=0):SET(F1,0);0:0
P5  +(VAL(F1)=2)*(VAL(B5)<>91):SET(B1,VAL(B1)-1);SET(H1,VAL(H1)+(VAL(B5)=93));0:0
Q5  +(VAL(F1)=2)*(VAL(B5)=91):((VAL(H1)=1):SET(F1,0);0:SET(H1,VAL(H1)-1);SET(B1,VAL(B1)-1);0):0
R5  +(VAL(B1)>VAL(L1)+1)*(VAL(F1)<>3):SET(F1,3);0:0
```

How it works: B5 fetches the current program character's code
(guarded so an overshoot reads 0 instead of erroring).  C5..H5 are
the run executors for `+ - > < . ,`; I5..L5 handle `[`/`]` (a false
`[` enters skip-forward, a true `]` enters skip-back, both with the
depth counter H1); M5..Q5 are the skip steps (one character per
pass, bracket-depth aware — a nested conditional in Q5's value
position does the "land on the matching bracket" case); R5 is the
halt detector: when the pc runs past the program the mode becomes 3,
every condition goes false and `;R A` **settles** by itself.

### Running a program

1. Set the state: B1=2, D1=1, F1=0, H1=0, J1=0, K1=0 and L1=the
   program length.
2. Seed the tape: enter 0 into A1..A60 (however many cells you use).
3. Type the program characters into row 3, columns B.., one per
   cell, as TEXT.
4. Type (or paste) the 17 machinery formulas into row 5.
5. `;R A` — one instruction per pass; the machine stops by itself.
   Read the output down column S and the tape in column A.

`+++[>+++<-]` leaves 9 in A2.  `++++++++[>++++++++<-]>++++++++.`
prints `H`.  The classic `++++++[>++++++++<-]>+.` prints `1` — the
interpreter passes all of these in the test suite.

Constraints: the brackets must be balanced (an unbalanced program
walks pc to the end and stops via the halt cell); `.` needs the tape
byte in 32..127 (`CHR`'s domain); `,` needs enough input characters
in column T (running out stalls the machine with an error cell);
the tape is 250 rows deep and the output column is 255 rows — a
long-running loop can hit the 10000-pass recalculation cap, which
shows `Recalculation limit reached` and leaves the machine's state
intact for another `;R A`.

### Running "Hello, World!"

A ready-made sheet ships in `examples/brainf.csd` — load it and
recalculate:

```text
./build/calcstar examples/brainf.csd      ;R A       ;G S7
```

To build it by hand instead, follow the recipe below.

The classic program, minus its final `.` (the trailing newline byte
10 is outside `CHR`'s printable 32..127 domain):

```text
++++++++[>++++[>++>+++>+++>+<<<<-]>+>+>->>+[<]<-]>>.>---.+++++++..+++
.>>.<-.<.+++.------.--------.>>+.>++
```

Cell by cell:

1. **Seed the tape**: 0 into A1..A10 (type `0`, RETURN, `^X` down,
   repeat — the tape is a column).  Then seed the output cell:
   `;G S7`, enter `+""` — a formula whose value is the empty string,
   so the first `.` has something to append to.
2. **State** (`;G B1`, then type, RETURN, `^D` right to the next
   state cell): B1=2, D1=1, F1=0, H1=0, J1=0, K1=0, L1=105 (the
   program length).
3. **The program**: `;G B3`, then ONE character per cell as text:
   type the character, RETURN (the cursor stays), `^D` right, repeat.
   A single `+`, `-`, `.`, `,`, `[`, `]`, `<` or `>` entry is stored
   as text automatically — no toggle needed.
4. **The machinery**: `;G B5`, then the 17 formulas from the table
   above, one per cell (type, RETURN, `^D`).
5. `;R A` — one instruction per pass, ~1770 passes; the machine
   halts itself (F1 turns to 3) and `;R` settles.  Watch cell S7
   build up while it runs: "H", "He", "Hel", ..., "Hello World!"
   (each `.` concatenates one character onto it).

The run takes a few seconds; you watch the tape and output fill in
live, one pass at a time.  (Other keys typed while the machine runs
are not lost — they stay queued and act when it settles.)

## Internals: the sparse cell store (the DOS branch, 2026-11-14)

The dense 255x127 cell matrix is gone.  Cells live in a **chunked
pool** (250-slot chunks, allocated on demand, each chunk well under
64 KiB — the 16-bit-portability rule the design keeps) plus a
**per-row sorted index** of `{col, ref}` entries.  Memory follows
populated cells only; the full coordinate space (255 rows, 127
columns) still works.  The lookup/allocation distinction is explicit:

```text
cs_getcell(col,row)     NULL = absent (reads treat absent as empty)
cs_alloc_cell(col,row)  allocates when absent, reuses otherwise
cs_free_cell_at(col,row) releases the cell and its owned strings
cs_relink_cell(...)     moves a slot between coordinates (shifts)
cs_next_cell(&c,&r)     ordered scan, row-major coordinate order
cs_clear_all()          releases everything
```

The entry budget is `CS_BUDGET` (4000 — a flagged deviation from the
emulated 2000, affordable on the 386 target): 16 chunks of 250 slots,
about 440 KiB worst case, and the index stays bounded by the budget,
not by the coordinate space.  `;?` shows the room; the low-memory
warning scales with the budget.

## Startup banner and build number

The application starts with the message

```text
CalcStar 1.5, release under the terms of the MPL-2.0 license (build N)
```

painted inverse on the status row; the FIRST keystroke clears it (that
key acts normally).  `N` is the build number: a counter in
`build.count` that increases on every source-changing build (the
generated header `build/buildno.h` depends on the sources, so a no-op
`make` does not touch it and `make clean` keeps the count).  The
version string lives in `CS_VERSION` (cs.h).  The license text is
`LICENSE` (MPL-2.0).

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
[filename  ]  cursor:    B3      current:    B3    L-R
   (the box shows the loaded file's basename without its extension,
    truncated to 10 characters - `examples/brainf.csd` shows
    `brainf    `)
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

Coordinates accept **upper or lower case** everywhere they are typed —
`Goto>`/TAB, copy ranges (`a1>b9`), save/load corners, and formulas
(`+b2*10` evaluates; stored coordinates are uppercased for display).
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
- Delete (`;D`: `A)ll R)ow C)olumn F)rom-To E)ntry`) checks dependencies
  first: `ERROR: would delete ref(s) at ...`, with clear-all requiring
  `verify Y/N`.  Rows and columns prompt `Delete rows (From>To)` /
  `Delete columns (From>To)`, prefilled with the current row/column: a
  leading `>` keeps the prefill (`2>5` from prefill `2`), other keys
  replace it; `1>5` deletes rows 1..5 inclusive, `A>D` deletes columns
  A..D, and a full coordinate range also works (`A1>C3` = rows 1..3 /
  columns A..C — only the part relevant to the operation is taken).
  The `F)rom-To` option (`Delete (From>To)`) clears every cell in a
  rectangle (`@`-aware) like `;D E` per cell — no dependency check, but
  it asks `verify Y/N` (user-extended 2026-11, not in the manual).
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
