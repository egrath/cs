# AGENTS.md — guidance for AI agents working on the CalcStar clone

A faithful console clone of MicroPro's **CalcStar** spreadsheet (User's Manual,
first issue November 8, 1982), written in **pure C89** with curses.
Fidelity is the product: behavior, layout, keystrokes and message strings
follow the original. Read this file before changing anything.

## Read first

1. `.opencode/opencode.txt` — session memory. It records what is done,
   verified, and the traps already debugged. **Append a summary of every
   batch you complete** (what changed, how it was verified, new harness
   gotchas).
2. **Both manuals** (console: two OCR transcriptions, neither is
   authoritative alone — cross-check them; sometimes one is more clear
   and/or exhaustive; the second manual's error tables are the cleanest):
   - `notes/CalcStar_1982.txt` — the 1982 OCR (clean, 11086 lines).
     Verified anchors:
   - Comment function (`\`) 5.10 (~line 2017) + appendix (~6905)
   - Truncation rule 4.2 (~1401): "truncates, does not round"
   - System-function list ~7210; `**` power note ~7385
   - `?`-calculator: answers contain **12 decimal places** (4.1/4.2)
   - Appendix error lists ~7513 (`Prec. must be 0 - 12`, `ERROR: would
     delete ref(s) at <coord>`, ...)
   - `notes/CalcStar_1981.txt` — the older 1981 transcription; keep it as
     a co-primary source (its error-message list is more exhaustive and
     less garbled, and its evaluation-error table matches the parser's
     internal codes). Where the two disagree, cross-check both.
3. `/Users/egon/tmp/cs1.txt` — the genuine 80x24 screen capture. The idle
   screen must remain **byte-for-byte identical** (after tab expansion and
   trailing-space normalization).

## Identity & hard invariants

- **No floating point anywhere.** All math is the BCD engine. A value is
  `0.d1d2... * 10^exp10` with up to 40 digits (30 calculation + 10 guard;
  engineering extension flagged 2026-11: the original carried 22 digits
  and the manual promises only 14), exponent range ±999. Arithmetic
  **truncates**, never rounds; transcendental series results are rounded
  at the 30th digit (`round30`) so boundary-exact values (SIN(30°) = 0.5)
  come out clean and long chains keep ~24+ digits. The on-disk format is
  pinned to the original's 22 digit bytes (`CSN_FILE_DIGS`, CSD3) — wider
  mantissas truncate on save.
- **Visible precision is user-extended to 24 decimals** (`CS_MAX_P`,
  flagged 2026-11: manual Appendix C says `0 - 12`). The `?`-calculator
  follows at 24 too (manual 4.1 pinned 12). 24 sits inside the 30-digit
  honest core with 6 digits of margin, keeping the "display truncates
  and never shows a digit above the true value" guarantee.
  Mind the units: visible precision counts DECIMAL PLACES, the honest
  core counts SIGNIFICANT digits — honest decimals = 30 − (integer
  digits of the value); the 24 cap is safe for values with up to 6
  integer digits.
- **Standing deviation policy (user, 2026-11): deviations from the
  manuals are allowed when they stay faithful in spirit and clearly
  benefit the user — flag every one (here or in the flagged-extension
  lists), never silently.**
- **Coordinate input is case-insensitive** (user request 2026-11):
  `cs_parse_coord` folds `a-z` (Goto, TAB, copy ranges, save/load
  corners).  The formula parser and the reference scanners already
  folded case; the formula normalizer stores coordinate words
  UPPERCASED in `cont`, so the display and files are unaffected.
- **`;D` range deletes + F)rom-To are user-extended 2026-11** (the
  manual deletes ONE row/column per command; the menu string changed
  to `Delete: A)ll R)ow C)olumn F)rom-To E)ntry`): R/C prompt
  `Delete rows (From>To)` / `Delete columns (From>To)`, prefilled with
  the current row/column (`1>5`, `A>D`; a full
  coordinate range is accepted and only the relevant part is used —
  `A1>C3` = rows 1..3 / columns A..C; ALWAYS the full form, `>5` with
  an omitted left side is `bad range coord`; reversed ranges
  normalize).  A leading `>` keeps the prefill (cs_prompt_range /
  cs_prompt_coord setmode 2), other keys replace it.  Dependencies are
  checked per line over the whole range
  (merged `ERROR: would delete ref(s) at ...` message — the same
  refuse-behavior the single-line delete always had, kept unchanged
  per the user).  The new F)rom-To clears a rectangle like `;D E` per
  cell: no dependency check, `verify Y/N` only (user decision).
  `cs_delete_rows(r1,r2)` / `cs_delete_columns(c1,c2)` /
  `cs_clear_range(...)` are exported; `cs_shift_refs_n` is the
  delta-based ref shifter (refs beyond the range shift by −n).
- **Side effects + extended conditionals + iteration are user-extended
  2026-11** (the biggest deviation; not in the manuals): SET(cell,value)
  / RECALC() / RECALC(cell) execute as statements in conditional
  branches; branches are statement lists (`;` separators, exactly one
  value piece, executed in order, only the selected branch runs — the
  parser is a direct evaluator, so the unselected branch is
  skip-scanned via branch_end() and parsed from its own sub-buffer).
  RECALC is a REQUEST: the drivers (cs_run_recalc_passes in eval.c —
  `;R`, entry commit, ?-calculator) iterate whole-sheet passes until a
  pass changes no stored value, the 1000-pass limit (CS_MAX_PASSES) or
  ESC (`User interrupted execution`; the limit shows
  `Recalculation limit reached`; the loop paints `Calculating ...
  (pass N)`; while iterating the window is repainted after every
  changed pass so the user watches the values update live (user
  request 2026-11) — the plain one-pass paths keep their single
  repaint).  An OVERRUN sets cs_limit_hit; the callers re-paint
  `Recalculation limit reached` as the LAST status action after their
  final repaint (the driver's own message would be wiped by them:
  recalc_cmd, entry_commit, the ?-calculator, the ;A course — user
  request 2026-11: the resting screen must show success or overrun;
  cs_recalc_limit_reset() clears the state at each driver context).
  The keyboard is NON-BLOCKING for the driver's duration:
  cs_kb_nodelay_set(1) ... (0) brackets the loop and cs_poll_escape
  restores the wanted mode (a hard-coded blocking restore once made
  every pass wait for an ENTER — user bug 2026-11-12).  cs_recalc_entry is the stability sensor: it compares the
  NEW stored value against the OLD one (2026-11 bugfix — it once
  compared a zeroed value, so any nonzero fixed point kept the passes
  running to the 1000 limit) and counts a changed string result
  (`strres`, e.g. "counting"→"done") as a change too.  The --test pins
  the user's exact settling sheet (A3=5, A4=10, C4 counts to "done").
  cs_mark_sheet_change marks SET writes.  SET's value is
  evaluated before the write; the justification escapes \r/\l/\c come
  inside the string; the target is a plain cell specifier and a normal
  reference (shifts on copy/insert/delete).  ';' typed inside an entry
  inserts the separator (the menu stays available when idle).  Error
  mapping: a side effect as an operand → Error 4 (unknown fn); wrong
  statement lists → Error 7; KW_ABORT (13) = the internal user
  interrupt (an aborted evaluation leaves the cell untouched, no
  sticky display).  Nested conditionals in branches need parentheses;
  the unparenthesized ':' belongs to the outermost conditional (the
  old nocond mechanics are gone — branch texts are sub-parsed in
  isolation).
- Extension functions (Batch A, flagged not from the 1982 manuals): `SIGN`,
  `MOD`, `POWER`, `FACT`, `COMBIN`, `PERMUT`, `GCD`, `LCM`, `DEGREES`,
  `RADIANS`, `ROUND`, `TRUNC`, `PI()` and the trig set. Integer caps
  preserve BCD precision: `FACT/COMBIN/PERMUT` give `?n?` at `n >= 25`,
  `GCD/LCM` at > 12 digits; `MOD/POWER/ROUND/TRUNC` are pure `csn_*` calls.
  Function names cannot contain digits (the name scanner stops at digits),
  so no `LOG10` — the manual's `LOG` is log10. Comma-delimited arg lists,
  ranges via `>`, per the manual.
- Idle screen == cs1.txt exactly. Verify after **every** batch.
- Cross-file APIs are `cs_*` (snake_case). File-local helpers are `static`.
- Prompts/messages draw on the **last screen row at x0**. After a system
  message the terminal cursor rests on the **edit line** (`place_cursor`) —
  during an entry after the typed text, otherwise at the idle position —
  never in the cells and never parked in the status bar. Input expected
  in the status bar (`Command > `, `Goto>`, `verify Y/N`, save/recalc/
  format dialogs) keeps its own cursor via `promptvis`. System messages
  (`Error 0-> ...`, `Form Control Mode`, `Auto complete`, `SAVED`, ...) are
  painted **inverse** (`cputa` + `A_REVERSE`); input prompts stay
  non-inverse.
- **`;W` shows the heading of the row and column** (user-fixed
  2026-11): `row, column  = <rowHeading>,<colHeading>` where the ROW
  heading is the cell in COLUMN A at the cursor's row and the COLUMN
  heading is the cell in ROW 1 at the cursor's column — always
  content-based (shown even when the heading row/column is on-screen;
  the earlier "visibility" reading was wrong — the user's A7 example
  decided it).  The two reads were SWAPPED before ("= A1,-" for an
  empty A7).  Content: text as typed, numbers in their column's
  display format (numeric headings are a flagged choice), input
  formulas as "In!", and "---" when the heading cell is empty (three
  dashes, flagged — the "at least" note used one).
- **Per-cell notation: (auto) / (dec) / (sci)** (user-extended
  2026-11, not in the manuals): `;F` gains `N)otation (%s)` showing the
  cell's state in small letters; N cycles `auto → dec → sci → auto`.
  `(auto)` is the manual's default (decimal, exponential fallback when
  too narrow, then `!n!`).  `(sci)` always renders scientific; a value
  even exponential cannot fit shows `!n!` (flagged choice).  `(dec)`
  always renders decimal: a value wider than the column keeps its head
  (the first width−1 characters) and shows `*` as the LAST symbol — the
  user knows the number is visually truncated and looks the full value
  up in the `contents:` line.  The attribute lives on the cell
  (allocating an empty cell like the per-cell precision does; the
  manual's "can't create" budget message) and survives commits,
  deletes, copy and SET writes like prec/formf; `cs_clear_all` resets
  it.  The cell-window renderer, the `;W` heading display and the
  printed reports all honor it; the `contents:` line stays the
  notation-independent value view.  SAVE: 2 bits packed into the spare
  high bits (5-6) of the existing per-cell prec byte — the CSD4 layout
  is UNCHANGED and old files load as (auto).
- **First-class cell references, VAL/REF and dynamic ranges are
  user-extended 2026-11 (branch "ref-val", merged)** — an architectural
  change to the expression language, NOT backward compatible with the
  old implicit-deref reading (the user waived compatibility; option 3
  semantics were chosen by the user):
  - The evaluator's `Val` carries an explicit type discriminator
    (`VT_NUM / VT_STR / VT_BOOL / VT_REF / VT_RANGE`) plus lightweight
    `CsRef {col,row}` / `CsRng {start,end}` values (0-based; no heap).
  - **A bare cell identifier evaluates to a REFERENCE value** (e.g.
    `A1`, `AA20`).  References coerce to values where a value is
    consumed (option-3 semantics, `val_deref()`): numeric cell -> its
    number, TEXT cell -> its content as a string (the old behavior),
    empty/allocated/error cell -> Error 1 (never silently 0).  `VAL()`
    is the explicit numeric accessor: VAL(numeric-cell) -> the value,
    VAL(empty/text/error) -> Error 1; VAL must receive exactly one
    Reference (a Range is Error 6).
  - `REF(col,row)` builds a reference from one-based numeric
    expressions (option-3: `REF(A1,5)` uses A1's value).  Rejects
    fractional (Error 0), zero/negative and out-of-limits (Error 1;
    columns 1..127, rows 1..255), wrong types (Error 6) and wrong
    argument counts (Error 3).
  - **`>` is overloaded in parse_cmp (one place)**: reference > reference
    constructs a Range (normalized; diagonal -> Error 2) decided BEFORE
    any dereference; numbers/bools compare.  `< <= = <>` compare values
    (references deref).  A chained `a>b>c` and a Range in a
    value/reference position are Error 6.  NOTE: a top-level formula
    derefs its result, so `+A1>A5` compares (the leading '+' means a
    numeric formula) while a range only survives as a function
    argument; a two-reference `>` as a CONDITION needs VAL()
    (`(VAL(A1)>VAL(A5)):x:y`) because `(A1>A5)` now builds a range.
  - Range-consuming functions (SUM/CNT/AVG/MAX/MIN, REGR) take general
    arguments through the one evaluator interface — the old per-function
    range-parse special case (parse_arg/AK_RANGE) is DELETED.  A Range
    argument walks its cells (numeric counted, error cell -> Error 0,
    empties/text skipped); `REGR(range, ref)` takes a Range and a single
    Reference anchor.  SET's target must resolve to exactly one
    Reference (Range -> Error 6, no deref on the target position); the
    value argument derefs (option 3: SET(A1,B2) copies B2's number or
    text).  RECALC()/RECALC(ref) keep the request-flag driver and its
    safeguards unchanged (the argument is advisory).
  - The statement machinery (`run_branch`/`side_effect_stmt`) is
    unchanged in shape; `side_effect_stmt` now PROPAGATES exec errors
    (returns 2 + *err — a long-dead `r == 2` check in run_branch came
    alive; failed SETs no longer silently do nothing).
  - Every operand/argument dereference happens at the point of use
    (lazy) — never eagerly after parse — so references survive to the
    operators.  Error mapping: type mismatch 6, diagonal range 2,
    REF out-of-limits/bad coordinate 1, fractional REF coordinate 0,
    unknown function 4, malformed call 3/7.
- **The edit field scrolls horizontally** (user-extended 2026-11): the
  caret is always at the buffer end, so `draw_editline_idle` /
  `place_cursor` show the SUFFIX that fits `cs_scrw() - EDITTEXT_X`,
  recomputed at every paint (a resized terminal re-windows the entry
  live — KEY_RESIZE already forces a full repaint).  The field is
  padded to its full width (`cputn`) so backspacing (which slides the
  window back toward the text start) leaves no ghost characters.
- The cell-cursor marks `>` `<` paint INVERTED (`cchra` + `A_REVERSE`).
- TAB / `;G` with a bad target answers `Bad coord` (capital B, user
  instruction; copy/print keep the manuals' lowercase `bad coord`).
- The verify prompts are `verify Y/N - ` (trailing " - ").
- `?` while entering evaluates the pending text WITHOUT committing:
  the status bar shows `=` plus a `%24s` field of the 12-decimal fixed
  answer (`"=%24s"`, no floating point) and the edit field clears.
  The manual's trailing-`?`-then-RETURN calculator stays for the `;A`
  course (cs_commit_entry, same format).
- `@` resolves IMMEDIATELY the cell the cursor is on: the coordinate
  text is appended to the entry, the cell cursor stays; a bare idle `@`
  starts an entry with the current coordinate (`entry_begin_ref`).
  RETURN commits at the ENTRY cell and the cursor returns there. No
  two-press "atpend" pending mode exists anymore - drops the old bug
  where `@E3` was stored as literal junk text.
- The "@" reference works in ALL coordinate prompts (user requirement):
  `cs_prompt_coord(pr, default, out, n, setmode)` in screen.c - @
  inserts the CURRENT cell-cursor coordinate (instantly repainted) and
  the arrows move the sheet cursor while the prompt is up (via the
  exported cs_move_cursor). setmode=1 for single-coordinate fields
  (Goto>, print corners, Load position - @ REPLACES the input),
  setmode=0 to APPEND for range fields (;C From/To coord so "B3>" +
  @ forms "B3>A5"). Name fields keep literal @ text (flagged choice).
- Recalculation is **manual**: `;R` prompts `Recalculate - A)ll or E)ntry`.
  Committing an entry only evaluates that entry.
- After a data entry commit the cell cursor **stays**; RETURN moves right
  only when the edit buffer was empty. A bare `?`-terminated entry is the
  calculator (no store, 24 decimals shown).
- System functions in entries need a leading `+`/`-`, else they are text
  (manual: functions without a leading + or - are text entries).
- `\` starts a non-evaluative comment: kept in `cont` (shown on the
  contents line), invisible in the window, printouts, and to every
  reference scanner (scan/adjust/shift/dep_check stop at an unquoted `\`;
  `"\"` inside strings does not start a comment).
- The sticky `Error <n>-> <expr>  hit space` line is cleared by **any** key;
  Space just restores the `edit: ` line, any other key also processes
  normally. Never leave the app unresponsive. `<n>` is the evaluation
  code from the manuals' error Appendix (`cs_commit_entry` returns it);
  values <= 0 fold to 0 (value range: overflow / divide-by-zero /
  function-can't-evaluate).
  Format (user-verified against the original): code 0 paints
  `Value range ERROR 0-> <expr>? hit space [<coord>]`, other codes
  `Error <n>-> <expr>? hit space [<coord>]` (`cs_sticky_err` in eval.c);
  the 1981 manual shows the [cell] suffix (`Error 7-> ... [B4]`).
  The `;R` Recalculate fills a queue (`cs_errlist`/`cs_nerrlist`, in
  recalc order) and Space replays the ?n? cells ONE AFTER ANOTHER;
  a commit error is a one-message set (no queue).
  Inside the `;A` course there is no hit-space confirm (Space is data).
  Evaluation codes are 0..9, 12, 255 (255 = illegal value in REGR).
- Directional cursor semantics (manual 14.3): idle RETURN moves right, or
  **down** when the `;O` indicator is T-B, and wraps from column DW to the
  first column of the next row; `^Z` is L-R-mode aware (col A next row) and
  T-B-mode aware (row A of the next column).
- Locks (`;=`): the cursor marks the innermost edge of the locked area
  (manual: cursor at B3 locks rows 1..3 **and** columns A..B →
  `lockRows = curRow + 1`, `lockCols = curCol + 1`).
- Copy (`;C`) supports the manual's three shapes: cell → cell, **cell →
  range (repeat)**, range → matching range, with `R`(elative)/`N`(eutral)
  and `!`-frozen refs. After a copy, `cs_recount_entries()` keeps the
  entry pool count honest (`;? Room for` and the `MEMORY GETTING LOW`
  right-sided warning at ≤ 200 free entries depend on it).
- Save format is `CSD4` (magic "CSD4", then the saved range's origin col
  byte + origin row LE u16, per value: sign byte, exp10 LE i16,
  ndig byte, 22 digit bytes). Older magics (`CSD1`, `CSD2`, `CSD3`) are
  refused with the manual's `READ ERROR` — re-save to convert. The
  origin anchors the LOAD position (manual 14-9: the file's upper-left
  corner goes at the requested corner; All-mode origin = 0,0, so an
  All+load-at-A1 round trip is an identity).
- Preserved message strings (do not reword; cross-checked against BOTH
  manuals' error lists): `?n?`, `Error <n>-> %s  hit space`,
  `Room for %d entries`, `MEMORY IS TOO LOW`, `ERROR: would delete
  ref(s) at %s`, `bad coord`, `bad range coord`, `Width must be 3 - 63`,
  `Prec. must be 0 - 24` (user-extended range), `can't create`, `can't
  open` (print output),
  `can't open file`, `not ok`, `READ ERROR`, `no form flags`,
  `sizes don't agree`, `not along row/column`, `data too wide`,
  `bad form length`, `write error, not the same`,
  `write error <file> not found`, `write error end>start`,
  `write error bad coord`, `WRITE ERROR`, `WRITE CLOSE ERROR`,
  `Goto>`, `Command > `, `Recalculate - A)ll or E)ntry`, `verify Y/N`,
  `SAVED`, `Lock: R)ow C)ol B)oth`, `!n!` (cell token: column not wide
  enough),
  `-** Hit space to continue`, `-** End of report`.

## Coding style

- C89 only: declarations at the top of each block, `/* */` comments,
  `#ifdef KEY_*` guards for ncurses key codes, no `math.h`, no `double`.
- 4-space indent, small static helpers, `char buf[N]` + `strchr` /
  `memmove` for string work; display strings in the manual's English.
- Number formatting goes through `cs_fmt_num(const Csn *...)` in cells.c;
  the cell display truncates to the column precision and falls back to
  exponential form (e.g. `7.35E-6`) then to the `!n!` token when even
  that does not fit (manual: "column is not wide enough").
- Tests in `--test` (main.c) compare **exact decimal strings** with
  `ckc`/`cke` in the canon form `[sign]d.dddddddddddddE[+|-]e`
  (digits = `0.d1d2... * 10^E`)
  — beware: this is NOT the same E as the display's `d1.d2...E±e` form
  (display exponent = exp10 − 1). Mixing the two conventions has caused
  several false "bugs".
- Engine caution: `finish()`'s `baseExp` parameter means "place of the
  LAST digit of the digit string" and every call site must derive it
  itself (mul: `Ea+Eb-na-nb`; div: `na-(j0+L-1)+Ea-Eb-na+nb`; truncate:
  `exp10-n`; round30: `exp10-30`). Several historical bugs were wrong
  baseExp derivations.

## Building & testing

```
make clean && make        # per-file objects + binary in build/ (-std=c89 -pedantic -Wall, zero warnings)
./build/calcstar --test   # evaluator + save/load + engine battery
./build/calcstar [FILE]   # interactive; loads FILE at startup
```

Batch workflow (do all steps before reporting done):

1. `make clean && make` — zero warnings.
2. `./calcstar --test` — all pass. Extend `--test` with exact-string cases
   for anything you add (manual worked examples are preferred test data,
   e.g. `140%E4`=0.028, `(28**2)+(52**2)`=3488, `10*LOG(56/(12*10**-3))`
   =36.6900...).  Precision-era reference strings (40-digit engine,
   round30, verified against 60-digit decimal arithmetic and the live
   binary — pin these, never re-derive):
   - chain `+asin(acos(atan(tan(cos(sin(9))))))` in RADIANS: collapses
     to asin(sin(9)) with 9 rad in (pi/2, pi) → exactly 3*pi − 9 =
     `0.4247779607693797153879301498...`; canon `+4.2477796076937E+0`,
     fixed-24 `0.424777960769379715387930`.
   - `tan(atan(x))` round trip: `+TAN(ATAN(123456789.123456789))`
     fixed-24 `123456789.123456789000000000000000` (inverse trig
     publishes at 38 digits; was ...789000005056921 at round30).
   - `sqrt(2)`: fixed-28 `1.4142135623730950488016887242`; at prec 24
     `1.414213562373095048801688`.
   - `exp(1)`: fixed-30 `2.718281828459045235360287471350`; at prec 24
     `2.718281828459045235360287`.
   - π at prec 24 `3.141592653589793238462643`; `SIN(PI()/6)` /
     `COS(PI()/3)`: exactly `0.5` (round30 boundary values stay clean).
   - `SIN(1E-20)` = `ATAN(1E-20)` = `1E-20` exactly (radians; canon
     `+1.0000000000000E-19`); `SQRT(1E-998)*1E498` = `1` (relative
     Newton test).
   - cos is EVEN: `COS(-0.7)` = `COS(0.7)` = `0.7648421872844884…`
     (regression for the fixed sign bug).
   - Differential harness: `tests/precision_ref.c` + `tests/precision_ref.py`
     (25 inputs × 9 functions at 38 decimals vs a 150-digit decimal
     reference, RADIANS throughout; tolerance 2e-29 relative — see the
     file headers).
3. Interactive pty check of the affected UI paths with an `expect` script.
4. The cs1.txt idle diff (see recipe in `.opencode/opencode.txt`).
5. Update `.opencode/opencode.txt`.
6. **Commit the finished work to git with a meaningful message**
   (batch-level: what changed and why), **then push to `master`**.
   Check `git status --short` first so only intended files land in the
   commit; scratch fixtures (t_*.csd, pty files) must never be committed.

## expect/pty harness quirks (learned the hard way)

- One `send` per `expect` pattern (e.g. `expect "cursor:    E3"`); batched
  sends are flaky. Spawn **from the project directory**.
- Drain with `expect timeout` — `sleep` blocks the Tcl event loop. Keep
  **two** drains after the final keystroke: a log truncated mid-doupdate
  renders as a broken frame (`edit:` missing) and looks like a bug.
- Replay logs with `python3 /tmp/render.py [WxH] rawfile` (WxH optional,
  default 80x24).
- Tcl pitfalls: `\\?` sends a literal backslash (the comment function!);
  sending `" "` naked starts a TEXT entry that swallows following keys;
  `;S` needs three answers (name, password CR, `A` in `P)artial or A)all`).
- Save dialog: name → password (CR for none) → `P)artial or A)all`. The
  P/A question must be answered: any other key re-prompts, ESC cancels.
  File names: bare name → lowercase `.csd` appended; a typed `.csd`
  tail of any case is kept as entered, never doubled (`cs_savename`,
  shared by save/load/merge/password-probe).

## Things deliberately assumed (flag, don't silently change)

- Form mode (`;F F`) marks an empty cell CE_ALLOC (counted in `entriesUsed`, type line "(empty, but allocated) (form mode set)") and the flag **survives** commits / `;D E` / copy / save/load; only `cs_clear_all` resets it. The `;A` course reports `Form Control Mode` per stop, `Auto complete` at the end, `Auto aborted` on ESC; a bare RETURN at a stop keeps the value and jumps on. `;` inside the course is the command lead-in (opens `cs_command_menu`, never enters the cell; pending text survives). During an entry the type line shows the live interpretation plus the form suffix, and `current:` tracks the stop cell.
- Trigonometry inputs/outputs are **RADIANS** (user decision 2026-11,
  replacing the earlier flagged degree assumption — the trig set is not
  documented in either manual, and the user's precision suites assume
  radians).  `DEG`/`DEGREES` and `RAD`/`RADIANS` convert.  The inverse
  trig family publishes at the 38th digit (round_at 38; bias ~0.03 ulp,
  boundary values still snap); sqrt's Newton uses a RELATIVE convergence
  test (|Δy| < |y|·1e-40 — the old absolute 1e-40 exited after one step
  for tiny arguments).
- The `?`-calculator shows CS_MAX_P (24) decimals with no zero-stripping
  (manual 4.1 pinned 12 — user-extended 2026-11, see the standing policy).
- Precision range is `0 - 24` (user-extended from Appendix C's `0 - 12`,
  flagged 2026-11) even though one chapter says "0 to 10 places".

## Source map

| file | responsibility |
|---|---|
| `src/num.c/h` | BCD engine: Csn type, add/sub/mul/div/scale, parse/canon/fixed/sci formats, pow, trig (radians; inverse trig at 38 digits), ln/exp/log/sqrt, round_at |
| `src/parser.c` | expression grammar: numbers, refs/ranges, `SUM/CNT/AVG/MAX/MIN`, math+linear functions, conditionals, `%` postfix, `**`, `\` comments |
| `src/cells.c` | cell store, coords, ref scanners, `cs_fmt_num`, REGR state |
| `src/eval.c` | `cs_recalc_*` (manual-only), commit path incl. `?` calculator, comments |
| `src/screen.c` | layout, menu/status rows, `place_cursor`, prompts/messages |
| `src/command.c` | `;`-commands,	goto, format (0..12), delete dependency check |
| `src/fileio.c` | CSD4 save/load/merge (origin-anchored), password dialogs |
| `src/print.c` | reports to .TXT (typed name, or `.TXT` auto-extension), PRINTER = `CALCSTAR.PRN` spool, `>name` = `.DTA` records, display; page breaks |
| `src/help.c` | help pages |
| `src/main.c` | key loop (arrows, `@` refs, errpend), CLI arg load, `--test` |
