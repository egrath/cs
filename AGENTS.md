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
  pass changes no stored value, the 10000-pass limit (CS_MAX_PASSES,
  raised from 1000 2026-11-13 for long state machines) or
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
  running to the 10000 limit) and counts a changed string result
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
    constructs a Range (a normalized RECTANGLE - user decision
    2026-11: A1>E5 addresses the whole block; REGR keeps its own 1-D
    requirement with Error 2) decided BEFORE any dereference;
    numbers/bools compare.  `< <= = <>` compare values
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
  - SET's change notification is HONEST: the old cell state is
    snapshotted and `cs_mark_sheet_change()` fires only when the
    stored state really differs (an idempotent SET no longer keeps an
    iterated recalculation alive — an always-true SET branch settles
    under `;R A` instead of running to the 10000-pass limit).
    `cs_recalc_last_passes` records the last driver run's pass count
    (diagnostics/tests).
  - **COLIDX(x)** (user-ext 2026-11; briefly named COLNUM before the
    user-preferred rename): the 1-based column index.  The
    argument parses SCOPED: a 1-2-letter word cleanly ended (not
    followed by '(') is a column designator (`COLIDX(AB)`), else a
    general expression — a quoted designator (`COLIDX("AB")`) or a
    Reference (`COLIDX(REF(2,3))`, `COLIDX(A1)` — the row ignored).
- **The FRACTIONAL factorial** (user-ext 2026-11-13): FACT(x) for
  non-integer x is the Gamma function (FACT(x) = Γ(x+1)) via the
  Lanczos approximation (g = 7, 9 terms; the coefficients as
  high-precision decimal strings) in gamma_lanczos() — the BCD
  engine's pow/exp/sqrt/π do the math; the result carries ~15 correct
  digits (the coefficients' precision — the display's 14 are safe).
  csn_fact routes: exact non-negative integers keep the falling-
  product path AND its cap (FACT(25) -> ?n?); x < 0 is domain;
  non-integer x > 0 -> gamma; a huge argument overflows naturally
  (Error 0).  The factorial ENTRY (2.2!) works through the same
  route.  NCR/NPR stay integer-only.
- **INVFACT(y)** (user-ext 2026-11-13): the inverse factorial.  Exact
  integers walk the factorial sequence (INVFACT(120) = 5; y = 1 -> 1);
  everything else solves ln Γ(x+1) = ln y with NEWTON in LOG SPACE
  (lngamma_lanczos — the Lanczos form without the exp, so the full
  exponent range works) and the ANALYTIC digamma derivative
  (csn_digamma: shift the argument to >= 20 with psi(x+1) = psi(x) +
  1/x, then the asymptotic series to the z^-10 term — error ~5e-18).
  The iteration runs until the delta stops shrinking (the BCD noise
  floor) or 100 passes.  Domain: y <= 0 and y below the Gamma minimum
  (0.8856031944108887 — hardcoded) -> Error 0; for the two-sided
  interval just above the minimum the ASCENDING branch is returned.
  The argument comes through parse_numarg (Numbers and coerced
  References).  THE LNGAMMA BUG CLASS: an uninitialized `half` (the
  exponent zz+0.5 multiplier!) and an uninitialized lns2pi — the code
  gave ln(A) only; also the exponent multiplier must be (zz+0.5), NOT
  half alone.  The digamma was correct from the start (the "x10" was
  a canon misread: +1.7061E+1 = 1.7061).
    colidx_word() validates 1..127 ("DX" -> Error 1; a designator with
    digits/empty -> Error 1), wrong types -> 6, no arg -> 3.  NOTE: a
    column-header display showing the index ("A (1)") was BRIEFLY
    implemented and then UNDONE at the user's request ("more faithful
    to the user interface") — cs_col_hdr is gone, cs1.txt's Col> line
    is the ORIGINAL capture again.
  - **Dynamic ranges in the command dialogs**: the static coordinate
    parse is tried first; on failure the dialogs evaluate the text as
    an expression via the new `cs_eval_range()` (a Reference or Range
    value) — `REF(1,C2)>REF(1,C3)` works in `;D R/C/F` From-To, the
    copy corners, Goto and the print/load corners (helper
    `cs_coord_text()`; failure keeps the dialog's own message).
    Caveat: the dialog expressions run the full evaluator, so a
    conditional with side effects typed into a prompt executes.
  - Every operand/argument dereference happens at the point of use
    (lazy) — never eagerly after parse — so references survive to the
    operators.  Error mapping: type mismatch 6, diagonal range 2,
    REF out-of-limits/bad coordinate 1, fractional REF coordinate 0,
    unknown function 4, malformed call 3/7.
  - WORKED EXAMPLES (all pinned in --test, section "documented
    examples"): `VAL(A1)`, `VAL(REF(2,5))`, `VAL(A1)+VAL(B1)`,
    `SQRT(VAL(A1))`, `VAL(REF(1+1,2+3))`; `REF(1,1)`, `REF(2,5)`,
    `REF(1+1,2+3)`, `REF(A1,5)`, `REF(VAL(B1)+1,VAL(B2))`;
    `A1>A5`, `REF(1,1)>REF(1,5)`, `A1>REF(1,5)`, `REF(1,1)>A5`,
    `A5>A1` (normalizes), `SUM(A1>B5)` -> the 5x2 rectangle sum;
    `SUM(A1>A5)`, `SUM(REF(1,1)>REF(1,10))`, `AVG(REF(2,1)>REF(2,10))`,
    `CNT(A1>REF(1,10))`, `MIN(REF(1,1)>A10)`, `MAX(A1>REF(1,10))`,
    `SUM(A1>A5,B1>B3)` (multi-area), `REGR(REF(1,1)>REF(1,5),B1)`;
    `VAL(A1)>VAL(A5)` vs `A1>A5` (range); the user's
    `(1):SET(REF(C1,C2),VAL(B7));0:1` (writes B7's value into the cell
    addressed by C1/C2's values); the countdown
    `(VAL(A1)>0):SET(A1,VAL(A1)-1);RECALC();0:"finished"`;
    `1:SET(REF(2,1),VAL(A1)*2);RECALC(REF(2,1));0:"ok"`;
    `SET(A1,B2)` copies B2's number or text via coercion;
    `SET(A1,"\rTotal")`; dialog `;D R` `REF(1,C2)>REF(1,C3)`,
    `Goto REF(2,4)`.
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
- **cs_msg/cs_msg_plain are BOUNDED** (crash fix 2026-11-13): they
  used unbounded vsprintf into lastmsg[80] with the [75] truncation
  AFTER the write — a sticky-error line carrying a long formula
  (>~45 chars of formula text) overflowed the buffer and macOS
  FORTIFY aborted (EXC_BREAKPOINT, silent app death on paste of a
  long conditional).  Now vsnprintf(lastmsg, sizeof(lastmsg)) with
  lastmsg[256]; the display truncation stays at 75.
- **The factorial entry "5!"** (user-ext 2026-11-13): an entry ending
  with '!' whose PREFIX PARSES (decision (b): a syntax check, not an
  evaluation — `A5!` over an empty A5 shows ?n? until filled; the
  manual's '!...!' value holder is untouched) becomes a FACTORIAL
  cell: Cell.factf (persisted in the prec byte's bit 7), content
  verbatim, the window shows the VALUE (not "In!" — cs_is_inputf is
  flag-free and factf bypasses it), FACT(prefix value) via
  cs_eval_fact_source, and ;R RECOMPUTES via factf_recompute in
  cs_recalc_entry (NOT skipped like the value holder).  The dry parse
  (P.dry + cs_parse_only) detects side-effect statements without
  running them — the P.dry field must be initialized on every stack P
  (a garbage dry silently skipped all SETs once!).  Non-expression
  prefixes (STOP!, 5!!) fall through: STOP! stays text, 5!! hits the
  old numeric-error path (iserr).
- **INTG/FRAC/SGN/NCR/NPR** (user-ext 2026-11-13): INTG = trunc toward
  zero (csn_trunc with 0); FRAC = x - INTG(x), SIGN-PRESERVING
  (FRAC(-3.5) = -0.5 — user decision); SGN = a SIGN alias (SGN(0) = 0
  — the user's "=1" was a typo, kept 0); NCR/NPR = COMBIN/PERMUT
  aliases (the n >= 25 caps give ?n?).  All thin BCD wrappers in the
  parser's function dispatch.
- **The help screens (`;H`) are the ORIGINAL CalcStar text** (the user
  supplied the two pages verbatim, 2026-11-13; our extensions are NOT
  in the in-app help — they live in this file and README.md).  The
  pasted tab indentation is kept (curses expands it).  Flow: page 1 ->
  RETURN -> page 2 -> RETURN -> the main screen — the 'H' command
  case REPAINTS the main screen after cs_help_cmd returns (the original:
  "Pressing return on the second page switches back to the applications
  main screen"); without that repaint page 2 lingers until the next key.
- **Sorting: `;#`** (user-ext 2026-11-13): reorder the rows of a
  rectangular range by a key column (asc/desc).  Prompts "Range? "
  (cs_coord_text: static or REF pairs), "Key (Column)? " (a bare
  designator via cs_eval_keycol, or an expression yielding a Number
  index or Reference), "Order (A/D)? " (invalid keys re-prompt, ESC
  cancels everywhere).  Rectangle-row semantics (each row carries its
  cells within the range's columns); numbers before text on mixed
  keys, empty keys last, stable.  Formula references into the sorted
  block are REMAPPED text-level (cs_sort_refs: perm[row - r1] with
  ABSOLUTE destination rows; frozen '!' refs and comments/strings
  untouched); the cached values refresh on ;R.  NOT advertised in the
  command panel or the help (both stay byte-identical to the 1982
  interface); documented here and in README.  Implemented in
  cs_sort_range (exported for --test) + sort_cmd.
- **E()** (user-ext 2026-11-13): the Euler number as a constant
  function, matching the PI() pattern — csn_e() parses the 56-digit
  string "2.71828182845904523536028747135266249775724709369995957496696763"
  (the BCD mantissa keeps the leading 40 digits).  LN(E()) = 1; the
  engine's EXP(1) agrees with the constant to ~28 decimals (the
  constant is MORE precise than the exp series' rounding — pinned as
  a tolerance, not an equality).  The name "E" is a 1-letter function
  name - no conflict with the COLIDX designator parse (that is scoped
  to COLIDX's argument) or with scientific notation (parse_num's
  E-scan only follows digits).
- **MED(range)** (user-ext 2026-11-13): the median, in the SUM/AVG
  family's dispatch (parse_list args: ranges incl. rectangles,
  multi-area lists, direct numbers).  Text/empty entries skipped, an
  error cell in a range -> Error 0 (the family's behavior — user
  decision).  BCD insertion sort ascending; odd count -> the middle
  element, even count -> the average of the two middle elements
  (Excel-style — user decision); no numbers -> 0 (AVG's behavior —
  user decision).
- **The TYPED string model** (user-ext 2026-11-13): VAL is the TYPED
  accessor (numeric -> Number, TEXT -> String, empty/error -> Error 1;
  a bare reference behaves the same under option 3); string
  comparisons through all six operators via strcmp (byte order,
  case-sensitive); `&` concatenation as its own grammar level between
  parse_add and parse_cmp (String & String only); CHR(n) = one char,
  integer 32..127 only (everything else Error 0); ORD(s) = the code of
  a length-1 printable string (strict: wrong length, non-printable
  byte, Number argument -> errors, Number arg -> Error 6).  THE
  STRICT MIXING RULE: String+Number in one operation -> Error 6
  through every path (literals, refs, VAL, direct function args);
  the one exception stays: text INSIDE a referenced range is ignored
  by the SUM/MED family.  GRAMMAR TRAP: `!` after a value is the
  factorial POSTFIX — `X!=0` parses as `(X!)=0`; inequality must be
  spelled `<>` (three bf-machinery formulas bit this).  A leading
  unary `+` AFFIRMS a string (+CHR(65) is "A" — the standard entry
  form; parse_unary first gave the mixing error 6, bugfixed
  2026-11-13); a unary `-` on a string stays Error 6.  The ref
  shifter's delimiter lists gained `&` (4 sites in cells.c).
- **The DERIVED cell type** (user bug report 2026-11-13): a formula
  whose current result is a string IS a text value everywhere — the
  window shows it LEFT-justified, the type line reads "text: left
  justified", VAL and the bare-reference coercion yield the String,
  range functions (SUM/CNT/AVG/MAX/MIN/MED) skip it like text, `;W`
  headings show the string, print reports print it left-justified,
  and the sort treats it as a TEXT key.  The store does NOT flip
  CE_NUM/CE_TEXT: cont keeps the formula (re-evaluable), strres
  carries the string result and is set/cleared by every commit/;R
  evaluation — a numeric result clears strres and the cell is
  numeric again (pinned flip-back).  strres IS persisted: the CSD
  per-cell record carries "unsigned short strreslen + bytes" (written
  only for type==3 cells; the fileio header documents it) — a loaded
  sheet shows the string immediately, NO ;R needed (pinned round
  trips incl. a 200-char string; the earlier "not persisted" note
  was wrong).  The CSD magic stays CSD4: no format change was
  needed (user request 2026-11-13 — backwards compatibility
  waived, but the existing layout already carried the string).
- **SET preserves formulas** (user-ext 2026-11-13, flagged semantic
  refinement): a numeric SET into a cell whose entry is a FORMULA
  (an expression, not a pure number literal — num_literal_p scans
  sign/digits/./exponent) updates the VALUE and keeps the entry
  text, so a state machine whose counter targets its own cell keeps
  its program.  A pure-number-literal target (or empty/text) still
  becomes the plain value (the literal clobber is LOAD-BEARING: the
  state cells' cont must track their value or the next pass
  re-evaluates the seed and oscillates forever).  Without this, the
  first self-targeting SET destroyed the formula text and the machine
  "forgot its program" after one pass.
- **CS_MAX_PASSES 1000 -> 10000** (user decision, flagged): for long
  state machines; ESC still interrupts; cs_recalc_last_passes is
  normalized to the EXECUTED pass count (at the cap exit the for-loop
  has already incremented past the limit: `(pass > CS_MAX_PASSES) ?
  pass - 1 : pass`).
- **The brainfuck interpreter, built from documented formulas**
  (user-ext 2026-11-13, proof of concept for the string batch; NOT a
  command): 17 machinery formulas in row 5 columns B..R (evaluated
  left to right once per pass — row-major order IS the pass
  ordering), state cells B1 pc / D1 ptr / F1 mode (0 run 1 skipfwd 2
  skipback 3 halt) / H1 depth / J1 out / K1 in / L1 program length,
  program row 3 one char per cell (TEXT entries), tape column A
  (seed 0), output column S one char per row, input column T from
  row 20.  One instruction per `;R` pass; the halt detector R5 sets
  mode 3 and the sheet settles by itself.  The full formula list
  lives in README.md ("A brainfuck interpreter built from formulas");
  --test builds it programmatically (bf_build) and runs `+++[>+++<-]`
  (tape 9), `H`, `Hi` and a `,`-echo.  DEBUGGED LESSONS: (a) a
  two-branch conditional's FALSE branch executes whenever the
  condition is false — the Q5 "land on the matching bracket" case
  must guard the else to skip-back only (a nested conditional as the
  value piece, fully parenthesized so run_branch's `;` split sees it
  as ONE piece); (b) `!=` = factorial postfix (see above); (c) the
  `.` executor needs the tape byte printable or CHR errors and the
  machine stalls (graceful: error cells stop changing after ~2
  passes).
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
