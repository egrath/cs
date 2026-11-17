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
  come out clean and long chains keep ~24+ digits. The on-disk format
  (CSD5, 2026-11-17) stores the FULL mantissa: per value the exact ndig
  digit bytes, not the original's pinned 22.
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
- Save format is `CSD5` (2026-11-17: **the format VERSION rides in the
  magic** — CSD1..CSD4 are older layouts, refused with `READ ERROR`;
  a future layout change bumps the digit so a stale file refuses
  cleanly instead of desyncing). Layout: magic "CSD5", the saved
  range's origin col byte + origin row LE u16, per column width+prec,
  the password, the cell count, then per cell col/row/type/just/
  prec|notation|factf/formf/volatf/cont and per value: sign byte,
  exp10 LE i16, ndig byte, EXACTLY ndig digit bytes (the full-precision
  change, 5cfedcc, dropped the old CSD4 fixed-22 padding — pre-CSD5
  files with short values desync and refuse; re-save to convert —
  which is what broke examples/brainf.csd for a day and forced the
  regeneration + the version byte). The origin anchors the LOAD
  position (manual 14-9: the file's upper-left corner goes at the
  requested corner; All-mode origin = 0,0, so an All+load-at-A1 round
  trip is an identity).
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
- **KEY-STREAM INTEGRITY (user-ext 2026-11-13, two fixes)**: (a) a
  one-CHARACTER entry that is not a digit is TEXT (cs_commit_entry,
  before the numeric branch) — the bf program cells (+ - . , [ ] < >)
  can be typed with no toggle; a lone digit stays numeric and real
  formulas still error into ?n?; (b) cs_poll_escape and the driver's
  per-pass read PUSH BACK non-ESC keys (cs_getch_nodelay + ungetch) —
  the old read-and-discard ate queued keystrokes during evaluations:
  during a fast paste the machinery commits' statement polls consumed
  the user's cursor moves (the commits then stacked in one cell and
  fragmented).  (c) REPAINT BATCHING: redraw() defers the full-screen
  paint while keys are queued (cs_input_pending / cs_getch_nodelay in
  screen.c, paint_pending in main.c's loop; the loop paints as soon as
  the queue drains) — the per-key full redraw could not keep up with a
  paste and the tty buffer overflowed and DROPPED bytes; 200 pasted
  chars now cost 4 repaints instead of 213.
- **THE STATUS-BOX FILE NAME (user bug report 2026-11-14)**: the
  filename box painted `[%-10s]` — a MINIMUM width — so a long stored
  name ("examples/brainf.csd") overran the box and the cputn clip
  removed the closing bracket (the UI looked cut).  Fix: cs_fname_disp
  (cells.c, exported, pinned) = the basename after the last '/', the
  LAST extension stripped, truncated to 10; the paint uses
  `[%-10.10s]` so the `]` always sits at its column.  fname KEEPS the
  full name internally: ;S prefills from it and cs_savename re-saves
  into the same place.  The empty-name idle case is byte-exact
  (verified against cs1.txt).
- **THE OUTPUT ACCUMULATES IN ONE CELL (user request 2026-11-14)**:
  the `.` executor (mach G5) now APPENDS to the fixed output cell
  S7: `SET(REF(19,7),VAL(REF(19,7))&CHR(...))` — the user watches
  "H", "He", "Hel", ... build up in ONE cell.  The seed: S7 = the
  EMPTY-STRING FORMULA `+""` (cs_eval_formula returns the empty
  strres; VAL yields the empty String so the first `&` works without
  a special case — the empty-string-seed is a documented pattern).
  The written cell becomes a real CE_TEXT (SET's text path), the type
  line shows text, the display left-justifies.  The H/Hi/hello/, pins
  check S7's cont (the hello pin also pins CE_TEXT); the example file
  was regenerated (machinery + the S7 seed) and re-verified (load +
  ;R A -> 1769 passes, "Hello World!" in S7; pty: the cell visible
  with the contents line showing the full string).
- **The shipped example `examples/brainf.csd` (user request
  2026-11-14)**: the complete Hello World sheet — pristine state
  (B1=2, D1=1, F1=0, H1=0, J1=0, K1=0, L1=105), the seeded tape, the
  105 program chars and the 17 machinery formulas — saved All-mode
  through the interactive commit path.  Load + `;R A` runs it; the
  pty verified the load, the 1768 live passes and the output.  THE
  "no sample files (by design)" POLICY IS WAIVED for this one file by
  user request (flagged).  The file is generated by the RE-RUNNABLE
  `--genbf` flag (main.c: bf_build + cs_save All-mode) — re-run it
  whenever the machinery or the STORAGE LAYOUT changes (it regenerated
  the file as CSD5 on 2026-11-17 after the full-precision change
  broke the old CSD4 file).
- **The Hello World run (user request 2026-11-13)**: the classic
  program minus its final `.` (byte 10 is outside CHR's 32..127) —
  105 chars, ~1768-1770 passes, tape cells 1..7, output "Hello World!"
  at S7..S18.  The output-row offset in G5 moved +1 -> +7 (below the
  machinery) so the output never collides with the program row for
  any program length (the third output used to write into the program
  cell at column 19 — harmless-by-luck before).  The H/Hi/, pins moved
  to (18,6)/(18,7); the full run is pinned in --test (bf_build +
  "Hello World!" assertion).  The interactive recipe is documented in
  README ("Running Hello, World!") and verified END TO END in a pty
  (build keystroke by keystroke, ;R A, the output visible at S7).
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

- **THE STARTUP BANNER (user request 2026-11-14)**: at startup the
  status row paints inverse `CalcStar <CS_VERSION>, release under the
  terms of the MPL-2.0 license (build <CS_BUILDNO>)`; the FIRST
  keystroke clears it and acts normally (after the resize check, so a
  resize keeps the banner).  Mechanism: cs_start_msg/_active/_clear in
  screen.c; the banner survives repaints via a frame hook in cs_draw
  (unlike ordinary transient messages).  The banner paints BEFORE the
  CLI load: a successful load keeps it via the frame hook; a load
  ERROR clears the flag so the `write error ... not found` text wins
  (verified in a pty both ways).  CS_BUILDNO comes from the GENERATED
  build/buildno.h (-Ibuild; tools/buildno.sh bumps build.count) — the
  rule depends on the SOURCES, so the counter increments on every
  SOURCE-CHANGING build and a no-op make does NOT (verified: 1 ->
  no-op stays -> source change -> 2; make clean keeps build.count).
  Makefile gotcha: the buildno.h rule must stay AFTER the
  build/calcstar rule or it becomes the default goal (bit once).
  The idle-screen gate is unaffected (the capture recipes' keys clear
  the banner before the capture).  LICENSE (MPL-2.0) ships verbatim
  from mozilla.org; CS_VERSION "1.5" in cs.h.

- **THE SPARSE CELL STORE (branch "DOS", 2026-11-14, the biggest
  architectural change)**: the dense `Cell cells[255][127]` matrix
  (~2.2 MiB on a 16-bit compiler, 104-112 bytes per Cell) is replaced
  by a chunked pool + a per-row sorted index in cells.c.  THE TARGET:
  a 386 in protected mode under a DOS4GW-style extender (32-bit), but
  the design KEEPS the 16-bit discipline: no single allocation exceeds
  64 KiB (--test pins the chunk math).  LAYOUT: `CS_CHUNK 250` slots
  per chunk (`malloc(sizeof(Cell) * 250)` ≈ 25 KiB), up to
  `CS_NCHUNK = CS_BUDGET/250` = 16 chunks (CS_BUDGET raised 2000 ->
  4000, a FLAGGED deviation from the emulated pool, user decision);
  free slots thread through their `cont` field (cs_freelist); refs are
  1-based `1 + chunk*250 + slot` (0 = none), resolved through an
  8..16-entry chunk table.  The per-row index: `CsRow {u8 n, u8 cap;
  CsIdxEnt *e}` with `{u8 col; u16 ref}` entries SORTED by column —
  total index memory ≤ budget × 4 B, NOT proportional to the 32,385
  coordinate space.  Cell GAINED `unsigned char col, row` (its own
  coordinate: frees, relinks, the sort).  API (explicit lookup vs
  allocation): cs_getcell (NULL = absent/invalid; absent ≡ empty for
  readers), cs_alloc_cell (allocates when absent, REUSES an existing
  slot otherwise — including indexed-CE_EMPTY slots; NULL = budget or
  pool exhausted), cs_free_cell_at, cs_relink_cell (moves a slot
  between coordinates WITHOUT touching the pool — insert/delete
  shifts; an occupied target is released first, fixing the dense
  struct-assignment leak), cs_next_cell (the ordered row-major scan:
  callers advance with col = cl->col + 1), cs_clear_all (frees every
  row array and chunk).  cs_cell IS RETIRED (no shim): every call site
  became get/alloc explicitly (~250 sites; the --test reads use the
  tread helper: absent reads as an empty static cell; the test seeds
  use tcell = alloc-or-die).  INVARIANTS: (1) a Cell* is stable until
  THAT cell is freed (the pool never moves slots); (2) the evaluator
  only allocates, so cs_recalc_entry_p holds its cell across the eval;
  (3) recalc_pass PRECOLLECTS the populated coordinates once per pass
  into cs_passlist (row-major L-R; qsorted column-major for T-B) —
  the machinery's wiring order is preserved and the 32,385-coordinate
  walk became a ~populated-count walk (the pass cap runs in ~1 s
  instead of hanging); cells created mid-pass (SET writes) are inert
  that pass and join the next, matching the dense visits; (4) the
  save walks the ordered scan (coordinate order preserved); the load
  allocs and reports MEMORY IS TOO LOW cleanly when the pool/budget
  is exhausted mid-file (flagged: a >4000-entry file truncates);
  (5) cs_recount_entries walks the index.  ALLOCATION SLOTS: pool
  take/return is cs_pool_take/cs_free_cell_at ONLY — cs_free_cell
  clears content but keeps the slot live (cs_set_num/cs_set_text rely
  on it).  VERIFIED: --test 401 ok incl. the portability guards; the
  idle gate EXACT; the brainf example loads and runs (1768 passes).

- **THE MEMORY AUDIT FOLLOW-UPS (branch "DOS", 2026-11-14)**: (1) the
  leftover dense `Cell cells[][]` DEFINITION was deleted from cells.c
  (unreferenced since the conversion but still emitting ~3 MB of
  BSS!).  (2) clone_cells snapshots ONE CHUNK PER ROW now
  (`Cell **rows`, each ≤ 127 cells ≈ 14 KB) instead of the single
  ~400 KB block — the sort's snapshot reads `snap[row][col]` and
  free_clones(snap, n, sh) frees row by row; the biggest single
  allocation in the app is now a pool chunk (27 KB).  (3) parse_list's
  `Csn list[LISTMAX]` STACK FRAME (2048 × 52 = ~104 KB — fatal under
  DOS4GW's small stack) is a lazily-GROWING HEAP buffer (start 64
  entries, doubling to CS_BUDGET; a full-budget MED transiently holds
  ~208 KB — the one oversized transient, fine on the 32-bit extender);
  LISTMAX retired; every error path frees.  (4) the ;A course's
  `int seq[32385]` stack frame (~65 KB) is `static int seq[CS_BUDGET]`
  (the course is not re-entrant).  (5) main.c:638's C99
  mixing-declarations warning fixed (the guard block moved to
  port_guards()).  VERIFIED: --test 403 ok (incl. the guards), the
  idle gate EXACT, the brainf example runs.

- **THE ROW/COLUMN DELETE HANG (user bug report 2026-11-14, branch
  "DOS")**: deleting a row/column FROZE the app whenever a formula
  referenced the deleted line.  Root cause: dep_check's ordered scan
  advanced `c` in only two of its three paths — the "hit" path (a
  formula referencing the deleted line) fell back into cs_next_cell
  with the UNCHANGED coordinate, revisiting the same cell forever.
  Fix: the scan advance `c = cl->col + 1` after the hit block.
  dep_check_range is exported (declared in cs.h) for the regression
  pins: a formula referencing the deleted row -> the scan reports
  "A4" and TERMINATES; the same for a column ("C1").  The interactive
  ;D R flow pty-verified: the inverse "ERROR: would delete ref(s) at
  B1" message instead of the freeze.

- **THE RELINK ABSENT-SOURCE BUG (user report 2026-11-14, branch
  "DOS")**: cs_relink_cell RETURNED when the source was absent, but
  the dense semantics it replaced (`cells[t] = cells[s]`) move
  "empty" over occupied targets too — deleting columns C>D kept the
  old D (3.00), deleting rows 4>7 kept c and e (the user's exact
  traces).  Fix: an absent source RELEASES an occupied target
  (a degenerate source==target relink is a no-op).  Pinned with both
  user cases as exact final states (B3..E3=1..4, delete C>D ->
  1.00/4.00/empty; a,b,c,d,e,f,(empty),x, delete rows 4>7 ->
  a/f/empty/x); the interactive column case pty-verified.
- **ROW() AND COL() (user request 2026-11-14)**: the current cell
  cursor's row/column, 1-indexed, evaluated against the cursor AT
  EVALUATION TIME (curRow+1/curCol+1) — a stored formula
  re-evaluates against wherever the cursor sits during ;R (FLAGGED
  semantics, like the manual's cursor concept — documented in
  README).  The zero-arg family's convention: arguments are Error 6
  (PI/E), not the REF family's 3.  Pinned at A1 and C3 plus the
  arg-count errors.

- **THE STREAMING AGGREGATES (branch "DOS", 2026-11-14, user spec)**:
  parse_list and its heap value array are GONE.  SUM/CNT/AVG/MAX/MIN
  stream through CsAgg {sum, ext, cnt, started} (~68 bytes on the
  stack) — order-insensitive, so the semantics are identical to the
  collected list: the strict mixing rule (a direct String -> Error 6),
  an error cell in a range -> Error 0 aborting mid-stream, the
  csn_add overflow -> Error 0, empty -> 0, Bools aggregate as 0/1.
  MED keeps a BOUNDED heap buffer (MED_MAX 512 entries, ~27 KB,
  freed at every exit): a range beyond the bound returns Error 0 —
  never a silently truncated median (user spec); the bound is
  documented in README.  The ;A course's static int seq[CS_BUDGET]
  (16 KB of BSS) is replaced by the LIVE cs_next_form scan (the
  course does not create or delete form flags, so no buffer is
  needed); pty-verified end to end (2 Form Control Mode stops, the
  entries committed, "Auto complete", the form flags on the type
  line).  FOUND BY THE PTY DURING THIS BATCH: the format command
  dereferenced cs_getcell(curCol,curRow) on an ABSENT cell (the
  dense slot was never NULL) — CRASHED the app the moment ;F was
  opened on an empty cell; fixed with NULL-tolerant label reads and
  per-branch lazy allocation ("can't create" on budget failure, the
  manual's wording).  New pins: MED over 513 values -> Error 0 while
  SUM/AVG/MAX/MIN/CNT stream all 513 correctly.  VERIFIED: --test
  419 ok, the idle gate EXACT, the brainf example runs.
- **THE MEMORY SAVED BY STREAMING (the assessment)**: before: the
  aggregation list was a heap buffer growing to budget × 52 B
  (~208 KB transient at the full budget, plus ~7 realloc copies per
  full-budget aggregation) and the ;A course held a permanent 16 KB
  static array.  after: the streaming four use ~68 stack bytes
  (ZERO heap), MED's transient is capped at ~27 KB (-181 KB at the
  extreme), and the 16 KB BSS is gone (-16 KB permanent).  The worst-
  case stack is now the parser recursion (~12 levels × the parse
  chain ≈ 12-16 KB) plus small frames everywhere else (the largest
  local ≈ 500 B in num.c's digit scratch) — the DOS4GW build should
  reserve a ≥ 24 KB stack (a linker-file setting, documented).

- **EMPTY REFERENCED CELLS ARE ZERO (user decision 2026-11-14,
  branch "DOS")**: the ref-val language's option-3 rule (a bare
  reference to an empty cell -> Error 1) is RELAXED for arithmetic:
  a bare reference (and REF()'s deref) to an ABSENT, CE_EMPTY or
  CE_ALLOC cell yields 0 — the manual's rule, restored because the
  user's relative-copy traces broke on it (A2>B2 -> A5>B5 showed
  ?n?; the original yields 1.00/2.00).  THE STRICT CONTRACT STAYS:
  VAL(empty) -> Error 1 (the typed accessor), the factorial entry's
  prefix keeps "?n? until filled" (cs_empty_strict toggled around
  cs_eval_fact_source INCLUDING its post-eval deref — the first
  attempt cleared the flag before the deref and the A9! pin caught
  it), error cells stay Error 1.  Pins updated (the bare-empty cases
  now succeed with 0; the VAL pins unchanged); the user's two copy
  scenarios pinned (the single-cell fill chain 3..10; the range copy
  A2>B2 -> A5>B5 = 1.00/2.00, pty-verified).  The copy core is
  exported as cs_copy_block (copy_cmd's mechanics, also pinned).

- **THE EVALUATION CONTEXT (user bug report 2026-11-14, branch
  "DOS")**: the user's trace showed +ROW() copied down A1>A2>A10
  yielding 1 everywhere — the cursor-at-eval-time semantics made a
  stored +ROW() a GLOBAL read (every copy evaluated with the parked
  cursor; ;R re-evaluated them all against one cursor position).
  FIX: the evaluator has a CONTEXT (cs_eval_ctx_on/col/row in
  parser.c): ROW()/COL() in a STORED formula answer the cell's OWN
  position; context-free evaluations (the ?-calculator, the dialogs,
  direct evals) keep the cursor.  The context is set-and-restored
  around the evaluation at THREE sites: cs_recalc_entry_p (the
  recalculated cell), cs_commit_entry (now a wrapper around
  cs_commit_core — the entry's target cell), and copy_one (the
  DESTINATION — a copied +ROW() evaluates for its new home
  immediately and stays stable across ;R).  factf_recompute inherits
  the commit's context.  PINS: the interactive ckc cases unchanged;
  a stored +ROW() at F7 = 7, +COL() at C5 = 3 (csn_cmp, not the
  nd[0]-as-value trap for 2-digit values!); the user's copy-down
  case (2..10).  pty-verified: the copy down shows 2.00..9.00.

- **THE COPY CONTEXT DELTA BUG (user report 2026-11-14, branch
  "DOS")**: +COL() at B1 copied to C1>F1 yielded 2,3,4,5 (off by one)
  and +ROW() at A2 copied down yielded 2..9 — copy_one set the
  evaluation context from its dc/dr parameters, which are the
  RELATIVE SHIFT DELTAS, not coordinates (my origin-source pin passed
  by luck: at A1 the deltas equal the coordinates).  Fix: the context
  assignment moved OUT of copy_one into cs_copy_block's loop with the
  true destination coordinates; copy_one keeps the shift-delta
  meaning of its parameters.  Pinned with both user cases (the
  exact final states) and the +COL() case pty-verified
  (2.00/3.00/4.00/5.00/6.00 across B1..F1).

- **C89 PRINTF DISCIPLINE (user request 2026-11-14, branch "DOS")**:
  snprintf/vsnprintf are C99 - the DOS compiler must never see them.
  All 7 sites replaced: cs_savename and print_name use explicit
  bounded memcpy compositions (the %.100s bound spelled out);
  cs_start_msg uses strncpy; cs_msg/cs_msg_plain use vsprintf with
  the bound BY ANALYSIS documented at the site (every %s argument in
  their call graphs is a formula/coordinate text <= CS_EDITMAX = 120,
  the fixed text <= 60, so the expansion stays under lastmsg's 256 -
  the paste-crash fix keeps its guarantee); the UNBOUNDED string
  results (the ?-calculator) go through the new cs_msg_str, which
  copies bounded.  THE AUDIT LESSON: grep for C99 functions before
  the DOS build - the macOS headers declare them even under
  -std=c89 -pedantic, so the build never warns.
- **THE EXTERNAL-REVIEW FIXES (branch "loadfix", 2026-11-15, items
  5-7 of an external AI review)**:
  - (5) `cs_ptr_ref` DELETED: the chunk-ownership pointer scan
    (`c >= cs_chunks[k] && c < ...` + pointer subtraction across
    separately allocated chunks) is ISO-C undefined behavior.  Cell
    gained `unsigned short ref` (its own 1-based pool ref, set when a
    fresh chunk's freelist is threaded); cs_pool_take saves the slot's
    own ref before the memset and restores it after (the memset zeroes
    it - the save/restore bracket).  cs_getcell's chunk-table indexing
    (not UB) stays.
  - (6) ALLOCATE-BEFORE-COMMIT everywhere a cell's content is
    replaced: cs_set_text/cs_set_num now return int (all strings
    strdup'd and checked FIRST, the old content freed only after, the
    cell unchanged on failure); the commit path wraps them
    (commit_text/commit_num in eval.c) so a failure reports MEMORY IS
    TOO LOW and refuses the entry.  copy_one's text branch, the
    !...!  value-holder branch (raw strdups removed - it now uses
    commit_num with a zero value), swap_two_cells (the strdups abort
    the swap BEFORE cs_relink_cell), and clone_cells (progress-tracked
    unwind: fail_i/fail_j free exactly the allocated strings, no
    aliasing of originals - the snapshot now NULLs first and strdups
    from the source) all follow the discipline.
  - (7) cs_load HARDENED with a two-pass structure: pass 1 walks the
    ENTIRE file (header, count validated against CS_BUDGET - an
    over-budget file is refused with READ ERROR (flagged wording
    choice) - and every record through rd_check_record: all reads
    non-EOF, all lengths non-negative, type must be 2 or 3 (an
    unknown type would desync the stream - the old loader silently
    ended the load "successfully" on cc/rr out-of-sheet, now they go
    through the drop path in sync)); pass 2 rewinds, clears, loads
    with every malloc CHECKED (MEMORY IS TOO LOW on failure) and every
    fread REQUIRED complete (READ ERROR - the old len=0 downgrade
    silently loaded truncated files as empty cells).  A failed
    validation never touches the sheet.  GOTCHA FOUND BY THE TESTS:
    the clear must stay BEFORE the colw reads in pass 2 (my first
    restructure put it after and cs_clear_all reset the widths to the
    defaults - the "save formula + cols" pin caught it immediately).
  - BONUS (found during the loader work): cs_file_password probed for
    magic "CSD1" while saves write "CSD4" - the save dialog's
    "Verify the password to remove" check NEVER fired.  Fixed; pty
    verified (the prompt appears, wrong pw -> "not ok", ESC still
    cancels).  Also the load dialog no longer overpaints a READ ERROR
    with "write error ... not found" (the generic message paints only
    when the file is genuinely absent).
  - PINS (6): an over-budget count refused, an unknown record type
    refused, a truncated file refused - each with A1/B1/entry count
    verified UNTOUCHED after the refusals.
  - VERIFIED: --test 529 ok; the idle gate EXACT; the pty save/load +
    password flows.

- **THE REFERENCE MANUAL PDF (user request 2026-11-14)**:
  docs/CalcStar_UM_11_8_1982.pdf is COMMITTED (a deliberate repo
  reference; the earlier untrack is reverted by this explicit
  request).

- **THE FILL COPY (user request 2026-11-14, branch "DOS")**: a
  straight range-to-range copy (the same column span or the same row
  span, mirrored directions included) asks "F)ill or D)estination"
  (the last question, after R/N; ESC cancels; anything else
  re-prompts).  D = the current single copy; F = the chained fill
  (cs_fill_block, exported): the source TILED and re-shifted at
  every intermediate distance, evaluated nearest-to-source first
  (the tiling rule: the target takes the source cell at distance
  mod the source's dimension with the shift from that cell — the
  user's 1-row example degenerates to the per-distance shift; a
  2-row source alternates).  The stored formulas are
  self-consistent across ;R.  The fill's region = the span between
  the source and the destination plus the destination block; the
  budget exhaustion mid-fill reports MEMORY IS TOO LOW and stops
  (a partial fill, flagged).  Pinned: the vertical fill (3..10 and
  6..20), the horizontal fill (3..6 and 6..12), the mirrored fill
  up (A2..A8 = 2..8 — the chain builds upward with the empty-as-
  zero rule).  pty-verified: the user's exact vertical grid.
  THE CONVERSION BUG: my first fill indexed the snapshot
  tmp[col][row] AND row-indexed past the 1-row snapshot — the
  crash led to the tiling helper fill_one (also the double-fill
  in the loop-condition probe cleaned).

- **THE MERGE (2026-11-14)**: the "DOS" branch merged to master
  (fast-forward) and deleted; the README gained a COOKBOOK section
  with worked examples for the day's features (the string model,
  ROW()/COL(), the fill copy with the user's grids, empty-as-zero,
  the bounded aggregates, the brainf cross-link).  The docs PDF and
  the manual PDF remain in docs/.

- **THE MEMORY AUDIT FIXES (user report 2026-11-15, branch
  "memfix")**: (1) cs_clear_all LEAKED every live heap string (the
  sparse clear freed the chunk memory directly without walking the
  cells — the missing cs_clear_strings the user remembered): the new
  cs_clear_strings walks the row index and frees cont/rep/strres per
  populated cell before the chunks die.  (2) cs_relink_cell's OOM
  path pushed the slot to the freelist WITH its live strings (the
  next pool_take memsets over the pointers) — the strings are freed
  first now.  (3) the SORT's snapshot is GONE: the keys are read
  from the LIVE cells (nothing changes until the permutation is
  applied), and the permutation is applied IN PLACE by swapping row
  pairs (the swap = strdup-save one side, cs_relink_cell moves the
  other side, the strdup'd content lands in the vacated row; the
  while-swap loop fixes perm entries, so the ORIGINAL permutation is
  copied to perm0 for the cs_sort_refs remap — the swaps mutate perm
  and the first version remapped with the fixed-up identity).  A
  whole-sheet sort no longer transiently copies all 32,385 slots
  (~3.4 MB) — the sort's transient is now just the perm arrays
  (nrows x 4 ints).  (4) the COPY's snapshot exists ONLY for an
  overlapping destination (the overlap test d1<=c2 && d2c>=c1 &&
  e1<=r2 && e2r>=r1); a non-overlapping copy reads the source cells
  LIVE (absent sources read as the empty struct — copy_one's
  CE_ALLOC branch).  The overlapping copy's sequential evaluation is
  the HISTORICAL behavior (the snapshot protects the source TEXTS;
  the formulas evaluate against the mid-copy sheet — pinned:
  A2 = 1, A3 = 2 with the adjusted texts).  (5) the sort's NULL key
  cells guarded (an absent key reads as empty -> the empties
  partition).  VERIFIED: --test 432 ok (the new overlap pin, all the
  sort/fill/copy pins), the idle gate EXACT.  ASAN's leak detection
  is unavailable on this macOS platform ("detect_leaks is not
  supported") — the leak fixes are by code inspection.

- **THE SORT RANGE RULES (user request 2026-11-15, branch
  "memfix")**: (1) a ONE-COLUMN range (A1>A5) implies the key — the
  Key (Column)? prompt is skipped (the key is that column);
  (2) a ONE-ROW range (horizontal, or the degenerate 1x1) is
  rejected with "bad sort range" (the terse-lowercase family voice;
  the user's wording) — before the key prompt, so the horizontal
  case errors immediately; (3) cs_sort_range gained a defensive
  early return for nrows <= 1 (nothing to permute; the core stays
  safe for any caller).  Pinned: a one-row cs_sort_range call is a
  no-op; the pty verified both flows (the single-column sort runs
  straight to Order (A/D)?; the horizontal attempt paints the
  inverse "bad sort range").

- **THE /r /l /c ALIGNMENT ON FORMULA STRING RESULTS (user request
  2026-11-15)**: the re-justify path (/r /l /c alone in the edit
  field) now also accepts a CE_NUM cell with a strres (a formula
  whose current result is a string) — the formula stays, only
  c->just changes.  The strres DISPLAY honors just (cell_render:
  L/R/C; print_render: L/R/C; the type line: justname(c->just)
  instead of the hardcoded left).  cs_set_num defaults the just to
  strres ? 'L' : 'R' (the string default = today's left; numeric
  always right); the recalc path never touches just (the alignment
  survives ;R); recommitting resets it (the edit-resets-formatting
  rule); copy_one carries src->just for CE_NUM (a copied aligned
  formula keeps it — flagged improvement).  The CSD just byte
  already round-trips (no format change).  The narrow rule: /r on a
  formula whose CURRENT result is numeric is accepted but invisible
  (numbers are always right); /r on a plain numeric cell changes
  nothing.  Pinned: the default L, /r /c /l, ;R preservation, the
  numeric flip, the copied alignment, /r-on-numeric no-change.
  pty: the user's +(...):"even":"odd" right-justified after /r with
  the type line "text: right justified".  NOTE (user 2026-11-15):
  the CSD format may change freely without asking until release.

- **LG() = LOG() (user request 2026-11-15)**: the base-10 alias in
  the name-lookup table (no new enum entry) — the SGN/NCR/NPR alias
  family pattern.  Pinned: LG(100) = 2, LG(10) = 1, LG(1) = 0
  (ckc's "0" = the exact zero), LG(0)/LG(-5) matching LOG's error
  class, and the manual's worked example 56/(12*10**-3) = 36.69
  through LG.  The canon gotcha bit again: LOG(100) is +2.0E+1
  (digits 10, exponent 1), NOT 2.0E+2.

- **THE FINAL BATCH (the user's report, 2026-11-16, branch
  "finalfix")**:
  - THE LOAD-AT-ORIGIN REFERENCE ADJUST: a formula loaded into a
    different location behaves like a copy there - pass 2 adjusts
    each formula's content with cs_adjust_refs by the file's offset
    (c0-bascc, r0-basrr) once per cell; the frozen '!' refs stay,
    the value holders are skipped.  PIN: A1=7 + B2=+A1*2 saved,
    loaded at the origin B2 -> the formula at C3 = +B2*2 (A1 lands
    at B2) and the recalc gives 14.  THE PIN TRAPS: the self-
    referencing +B2*2 at B2 DOUBLES each pass (1000 = the canon of
    100 bit me again); the load CLEARS the sheet so the target data
    must be part of the file.
  - THE FULL-PRECISION FORMAT: the CSD record writes exactly ndig
    digit bytes (up to CSN_DIGS = 40) instead of the fixed 22 - the
    loader reads the VARIABLE count.  THE "OLD 22-DIGIT FILES LOAD
    UNCHANGED" CLAIM MADE HERE WAS WRONG: the old CSD4 writer PADDED
    every value to 22 digit bytes, so any record with ndig < 22 left
    bytes the new loader read as the next record's header - the
    example proved it (READ ERROR on examples/brainf.csd the next
    day).  The magic is now CSD5 (the version-in-the-magic rule) and
    CSD4 files refuse with READ ERROR; re-save to convert.  The
    40-digit engine keeps its
    mantissa across save/load (the engine's own parse of a 41-digit
    literal keeps 40, trailing zeros stripped by finish - the pin
    compares against the PARSED value, not the literal).
  - THE DYNAMIC ERROR QUEUE: cs_errlist reallocs (64 doubling) -
    the fixed 256-entry queue silently dropped further errors; the
    queue lives for one driver run, errq_reset exported for the ;R E
    path.
  - -lm REMOVED from LDLIBS (no math.h anywhere by design; the link
    verified).
  - THE DEL KEY (KEY_DC) = ";D E": delete the current entry (the
    manual's entry delete: no dependency check, no shifts); the
    ASCII DEL (127) stays backspace in the edit fields.  THE PTY
    VERIFIED: the DEL key empties the cell (Room for 4000 entries).
  - THE GCC-14 WARNINGS: cs_coord_str(buf,bufn,col,row) (cells.c,
    exported) composes the coordinate text BOUNDED - all eight
    sprintf("%s%d", cn, row+1) sites replaced (the sticky error, the
    row labels, the cursor line x2, the prompt coord, dep_check's
    buf, the delete defaults x2, the @ refs); the row labels'
    "%3d%c" composed manually.  gcc-14 -Wall -Wextra -pedantic-errors:
    ZERO warnings across the whole tree (the user's compiler gate).
  PINS: +4 (the 40-digit round trip, the origin adjust + recalc, the
  300-error queue).  --test 899 ok; the idle gate EXACT.

- **THE PRINT FIX (the user's report, 2026-11-16, branch
  "printfix")**: with the top-left corner at A1 the print applied the
  FIX-ORDINATES behavior (the old `fix = 1` default; the prompt is
  skipped when A1 is chosen, so the implicit answer never arrived) -
  the first data row duplicated as a "heading" line, every data line
  indented by the ordinate margin, and the fix=0 branch would even
  have printed the column letters.  FIX: fix = 0 by default + the
  explicit noord = (c1 == 0 && r1 == 0) flag: the A1 print emits NO
  ordinates line at all and an EMPTY row-label margin ("the array
  prints as shown").  The fix=1 semantics for the non-A1 corners
  (the headings from the range's first row, the row labels from the
  column to the left) are untouched.  THE PTY PROOF: the user's
  exact repro (A1=aaa B1=bbb A2=ccc B2=ddd, the title TEST) now
  produces exactly TEST / the two plain rows / -** End of report.
  THE PTY DEBUGGING NOTE (two hours): the data-entry commit KEEPS
  the cursor, so the naive aaa\r bbb\r typing REPLACED A1; the
  arrow-key escape sequences leaked into typed entries; and the
  print dialog's title loop breaks on an EMPTY first title (the \r
  before the TEST text landed it in the sheet) - all script bugs,
  the app was right.  THE FLOW VERIFIED: the fix=1 prompt appears
  for the non-A1 corners (B2:C3 smoke-tested).

- **THE MEMORY-SAFETY BATCH (the user's report, 2026-11-16, branch
  "safetyfix")**:
  - print_render: the STRING-RESULT length is bounded to the width -
    a 200-char loader-permitted result overran the ~65-byte render
    buffer (the text branch already clipped; the strres branch's
    negative-pad clamp didn't bound the copy).
  - csn_fmt_sci: SELF-BOUNDS - the frac is reduced to fit bufn (the
    legal width 63 gives frac 57 and a 66-byte total, past the
    caller's 64-byte temp); cs_fmt_num's temp widened to
    CS_MAX_W + 8.  VERIFIED with the poisoned-guard probe: the
    64-byte temp truncates safely at width 63.
  - cs_msg/cs_msg_plain: the BOUNDED FORMATTER (cs_msg_vfmt, C89 -
    no vsnprintf) walking %s/%d with length-checked copies; the
    dependency-list call site (ERROR: would delete ref(s) at ...) can
    reach ~253 chars alone, falsifying the old by-analysis bound.
  - cs_load: the column widths CLAMP to the manual's 3..63 and the
    per-cell prec to CS_MAX_P (a corrupt width 255 fed every render
    buffer).  PIN: the crafted width-255 file loads clamped to 63.
  - cs_parse_coord: the row accumulator cannot wrap - more than 3
    digits is an outright rejection (no row above 255 is legal; a
    13-digit row wrapped into a valid-looking coordinate).  PIN: the
    13-digit row rejected, A255 fine.
  - factf_recompute's buffer: ALREADY FIXED (the factorial batch).
  - THE PRINTER: the width clamps at 240 (the heading-assembly
    line[256]); the output file opens LAST (after every prompt - the
    old "w" open truncated an existing file BEFORE the range/options;
    the pty proof: a cancelled print left the existing file with its
    PRECIOUS content); the CSV export is RFC-4180 (the TEXT values
    quoted, the internal quotes doubled, numerics bare - the user's
    choice; the value remains the rendered width-bounded field,
    flagged).  THE PTY GOTCHA THAT COST AN HOUR: the print dialog
    needs FOUR returns (the file, both corners, the form length) -
    my scripts under-counted and the form editor silently ate the
    y/n keystroke, looking exactly like a hang (the app was fine).
  PINS: +3 (the row wrap, the three-digit rows, the width clamp) +
  the pty (the CSV quoting, the cancel-proof).  --test 895 ok; the
  idle gate EXACT.

- **THE MEMORY BATCH (the user's report, 2026-11-16, branch
  "memfix2")**:
  - (A) THE REGRESSION ARRAYS: already gone (the regression-state
    batch deleted gInd/gDep/REGR_MAX - the reviewer's finding
    predated it).
  - (B) SPLINE allocates by COUNT: a counting pass (the same
    pair-walk) precedes the EXACT allocation - a two-point spline's
    transient dropped from ~47 KB to a few hundred bytes; the 512
    bound stays (the documented shared limit).  The old flat
    SPLINE_MAX arrays cost ~47 KB for ANY spline.
  - (C) THE SNAPSHOT IS SPARSE: clone_snap holds only the POPULATED
    cells with their relative coordinates ({rc, rr, Cell}, the
    strings strdup'd), binary-searched by position - the old dense
    rectangle cost 3.6 MB at 255x127.  Signals: n = -1 (the alloc
    failure) vs n = 0 (a WHOLLY BLANK source - the copy then clears
    the overlapping targets).  THE FILL NEEDS NO SNAPSHOT AT ALL:
    its writes never touch the source block (nearest-to-source
    ordering) - fill_one reads the live sources and clone_cells/
    free_clones are gone.  THE CLONE LEAK: the unwind frees the
    CURRENT entry's already-duplicated strings (the fields are
    NULLed before the assignment, so a non-NULL field is ours) - the
    reviewer's find.
  - (D) EMPTY SLOTS: cs_release_if_empty(col,rw) (cells.c) drops a
    slot that carries NOTHING (not CE_EMPTY content, no
    formf/factf/prec/notation) - the PREFERRED option of the two
    the reviewer offered (no second counter).  Applied at the two
    stranding sites: cs_commit_entry (a ?-calculator entry allocated
    then returned - the pool drained while entriesUsed looked fine,
    the user's repro) and exec_set_stmt's failure returns.  PIN: the
    ?-commit leaves cs_getcell NULL and entriesUsed 0; the failed
    SETs (the arg-count/type errors) likewise.
  - (E) THE RESERVE-FIRST RELINK: cs_relink_cell returns int and
    ensures the destination index's capacity BEFORE any mutation -
    the old order removed the source, released the target, THEN
    grew, so a failed realloc DESTROYED the source mid-shift.  The
    callers guard the -1 (the shifts stop with MEMORY IS TOO LOW -
    a partial shift, flagged; the swap frees its strdup temps and
    aborts; the swap is capacity-neutral so the failure is
    unreachable in practice).  THE COPY BUDGET PRE-CHECK: the net
    growth (the populated sources - the populated destinations) is
    checked UPFRONT - an over-budget copy refuses instead of dying
    halfway (the pinned 3997+2-seed refusal; the loader's
    clear-then-fail was already fixed by the two-pass validation).
  PINS: 6 (the calculator slot, the failed SET slots, the
  overlapping blank copy, the budget refusal, the existing
  spline/fill/shift pins all green).  --test 892 ok.  MY PIN BUGS:
  the 3999-cell seed overflowed the 255-row sheet (16 columns), the
  CS_BUDGET 4000 left no room for the copy's sources (3997+2), and
  a duplicated pin block with a broken string literal.

- **THE NUMERICAL-ENGINE FIXES (the user's reports, 2026-11-16,
  branch "eng-numfix")**:
  - POW: the integer exponent path rewritten as BINARY SQUARING on
    the BCD bits of |b| - the old k > 64 branch REJECTED a negative
    base outright ((-2)**100 errored though 2^100 is representable)
    and the exp/ln fallback lost the exactness.  Now: exact for any
    representable integer power (the pty shows the full 31-digit
    2^100), the sign by the parity of the exponent, a negative
    exponent divides 1, the squarings overflow naturally.  VERIFIED
    against 60-digit decimal arithmetic.
  - LCM/GCD: pure BCD euclid (bcd_gcd) - the old long-math mapped a
    NEGATIVE product to overflow (LCM(-4,6) errored instead of 12)
    and overflowed a 32-bit long on the inputs the 12-digit cap
    admits.  LCM >= 0 by convention (the absolute values), the cap
    applies to the result, GCD(0,x) = |x|.  long_gcd deleted.
  - ROUND/TRUNC: the IDENTITY shortcut - when the value's lowest
    significant digit sits at a place >= |dd| the operation cannot
    change it (ROUND(9.9E+998,1) overflowed the scale-up though the
    answer is the value; any representable value near the ceiling
    has last-place >= exp10-40 >= |dd| since a true fraction at that
    magnitude is unrepresentable).  NOTE: 1E+999 itself is OUT OF
    RANGE (the engine's max < 10^999) - the pin uses 9.9E+998.
  - EVEN/ODD: the parity is the parity of the LAST DIGIT - no native
    conversion, exact for any magnitude.  THE MANTISSA TRAP: the
    stored digits omit trailing zeros (1E+500 is one digit!) - the
    value's true last digit is 0 when exp10 > ndig.  EVEN(1E+500) =
    1 pinned.
  - csn_parse: a MALFORMED exponent (E with no digits: 1E+/1E/1E-)
    is CSN_ER_SYNTAX (the eval codes 8/7/8); the accumulator is
    BOUNDED (latched past CSN_EMAX+4, the digits still consumed) -
    the UBSan signed-overflow at num.c:372 is gone (ZERO runtime
    errors under -fsanitize=undefined) and a huge exponent is
    rejected as CSN_ER_OVERFLOW instead of accepting a wrapped
    exponent.  1E0/1E00/1E-0 stay valid.
  - THE LONG AUDIT: csn_int_val checks the digit count BEFORE the
    conversion (9 digits fit every long; the old 15-digit cap
    overflowed a 32-bit long on the DOS targets); csn_to_long_val
    replaces atol with a checked digit loop (acc > (LONG_MAX-d)/10
    -> saturate; the callers bound their values); long_gcd and
    csn_lcm's long multiplication are GONE (pure BCD).
  - CORR/RSQ/REGR: the TWO-PASS CENTERED form (the means first, then
    the deviations) - the raw n*sxx-sx^2 form cancels catastrophically
    for large offsets with small variations.  REGR: slope = Sxy/Sxx,
    icept = my - slope*mx; CORR: r = Sxy/sqrt(Sxx*Syy).  The offset-
    line pins are EXACT (r = 1, slope = 2 through values near 1e6).
  PINS: 32.  --test 886 ok; the idle gate EXACT.  MY PIN SLIPS: the
  canon exponents again (LCM 12 = E+2, 130 = E+3), 1E+999 is out of
  range (9.9E+998 pinned), the REGR pin expected the slope where the
  function returns meany.

- **THE REGRESSION STATE FIXES (the user's reports, 2026-11-16,
  branch "bugfix-alloc")**: (1) THE STALE SNAPSHOT: cs_reg_reset()
  existed but nothing called it - a valid REGR, then a clear, then
  SLOPE() served the OLD regression (the user's repro); a FAILED
  new REGR left the old state available too.  FIX: cs_reg_reset at
  THREE sites - cs_clear_all (which covers the non-merge load), the
  merge branch of cs_load, and at the TOP of the REGR dispatch
  (every attempt starts fresh: the state rebuilds from the sums on
  success, a failure leaves it EMPTY -> SLOPE() answers Error 9).
  The manual's snapshot semantics otherwise PRESERVED: ordinary
  edits don't invalidate; THE SORT doesn't either (the sums are
  order-independent - pinned).  (2) THE 512-CAP + THE DEAD ARRAYS:
  REGR_MAX/gInd/gDep deleted - the sums run unconditionally so
  every observation participates (NO cap, no silent drop), and the
  report's third finding confirmed the arrays were write-only dead
  weight (~53 KB of BSS reclaimed for the DOS target).  THE CAP WAS
  UNREACHABLE IN PRACTICE: the 1-D constraint bounds a vertical
  range at 255 rows (my "600 observations" pin was impossible -
  REGR's Error 2 caught it; the pin uses the full legal 255).
  PINS: the clear invalidates, the failure invalidates, the load
  resets, the sort leaves SLOPE() intact, the full-255 regression
  (meany 257, slope 2, PROJ(300) = 601).  --test 854 ok.

- **THE FACTORIAL FIXES (the user's reports, 2026-11-16, branch
  "bugfix-alloc")**: (1) THE DRY PARSE EXECUTED SIDE EFFECTS:
  cs_parse_only routed through cs_eval_typed which set ps.dry = 0
  unconditionally - the factorial entry's "syntax check" RAN the
  prefix, and a conditional with SET executed TWICE per commit (the
  check + the evaluation).  FIX: cs_eval_typed gains a dry
  parameter; cs_parse_only passes 1, the other four callers
  (cs_eval_formula, cs_eval_fact_source, the COLIDX/eval-range
  paths) pass 0.  PIN: the counting-SET factorial entry
  +(1):SET(A1,VAL(A1)+1);0:1! leaves A1 = 1 (one execution), not 2.
  (2) factf_recompute EVALUATED OUTSIDE THE CONTEXT: the delegation
  from cs_recalc_entry_p happens before the normal path's bracket
  and factf_recompute never set one - ROW()! in row 3 recomputed as
  FACT(cursor row) after the cursor moved (the user's exact repro:
  6 -> 1).  FIX: factf_recompute brackets the own-cell context
  (save/set/restore at all four exits).  PIN: ROW()! at A3 stays 6
  across a cursor move + ;R A.  (3) THE UNPROTECTED PREFIX BUFFER:
  factf_recompute memcpy'd n-1 bytes into char pfx[CS_EDITMAX+1]
  with no length check - a LOADED oversized or EMPTY factf entry
  overflowed the stack (n-1 negative = a huge size_t).  FIXED AT
  BOTH CONSUMERS (the report missed that my batch-G copy path has
  the same shape): a factf content shorter than 2 or longer than
  CS_EDITMAX+1 makes the cell an ERROR (the same treatment as a
  failing prefix), no truncation that would evaluate the wrong
  formula.  PINS: the oversized/empty contents -> error cells, the
  copy of an oversized factf -> an error cell.  MY PIN TYPO: the
  guard assertion missed a negation (the empty cell's expectation).
  --test 842 ok; the pty shows ROW()! = 6.00 across the commit and
  two recalculations with the cursor moved away.

- **THE DEPENDENCY '&' FIX + THE REF POLICY (the user's reports,
  2026-11-16, branch "bugfix-alloc")**: (1) dep_check's delimiter
  list lacked '&' - the string batch added it to the FOUR cells.c
  scanner lists but not to command.c's, so +A5&" units" slipped the
  row-5 deletion check (the formula silently re-pointed at the
  former row 6).  FIXED (one token); the audit confirms all FIVE
  scanner lists now agree (dep_check + cs_adjust_refs/cs_shift_refs
  x2/cs_sort_refs, each with ! > , ) space : + - * / % \\0 < = &).
  (2) THE REF POLICY (the user's decision, with the multi-level
  indirection argument: VAL(REF(VAL(D1),VAL(D2))) through a
  VAL-chain - the address graph is unbounded, following it is not
  reasonable): REF's coordinates are DATA - NEVER renumbered by
  row/column insert/delete/copy/sort, and the dependency check does
  NOT attempt to follow them: a deletion PROCEEDS beside REF
  formulas (no veto), the chain re-resolves on the next
  recalculation.  The least-special-cased option: every text
  scanner naturally ignores REF( (no coordinate token) - no new
  code path.  Consequence documented in README's REF section: a
  formula whose address must follow the STRUCTURE uses a static
  reference; whose address is DATA uses REF.  PINS: the & repro
  found by dep_check_range (B1), the '&' through the copy shifter
  and the insert shifter, the user's six-cell indirection chain
  (resolves "test", survives an unrelated deletion), REF(2,7)
  never renumbered across a deletion, and the deletion proceeding
  beside a REF formula.  MY PIN BUGS: dep_check_range/cs_shift_refs
  take 0-BASED rows (I passed 5 = row 6 - three FAILs that were
  off-by-one pin args, no code bugs) and the text commit stores the
  typed quotes ("\"test\"" is content WITH quotes - the pin needed
  a bare "test").  --test 836 ok; the pty shows the refusal
  "ERROR: would delete ref(s) at B1" and the REF deletion
  proceeding.

- **THE SORT FIXES (the user's persistence review, 2026-11-16,
  branch "bugfix-alloc")**: (1) THE COMPARATOR'S INCONSISTENCY: the
  three key classes (numeric, text, formula-string) made text vs
  strres return +1 in BOTH directions - an inconsistent comparator
  (the user's repro: text "B" above a formula yielding "A" sorted
  wrong; worse, qsort's whole output is undefined once such a pair
  is compared).  FIX: ONE string class - ordinary text (cont) and a
  formula's string result (strres) compare through the SAME strcmp
  byte order; numbers first, empty keys last (unchanged).
  (2) THE RANGE REMAP POLICY: cs_sort_refs remapped the two
  endpoints of a range INDEPENDENTLY - a rearranged block could turn
  A1>A2 into A3>A1 (normalized A1>A3, silently including cells that
  were never referenced).  THE POLICY (deliberate, documented):
  coord>coord pairs in the block remap as a UNIT - the image of the
  row interval under the permutation: the image of the WHOLE block
  is the block itself (the extent is invariant - SUM(A1>A3) over a
  sorted block keeps covering the same data), a CONTIGUOUS partial
  image emits the normalized rectangle over [imn..imx], and a
  SCATTERED image cannot be represented: the range stays AS TYPED
  (flagged - the bounding box would silently over-include; the
  multi-area rewrite would corrupt REGR).  A top-level comparison
  shares the text shape and is treated the same way (a text-level
  scan cannot tell a range from a comparison - flagged limitation).
  THE EMISSION BUG I INTRODUCED AND FIXED: the endpoints' OWN
  images need not span the image set (the whole-block case:
  perm(A1)=3, perm(A3)=2 emitted rows 2..3 instead of 1..3!) - the
  rectangle is emitted over the image interval's min/max.  MY PIN
  BUG: tread(col,row) - the mixed-sort pins read B1/A3 instead of
  A2/A3 (the code was right all along).  PINS: the user's repro
  (asc + desc), the three-way ranking, the whole-block invariance,
  the contiguous partial image, the scattered leave-as-typed, the
  single-row remap.  --test 829 ok.

- **THE COPY POLICY (the user's persistence review, 2026-11-16,
  branch "bugfix-alloc")**: a copy reproduces the COMPLETE cell
  state.  (1) THE BLANK-SOURCE FIX: copy_one's final branch turned
  EVERY blank source (absent, empty, AND allocated-empty) into a
  CE_ALLOC destination - copying mostly-empty ranges consumed the
  cell budget and the sparse structure diverged.  NOW: the callers
  (cs_copy_block's loop, fill_one) check the source FIRST - a blank
  source FREES the destination (cs_free_cell_at, the complete state
  of "nothing" is nothing, the sparse structure mirrors the source,
  the budget returns) and only a REAL CE_ALLOC source copies as
  CE_ALLOC (the form-mode state is legitimate to reproduce).
  (2) THE FLAG CARRY: notation, formf and factf were not copied.
  THE FACTORIAL BUG (the user's repro): copying "5!" evaluated the
  content as an ORDINARY formula (entry syntax, not expression
  grammar) -> an error cell; NOW the factf source re-evaluates its
  (ref-shifted) prefix through cs_eval_fact_source + csn_fact and
  carries the flag ("A5!" copied down re-shifts to "A6!" and
  recomputes).  THE VALUE HOLDER (!...!): copies VERBATIM (content,
  stored value, flags) - evaluating it as an expression was the same
  bug class.  The CE_NUM/CE_TEXT/CE_ALLOC branches now carry the
  full flag set (just/prec/notation/formf/factf/volatf).  PINS: the
  blank over a populated destination frees it (+ the budget
  returns), the blank over an empty destination allocates nothing,
  the alloc state copies, "5!" copies with FACT(5) = 120 and the
  flag, the notation and formf carry.  MY PIN SLIPS: the first
  blank-copy pins copied POPULATED sources (A1=5 over B1=9) - the
  pins needed an empty SOURCE cell.  --test 822 ok; the pty shows
  the copied factorial as 120.00 (no error cells).

- **THE PERSISTENCE BUGS (the user's reports, 2026-11-16, branch
  "bugfix-alloc")**: (1) THE ALLOC-EMPTY CELL BROKE THE ROUND TRIP:
  the save writes every non-CE_EMPTY cell (type 1 = CE_ALLOC), but
  the hardened loader accepted only types 2/3 - a form-mode cell in
  the file was refused with READ ERROR (and the PRE-hardening loader
  silently DESYNCED on it - the record's payload parsed as the next
  record's header).  FIX: rd_check_record accepts ty in {1,2,3} (the
  type-1 payload shape = type 2's: cont + rep); pass 2 materializes
  it as CE_ALLOC with the flags (formf/prec/notation/factf/volatf/
  just), releasing the empty cont so the in-memory state is the
  native NULL-content one.  PINS: the alloc cell + its formatting +
  both neighbors + the honest entry count after the round trip.
  (2) THE PASSWORD-PROBE OFFSET BUG: cs_file_password (written for
  the CSD1 layout) skipped magic + 254 colw bytes = offset 258, but
  CSD4 carries the 3 ORIGIN bytes first - the probe read colp[A] as
  the password length and two column bytes as the password: EVERY
  overwrite asked "Verify the password to remove" and even the
  CORRECT password failed ("not ok") - the user's exact repro
  ("the file has no password" yet ENTER fails).  FIX: skip the 3
  origin bytes before the column table.  THE MESSAGE STAYS "not ok"
  (the user's instruction; the planned "password mismatch" is NOT
  applied).  PTY-VERIFIED THE WHOLE FLOW: save with secret ->
  wrong pw -> VERIFY + not ok (refused); correct pw -> VERIFY +
  SAVED (this path was ALSO broken before the fix); the now-
  passwordless file overwrites with NO verify prompt -> SAVED; the
  final pwdlen byte 0 (the remove semantics).  --test 815 ok.

- **THE VOLATILE SEMANTICS CHANGE (the user's call, 2026-11-16,
  branch "eng-batch-g")**: a random cell RETAINS its value once set
  - the whole-sheet drivers (;R A, RECALC(), the settle passes) SKIP
  the cell entirely (no redraw, not even re-evaluated); only the
  EXPLICIT re-roll paths redraw: ;R E (the cs_volatile_draw_set
  bracket around cs_recalc_entry in recalc_cmd) and RECALC(cell)
  (exec_recalc_stmt records a ONE-SHOT target: cs_recalc_target_* +
  cs_recalc_target_on, consumed by the cell's first visit in the
  settling passes).  The commit path draws naturally (it evaluates
  the entry text directly - not through cs_recalc_entry_p), so no
  bracket there.  THE SAME-TEXT GUARD: cs_commit_core returns
  without storing when the new entry text equals c->cont and the
  cell is volatile and NOT in an error state - re-affirming
  retains, redefining draws, an error state retries (the user's
  model: "retype +RAND(0,5) retains; change to +RAND(0,6)
  recalculates").  THE SENSOR SUPPRESSION is now defense-in-depth
  (the skip makes the volatile cell unable to destabilize the
  loop); kept.  Consequences documented: dependents stay stale
  until their own recalculation (;R A re-evaluates them against the
  new value while skipping the volatile cell).  THE PTY PROOF: the
  A1 value sequence across the flows = commit draw, two ;R A runs
  UNCHANGED, then ;R E -> a new value, ;R E -> another new value.
  PINS: +5 (the retain across ;R A twice - csn_cmp exact; the
  same-text recommit retains; the changed entry redraws in range;
  the ;R E triple-roll not-all-equal (statistical < 1e-12,
  flagged); the RECALC(cell) re-roll + no overrun).  --test 810 ok.

- **ENGINEERING BATCH G (user request 2026-11-15, branch
  "eng-batch-g")**: ISPRIME + the random family.  ISPRIME: the
  12-base deterministic Miller-Rabin (2..37, Sorenson-Webster -
  exact for x < 3.3e24), the 19-digit cap (modmul's full product
  must stay inside the 40-digit mantissa), x < 2 (negatives too) ->
  0, fractional/unextractable -> Error 0 (the EVEN/ODD contract),
  Carmichael numbers (561/1105/1729) fail correctly, the values
  verified against sympy.  TWO ENGINE LESSONS: (a) csn_div is the
  REAL quotient (1/2 = 0.5), NOT the integer one - the modpow bit
  extraction must SUBTRACT the bit before halving (the remainder is
  even -> the /2 is exact); the first draft looped 64 times on
  0.5^n and hit the overflow return, making every prime composite.
  (b) THE 19-DIGIT DISPATCH BUG: csn_to_long_val overflows above
  LONG_MAX (9223372036854775837 > ...807!) - the trivial checks
  (x < 2, == 2, even) now run in BCD (cmp/mod) and mr_test consumes
  the Csn directly.  RAND/RANDF/SEED: the BCD LCG (state·a+c mod
  10^40, Hull-Dobell-complete: a = 847562105789531461 = 1 mod 20,
  c = 1234567890123456789, period 10^40), the update splits the
  state base 10^20 so every product is exact (lo = (s0*a+c) mod
  10^20, hi = (s1*a + carry) mod 10^20); the DRAW uses the HIGH
  digits with REJECTION SAMPLING (the low digits of a decimal LCG
  are weak); the scale10(-20) extracts needed csn_floor (scale10
  alone leaves the fraction - the second silent bug, found by
  RAND(3,3) = 3.7278).  RAND: a uniform INTEGER on [min,max], the
  bounds truncate OUTWARD, min>max swaps; RANDF: [min,max) (flagged
  - the max is excluded by construction); SEED(x): sets the state,
  returns the value, so the pinnable draw is SEED(42)-SEED(42)+
  RAND(...) (SEED returns its value - my first pins added it!).  THE
  SEED: the wall clock * 10^6 + clock() mod 10^6 at first use (both
  ISO C89; time_t enters through its DECIMAL DIGIT STRING - the
  width is implementation-defined; %lu of the unsigned magnitude,
  bounded by analysis).  THE VOLATILE FLAG (the user's refinement -
  parse-time, not per-pass): Cell.volatf set by the commit wrappers
  (cs_entry_volatile scans RAND(/RANDF(/SEED( case-insensitively -
  the stored text keeps the case as typed); cs_set_text/cs_set_num
  CLEAR it (raw content commits), copy_one carries it, the SET
  preserve-formulas path writes val directly so it survives; the
  CSD record GAINED a volatf byte (save/load/rd_check_record
  updated - the format changes freely pre-release); the sensor in
  cs_recalc_entry_p suppresses the change on a flagged cell (the
  sheet settles; every ;R redraws).  examples/brainf.csd
  REGENERATED (the old file's record layout misaligned under the
  new byte; the genprobe updated to cs_alloc_cell, the file
  re-verified: loads, 1769 passes, "Hello World!" in S7).  THE
  UNIFORMITY: 6000 draws in [1,6] spread 956..1047 per face; 2000
  RANDF(2,5) draws all in [2,5).  PINS: 55 (ISPRIME's set incl.
  sympy-verified 19-digit cases, the seeded determinism, the range
  membership conditionals, truncate-outward, swap, min=max, RANDF
  bounds, the arg-count/type errors, the volatile flag + settling
  + CSD round trip).  VERIFIED: --test 805 ok; the idle gate
  EXACT; the pty shows the ISPRIME answers, the seeded draw, and
  the RANDF value.

- **ENGINEERING BATCH F + the dm2 follow-up (user, 2026-11-15,
  branch "eng-batch-f")**: ROOT(f, lo, hi) - the EXPRESSION-BASED
  root finder, the one architectural item of the list.  THE
  MECHANISM: the first argument is CAPTURED as raw text
  (root_arg_end scans for the top-level comma through quotes and
  parens - the branch-capture precedent) and re-evaluated per trial
  by root_eval (cs_eval_formula with a parser-level binding:
  g_bound_x/g_bound_on, save/restore bracket - a nested ROOT is
  safe); in the atom parser's word branch a bare X (any case) with
  the binding on yields the bound value.  SAFE by construction:
  is_coord_ahead ran first (X2 is a reference) and functions need (
  - X cannot collide.  BISECTION: the bracket normalized (lo > hi
  swaps), endpoint roots returned directly (f(lo) = 0 -> lo), the
  no-sign-change case -> Error 0, the loop: m = (a+b)/2, the
  tolerance break at |b-a| <= max(|a|,|b|)*1e-24 (RELATIVE - ~80
  steps from any bracket width, 300-pass cap), the answer is the
  interval's midpoint.  INTEGER ROOTS are reached only to the
  tolerance (2.9999999999999... possible) - the pins use comparison
  conditionals; sqrt(2)/pi have stable 14-digit canons (the true
  15th digit is 9).  CAVEATS (flagged, documented in README): side
  effects in the argument execute PER TRIAL (a fresh parse each);
  a string result is an error.  THE BINDING DOES NOT LEAK (bare X
  outside ROOT stays Error 6 - pinned).  MY PROBE BUG: I wrote '^'
  for the power operator (the grammar's is '**', the manual's) -
  four "failures" that were my own formulas; also SIN(X)-1 has no
  sign change on [0,2] (it touches zero without crossing - the
  correct refusal).  THE dm2 FOLLOW-UP (the user's addition):
  CONV's area dimension gains dm2 (1/100 m2) - the ladder now
  mm2/cm2/dm2/m2/hectare, exact round trips.  A pin slip of mine:
  the dm2 round trip used "km2" which is not a unit.  PINS: 4
  (dm2) + 18 (ROOT).  VERIFIED: --test 768 ok; the idle gate
  EXACT; the pty shows ROOT sqrt2 = 1.414213562373095048801689,
  ROOT pi = 3.141592653589793238462642, the refusal, and the bare
  x Error 6.

- **ENGINEERING BATCH E (user list 2026-11-15, branch "eng-batch-e")**:
  SPLINE + DIFF.  THE REFACTOR: pair_ranges SPLIT into pair_parse
  (the two aligned ranges + optionally a trailing scalar t - DIFF/
  SPLINE need t BEFORE the walk) and pair_walk (the shared row-major
  loop); CORR/INTEG updated to the two-call shape.  DIFF: the
  piecewise-LINEAR slope at t, STREAMING (no buffer - unbounded, the
  513-point pin shows it succeeding where SPLINE errors); t at an
  interior knot takes the segment to the RIGHT, at the last knot the
  last segment (the q/pair-before-last bookkeeping); t outside the
  span -> Error 0.  SPLINE: the knots collected into bounded buffers
  (SPLINE_MAX 512, the MED bound - the define is its OWN 512 because
  MED_MAX is declared later in the file), the natural cubic: M via
  the THOMAS ALGORITHM in BCD (the sub/super are the h values
  themselves - no a/c arrays; the back substitution overwrites dpr
  in place; the system is strictly diagonally dominant: 2(h1+h2) vs
  h1+h2 - no zero pivots), evaluated as
  S = A*y1 + B*y2 + ((A^3-A)*M1 + (B^3-B)*M2)*h^2/6; n = 2 is the
  straight line; the knots reproduce EXACTLY (A = 1 -> A^3-A = 0 in
  BCD); t outside the span, unordered x, holes, n < 2, the 513-point
  bound -> Error 0.  The worst-case transient is 5 Csn arrays
  (~130 KB, flagged; each under 64 KB).  THE MATH CHECK: the x^2
  natural spline is NOT the parabola (M0 = 0 forced - 0.3392857...
  at t = 0.5, derived and pinned); the arch (0,0),(1,1),(2,0) gives
  M1 = -3 and 0.6875 at the midpoints.  PINS: 45 (the arch
  symmetric values, the x^2 table incl. exact knots, the linear
  two-point case, the self-range case, DIFF's knot/last/outside/
  uneven cases, all the error paths, the 512 bound, DIFF
  unbounded).  AUTHORING SLIPS: three (the DIFF "bound" pin - DIFF
  streams; the A/B seeds cleared before the knot pin; a "hole" pin
  with no hole).  VERIFIED: --test 746 ok; the idle gate EXACT; the
  pty shows SPLINE = 0.6875, DIFF = 1, and the out-of-span error.

- **ENGINEERING BATCH D (user list 2026-11-15, branch "eng-batch-d")**:
  CONV(value, from, to).  THE TABLE: ~46 units in 15 dimensions,
  every factor an EXACT RATIONAL num/den to the canonical unit -
  psi = 44482216152605/6451600000 Pa and HP = 745699871582270220/
  10^15 W are the exact DEFINING rationals, not the rounded 16-digit
  standards (their true decimals are non-terminating).  Names 7-bit
  ASCII (mm2/cm2/m3/ohm/kohm/farad/uf/km-h/mph), lookup
  case-insensitive (conv_eq).  THE ELECTRICAL GROUP is five separate
  dimensions (potential/current/resistance/capacitance/inductance) -
  V to A is a nonsense conversion and is refused.  TEMPERATURE is
  the one non-multiplicative dimension: staged through Celsius
  (F: (x-32)*5/9; K: x-273.15).  THE NAME CLASH: bare "f" =
  Fahrenheit, the farad = "farad"/"uf" (flagged; the user's list had
  both on F).  THE UNIT ARGS deref to strings (literal or TEXT CELL
  - CONV(100,B10,"m") works; conv_unit_arg); wrong type Error 6;
  unknown unit/dimension mismatch Error 0; arg count Error 3.
  Round-trip honesty: terminating factors round-trip EXACTLY (mile
  pinned); non-terminating ones to the 30-digit honest core (kg
  pinned as a tolerance conditional - the canon string would be
  2.9999999999999E+1).  PINS: 36.  THE SI CUBIC VOLUME SCALE (the
  user's addition to batch D, 2026-11-15): mm3/cm3/dm3/km3 join
  ml/l/m3 in UDD_VOL - 1 mm3 = 1e-6 L, 1 cm3 = 1 ml and 1 dm3 = 1 l
  BY DEFINITION (both equivalences pinned), 1 m3 = 1000 L,
  1 km3 = 1e12 L; all factors terminate so every pair round-trips
  exactly (the full-ladder mm3<->km3 trip pinned exact).  THE CANON
  GOTCHA bit six times in a row (1000 = +1.0E+4 not E+3; 1e-9 =
  +1.0E-8; 3 = +3.0E+1) - all six engine values were RIGHT, my
  hand-derived exponents were wrong; every one now cross-checked
  against exact decimal arithmetic.  ALSO IN THIS BATCH (the user's
  request): the Makefile CFLAGS gained -Wextra -pedantic-errors (the
  user added the flags; one warning surfaced - side_effect_stmt's
  oldtype was an int vs the CellType enum field; now typed CellType).
  Zero warnings under the strict flags.  VERIFIED: --test 704 ok;
  the idle gate EXACT; the pty shows CONV(1,psi,Pa) =
  6894.757293168361336722673445 and the dimension mismatch refusing.

- **ENGINEERING BATCH C (user list 2026-11-15, branch "eng-batch-c")**:
  the paired two-range functions: CORR, RSQ, INTEG.  THE PAIR WALKER
  (pair_ranges): two aligned ranges, shapes must agree (the user's
  locked rule -> mismatch Error 0), row-major walk, a `holes` flag
  selects the pairing policy (the statistics SKIP a pair when either
  side has no number; the table functions treat a hole as an Error -
  the locked rule); a visit CALLBACK accumulates (visit returns 0 =
  ok, nonzero = failure -> the caller maps to Error 0 - the first
  draft returned the csn error code 0 = "success", a silent-bug trap
  caught before the first run).  CORR: r = (n*sxy-sx*sy) /
  sqrt((n*sxx-sx^2)*(n*syy-sy^2)), zero variance on either side or
  n < 2 -> Error 0; RSQ = num^2/denom (never negative by
  construction).  INTEG: the trapezoidal rule over consecutive
  pairs, x STRICTLY increasing in walk order (duplicates and
  decreasing -> Error 0), n < 2 -> Error 0.  Reversed ranges
  normalize independently (the established rng rule) - pinned.
  PINS: 21 (perfect/negative/mixed correlations, the skip rule with
  a text hole, zero-variance, shape mismatch, non-range Error 6,
  arg-count Error 3, the x^2 trapezoid 9.5 over [0,3], the two-point
  exact integral, holes, unordered x).  AUTHORING SLIPS: (a) my
  "zero variance" pin data was actually perfectly correlated - the
  constant-range seed was needed; (b) I transposed the two-point
  INTEG seed.  THE PTY GOTCHA (again, worse): the data-entry commit
  KEEPS the cursor - RETURN with an empty buffer moves RIGHT, so
  column seeding needs ^X (down) after every commit; my first two
  scripts scattered the digits across row 1 (n=1 -> the CORR Error 0
  was CORRECT behavior, the seeds were wrong).  Diff-painting also
  hid the later ?-answers (identical/near-identical frames paint
  nothing) - the pins are the authority.  VERIFIED: --test 668 ok;
  the idle gate EXACT; the pty shows the live CORR = 1.0 and the
  seeded columns.

- **ENGINEERING BATCH B (user list 2026-11-15, branch "eng-batch-b")**:
  the statistics + percentiles.  STREAMING: CsAgg gained sumsq/mean/
  m2; SUMSQ/RMS accumulate the running sum of squares (RMS = sqrt of
  it); VAR/VARP/STDEV/STDEVP run WELFORD'S SINGLE PASS in agg_item
  (mean += (x-mean)/n; M2 += (x-mean_old)*(x-mean_new) - no buffer,
  no second walk, numerically stable; an intermediate overflow is the
  family's Error 0).  EDGE TABLE (the user's): VAR/STDEV n<2 ->
  Error, VARP/STDEVP n=1 -> 0 (M2 = 0 naturally), n=0 -> Error for
  the whole family incl. RMS; SUMSQ of nothing -> 0 (the SUM rule).
  PCT/QUART: PERCENTILE.INC through the MED buffer (med_collect_arg
  refactored OUT of parse_med_collect so PCT collects ONE argument;
  then the whole-list design: parse_med_collect collects EVERYTHING
  and the LAST argument is the percentile - that resolves
  PCT(1,2,3,4,0.5) = data 1,2,3 at p 0.5); QUART q integer 0..4
  (csn_integer_part + cmp equality rejects fractional/oversized),
  p = q/4; rank = p*(n-1), lo = floor (csn_floor), interpolate
  between the sorted neighbors; empty data / p outside [0,1] /
  fractional q -> Error 0 (unlike MED's empty -> 0 - flagged: a rank
  of nothing is undefined); MED_MAX 512 shared.  GOTCHAS: (a) my
  first one-arg PCT design broke PCT(1,2,3,4,0.5) (Error 3 - the
  scalars are separate arguments); (b) the VAR-overflow pin was
  wrong - WELFORD NEVER SQUARES THE VALUES, so VAR(1E500,1E500) is
  legitimately 0; the overflow needs deviation products beyond the
  range (VAR(1E501,3E501) -> Error 0).  PINS: 56 (range seeds
  A1..A5 = 1..5, the Wikipedia {2,4,4,4,5,5,7,9} example, the edge
  table, PERCENTILE.INC endpoints/midpoints, error cells, overflow).
  VERIFIED: --test 647 ok; the idle gate EXACT; the pty shows
  STDEV(wiki) = 2.138089935299395077476427, VARP = 4, PCT = 1.75,
  RMS(1,2,3) = 2.160246899469286743655322 (a QUART fragment "3.2"
  was a repaint artifact - the probe shows 3.25 exactly).

- **ENGINEERING BATCH A (user list 2026-11-15, branch "eng-batch-a")**:
  the scalar math + EE functions.  ENGINE: csn_floor/csn_ceil (from
  trunc_int + the sign of the fraction; a fraction beyond the
  mantissa's honest digits reads as none) and csn_hypot (the SCALED
  form m*sqrt(1+(n/m)^2) - the direct x*x+y*y overflows the exponent
  range near 1E500 while the result is representable; n/m underflow
  -> 0 is honest: n negligible next to m).  TRUNC_INT BASEEXP FIX
  (found writing this batch): trunc_int passed baseExp 0 always, so
  any value with exp10 > CSN_DIGS truncated to exp10 = the digit
  count (1E500 -> 1E40); now baseExp = a->exp10 - n (identical for
  exp10 <= CSN_DIGS, correct above).  PARSER: the function-name
  scanner accepts a DIGIT TAIL sealed by '(' (ATAN2 - the name
  scanner stopped at digits, which is why LOG10 never existed); safe
  because is_coord_ahead runs first, so a word that letters+digits
  make a coordinate never reaches the name scan - a function name
  may never LOOK like a coordinate (2 letters max + digits).  ATAN2
  is eng_atan2 (quadrant logic on atan(y/x) +- pi, x=0 -> +-pi/2,
  (0,0) -> domain error per the user).  DB = 20*log10(x) (x > 0),
  DBM = 10*log10(w)+30, DBW = 10*log10(w), LIN = 10^(dB/20), STEP
  (0 below, 1 at-or-above - STEP(0)=1 Heaviside, FLAGGED), CLAMP
  (lo>hi -> Error 0, FLAGGED), DIST through csn_hypot, LERP
  (t outside [0,1] extrapolates, FLAGGED), UNLERP (b=a -> Error 0).
  PINS: 62 (all values verified against 60-digit decimal arithmetic;
  canon gotcha bit once more - 100 = +1.0E+3, not E+2).  VERIFIED:
  --test 591 ok; the idle gate EXACT; the pty ?-calculator shows
  HYPOT=5, ATAN2(1,-1)=2.356194490192344928846982, CEIL(-3.2)=-3,
  DBM(1)=30, LIN(-20)=0.1, CLAMP=10.0, UNLERP=0.5.

- **THE ZADEH FUZZY BOOLEAN ALGEBRA (user spec 2026-11-15, branch
  "fuzzy")**: NOT/AND/OR/XOR in the parser dispatch, plain numeric
  results (VT_NUM, never a new type), all exact BCD (min/max and
  1-x never round — the section-5 properties hold exactly, De
  Morgan pinned as a self-comparison conditional instead of
  tolerances).  NOT(x): 1 arg via parse_numarg, 1-x; AND(...)/
  OR(...): MULTI-OPERAND through the streaming machinery
  (agg_item's FN_FAND/FN_FOR modes: the [0,1] domain check ->
  Error 0, then min/max tracking — RANGES work, the strict mixing
  rule applies); XOR(x,y): EXACTLY 2 args (Error 3 otherwise),
  MAX(MIN(x,1-y),MIN(1-x,y)).  The domain [0,1] enforced per
  operand, outside -> Error 0 (no clamping).  THE THRESHOLD:
  FUZZY_TRUE_THRESHOLD = "0.7" (the define in parser.c; the BCD
  constants cs_fuzzy_zero/one/true built once by cs_fuzzy_consts)
  applied at EXACTLY ONE site: the conditional truth test
  (numeric/BOOL conditions pass the threshold; STR conditions stay
  truthy-nonempty; comparisons produce exact 0/1 and are
  unaffected).  FLAGGED BEHAVIOR CHANGE: a bare numeric condition
  in (0, 0.7) or negative is now FALSE (was nonzero-true) — the
  user confirmed 0.7 is the only threshold.  The threshold is
  NEVER applied inside the operators (AND(0.6,0.9) = 0.6, pinned).
  BONUS: the >= comparison operator added (the mirror of <= — the
  user's threshold tests reached for it and it never existed; the
  numeric and string comparison switches both handle op 6).
  PINS: the spec's full test list (all four operators, the
  threshold trio, the invalid inputs, multi-operand, De Morgan
  both ways, double negation, every property) — 508 checks total.
  pty: the calculator answers AND=0.6/NOT=0.75/XOR=0.7/OR=0.9 and
  the conditional cells.

- **THE MERGE (2026-11-15)**: the "fuzzy" branch merged to master
  (fast-forward) and deleted; README's fuzzy section gained the
  cell-based worked example (A1 = 0.3 with NOT/AND/OR/XOR, the
  threshold conditional with strings) and the comparison-operator
  list (< <= = <> >= >).

- **EVEN()/ODD() (user request 2026-11-15, branch "parity")**: the
  integer parity functions — 1 if the integer x is even/odd, 0
  otherwise.  One dispatch block sharing CHR's mechanics:
  parse_numarg -> csn_integer_part -> csn_to_long_val -> cc % 2.
  A FRACTIONAL or oversized argument is Error 0 (the
  csn_integer_part contract — evenness is a property of integers,
  no truncation; the user's default).  Negatives correct (cc % 2
  handles both signs).  The 0/1 results compose with the 0.7
  threshold conditionals and with NOT (EVEN = NOT ODD, pinned).
  GOTCHA: ckc can't pin string-result branches — the conditional
  pin used the inline cs_eval_formula + sf check.

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

## THE UI-REDRAW ARCHITECTURE (branch "uidraw", 2026-11-16, in progress)

Goal: maximum interactive performance on slow terminals (the user's
Raspberry Pi test showed a full-screen flash on every key). Phased
commits, each with its own verification gate; the whole thing merges to
master when every phase passes.

- **Phase 1 (DONE): `clear()` removed from `cs_draw()` + explicit
  blanking.** The flicker root cause was the full `clear()` + repaint in
  every `cs_draw` — the terminal showed an erase flash, and every repaint
  re-emitted ~900 bytes. The replacement discipline: **each paint
  function blanks the lines it owns first, then writes content over the
  blanks.** ncurses diffs the virtual screen against the physical, so a
  blank over an unchanged blank emits ZERO bytes — the erase flash is
  gone and the byte cost is the real change set only (measured: the bench
  sequence dropped 2842 bytes / 4 clear-frames → **1259 bytes / 1
  clear-frame**, the remaining clear being ncurses's own startup clear).
  Ownership map (the blanking call sites):
  - `draw_menu` blanks lines 0..4 in BOTH modes — the extended toggle
    (`*`) must wipe the menu text off the screen (draw_menu paints only
    when `!extended`; the old clear() did the wiping).
  - `draw_headers` blanks the column/row header lines.
  - `draw_rows` blanks the WHOLE data window (all `cs_win_rows()` lines)
    before rendering — the separator gaps between cells, the row-label
    gap and the lines past the last sheet row are painted by NOTHING
    else; a scroll or a short sheet used to leave ghost content there
    (found by the frame-identity harness, see below). The cursor marks
    (`>` `<`) are erased by the same blank.
  - `draw_cursorline` blanks line_box, line_cursor AND the layout's
    spacer row `line_cursor + 1` — that row is owned by NO component
    ("one blank line between" in cs_compute_layout); help-page text
    used to ghost there forever after `;H` closed (found by the
    harness).
  - `draw_typecont` blanks line_type/line_cont (variable-length type
    text: "text: right justified" then "numeric" used to ghost "ified").
  - `draw_editline_idle` blanks line_edit (the idle case paints only
    "edit: ").
  - The transient message row is wiped explicitly in `cs_draw` (the
    pre-existing behavior, kept).
  - `help.c` still calls `clear()` for its own pages — self-contained,
    acceptable (the pages are static full-screen views); the 'H' key
    case repaints the main screen after.
- **The frame-identity harness (the phase gate)**: `tests/frame_harness.py`
  (run `python3 tests/frame_harness.py /tmp/cs_ref [--upto N]` from the
  project root; the reference worktree build defaults to /tmp/cs_ref) —
  a 15-segment scripted interaction (goto+commit, scrolls, far goto,
  format, lock, extended toggle ×2, ;R A, copy, both help pages, row
  delete, ESC), run against the NEW binary and the pre-change binary
  (a worktree build of the pre-change commit), every cumulative prefix
  rendered through `tests/pty_render.py` and the final frames COMPARED
  — all 15 checkpoints IDENTICAL after the fixes above. Captures are
  slow (~2-3 min); run it in the background.
- **The debug-tool battery lives in tests/ (2026-11-16, user request)**:
  `pty_render.py` (the full-CSI pty frame renderer), `frame_harness.py`
  (the 16-checkpoint frame-identity driver), `pty_driver.py` (the
  READING pty benchmark driver), `bench.exp` (the byte-count bench),
  `idle_capture.exp` + `idle_gate.py` + `cs1_reference.txt` (the
  idle-gate capture + comparison), and `pty_render_selftest.py` (the
  renderer's synthetic-CSI gate — it caught the CSI regex missing the
  `@` ICH terminator on the day it was committed). Scratch captures go
  to tests/.scratch/ (gitignored).
- **The harness renderers (learned the hard way)**: `tests/pty_render.py`
  now implements `ESC[J` ED with params (0 = cursor→end, 1 =
  start→cursor, 2 = full), `ESC[K` EL the same way, and DCH/ICH/ECH/IL/
  RI — pinned by `tests/pty_render_selftest.py` (the CSI final byte
  class must be 0x40-0x7e: the old `[A-Za-z]` class silently DROPPED
  ICH sequences and one hand-mangled renderer lost paints — two false
  "ghost" alarms). Capture scripts must send control keys with Tcl
  double quotes + escaping (`send "\x18"`) — `send {\x18}` in braces
  sends the LITERAL text (a whole stress run typed "\x18" strings into
  cells). ^B is \x02, not "^B".
- Phase 1 gates: --test all pass, idle gate MATCH, gcc-14 zero
  warnings, the stress walk (2469 bytes) without crashes.
- **Phase 2 (DONE): the paint components extracted, zero behavior
  change.** `cs_draw` delegates to four component painters in the
  historical order: `paint_menu` (menu + memwarn — the warning line
  sits on a menu row), `paint_header`, `paint_grid`, `paint_status`
  (the message-row wipe + cursor/type/contents/edit lines). All 15
  frame-identity checkpoints identical; the bench stays 1259 bytes.
- **Phases 3 + 4 (DONE, one commit — see the note below): the
  dirty-flag engine + the mutator conversions.**
  - The machinery: `cs_touch(what)` marks, `cs_paint()` paints the
    marked components and clears the flags, `cs_touch_all()` marks
    everything. The flags live in cs.h (`CS_D_MENU / CS_D_HEADER /
    CS_D_GRID / CS_D_STATUS / CS_D_CURSOR`); the common tail (startup
    banner repaint, prompt-mode end, `place_cursor`, `refresh`) always
    runs in cs_paint — the physical cursor rests correctly even when
    the flags were clean.
  - `redraw()` = cs_touch_all + (paint now, or defer while keys are
    queued). The main loop's drain flush calls `cs_paint()`, and the
    post-switch flush paints what the mutators marked — a paste still
    paints exactly once, on drain.
  - **THE NO-DIRTY-FLAGS CONTRACT: a direct cs_msg() message survives
    a no-touch flush.** cs_paint with clean flags does NOT run
    paint_status, so the message row is not wiped — "Bad coord", the
    ?-calculator answer and the sticky error line persist until the
    next interaction exactly like the old no-redraw paths. Corollary:
    a path that paints a message directly must NOT mark STATUS on that
    path (or its own flush wipes the message first); the ordering-
    sensitive sites use `touch_flush(...)` (main.c) / `crepaint(...)`
    (command.c) — mark, then paint-or-defer BEFORE the message paints.
  - The mutator mapping (converted this phase): cursor steps
    (`cs_touch_move` — scroll-aware: a scroll adds HEADER+GRID, a pure
    in-window step is CURSOR+STATUS), every entry-engine path (typing,
    backspace, the ^ toggle, ESC, ';'/'='/'"' in-entry → CURSOR+STATUS),
    the commit (`touch_flush(GRID|STATUS|MENU)` before the sticky
    error), the ?-calculator and the @ resolve (`touch_flush`), the DEL
    key, the ^ cell-type toggle, ^B (`GRID|STATUS` — the code shows
    showForm changes the CELL RENDER, not the menu: the draft table's
    "MENU+STATUS" was wrong), the recalc driver (GRID per changed
    pass — the status rows no longer blank per pass), goto (scroll-
    aware), lock (HEADER|GRID|STATUS), Edge, Order (STATUS), the
    extended toggle (touch all — every line moves), Help (touch all —
    the pages clear() themselves), format (HEADER|GRID|STATUS), and
    the 15 `cs_prompt_*`-then-repaint pairs (crepaint(STATUS) — they
    only clear the prompt line).
  - The remaining command dialogs (copy/delete/insert/save/load/print/
    sort/quit/what/?) still call redraw() = touch all — the fallback.
    Phase 4b can narrow them; the hot per-keystroke paths are done.
  - **CS_D_CURSOR is a COMPONENT, not yet fine-grained**: phase 4's
    paint_cursor() = paint_grid() (the whole data window). Phase 5
    replaces it with the two-cell fine-grained paint (paint_cell_at +
    the mark-gap spaces). The flags are already wired for it.
  - Phase 3+4 landed as one commit: the phase-3-only verification run
    (ck 0-2 identical) was interrupted by a rebuild that mixed phase-4
    edits into the tested binary — the harness gate for the combined
    state is the authoritative one. LESSON: never `make` while a
    frame-identity run is in flight; the harness spawns the tree's
    binary per checkpoint.
- Remaining phases: 5 = fine-grained cursor; 6 = static menu painted
  once; 7 = final measurement. Merge to master when all pass.
- **Phase 5 (DONE, `b34103a`): the fine-grained cursor.** The
  simplification that fell out of reading `cell_render`: it never
  consults `g_editstate` (pending entry text lives ONLY in the edit
  line and the type line) and every cell content change marks the GRID
  — so the CURSOR component paints ZERO cells: `paint_cursor` erases
  the two old mark gaps (plain spaces) and paints the two `A_REVERSE`
  marks at the new position — 4 single-character writes instead of a
  full grid paint. `draw_rows` syncs the tracked mark position
  (`markCol`/`markRow`) at the end of every full grid paint; a scroll
  never reaches the fine-grained path (it marks the GRID too, and the
  full paint syncs the tracking). `paint_cell_at` was dropped as
  unnecessary: no cursor path changes cell content.
  Verification: 16/16 checkpoints, the move-sequence pty comparison,
  --test, the idle gate, gcc-14.
- **Phase 6 (DONE, no code change): the static menu by construction.**
  The audit: `CS_D_MENU` is marked only by `cs_touch_all` (the redraw
  fallback: startup, resize, help-close, the extended toggle, the
  errpend key, load) and by the commit/DEL (which change entriesUsed —
  the memwarn). The truly hot paths (cursor steps, typing, backspace,
  ?, @) never mark MENU, so the five menu lines are painted only at
  startup/resize/toggle — "painted once" is already true through the
  phase-4 flags.
- **Phase 7 (this commit): the measurement table.** All numbers on
  this machine (macOS, M-series; the pty writes through a pipe — a Pi
  through a serial USB-tty pays the byte counts literally):
  | interaction | master (pre-uidraw) | uidraw |
  |---|---|---|
  | the 40-key bench sequence | 2842 B, 4 full-erase frames | 1259 B, 1 (ncurses's own startup clear) |
  | 25 cursor moves | 2818 B | 1302 B |
  | 10 commits (type+RETURN) | 2906 B | 1157 B |
  | bytes per cursor move | (full repaints per key) | 74.7 (marks + status diff) |
  | CPU, 400 unbatched moves on a seeded 7×11 numeric sheet | 0.242 s user | 0.147 s user (−40%) |
  The structural claim: `paint_cursor` is O(1) (4 char writes) where
  the pre-branch paint was O(screen) per key with a full erase; the
  dirty flags make every hot path paint only what changed. The bytes
  per move are EQUAL at the phases-3+4 and phase-5 stages — ncurses
  diff-suppression absorbs the grid repaint's bytes — so phase 5's
  measurable win is CPU (and it scales with the per-cell render cost:
  BCD formatting on a slow CPU); the byte win came from the phase-1
  clear() removal.
  - **THE PTY-DEADLOCK LESSON (2026-11-16)**: the first CPU numbers
    (~11.8 µs/move) were measured with a python pty driver that never
    read the master during its sleeps — the app's paint output filled
    the pty buffer, the app blocked in write(), stopped reading input,
    and everything flushed at EOF in one giant burst (the app's CPU
    read ~3 ms total). The honest numbers (a READING driver,
    /tmp/cpudrv3.py) are 50× larger and show the real −40%. Corollary:
    every pty driver must pump the master while waiting. The expect
    scripts are safe (log_file pumps), but their per-key `sleep`s
    still batch the paints — the frame-identity harness is a
    final-state comparison by design.
- **Phase 4b (DONE): the remaining command repaints narrowed.** copy
  (both the fill and the plain branch) → GRID|STATUS|MENU; the row/
  column/entry deletes → GRID|STATUS|MENU (the ALL-delete adds HEADER —
  it resets the window and the locks, whose locked-column marks live in
  draw_headers); the recalc tail → GRID|STATUS (the driver already
  painted the changed passes); the sort → GRID|STATUS; quit-cancel and
  the pure-message commands (storage `?`, What) → STATUS; the auto
  course's inner loop (stop-entry, per-keystroke echo, abort, complete)
  → crepaint(CURSOR|STATUS / GRID|STATUS). command.c's now-unused
  static redraw() removed. **crepaint paints UNCONDITIONALLY** (no
  defer): a deferred dialog paint has no guaranteed drain — a pasted
  burst that ends by quitting would flush a stale screen at endwin
  (the frame test caught a committed cell never painted). insert/
  delete/load/merge/auto-start and the help pages keep touch-all.
  Verification: the 16-checkpoint frame-identity harness, the
  sort/save/print/auto/quit-cancel pty comparison vs master (render3),
  --test, the idle gate, gcc-14 zero warnings.

## THE CSV IMPORT/EXPORT (user spec 2026-11-17, RFC 4180 in docs/)

- The dispatch: a `.csv` extension (any case) in `;S`/`;L`/`;M` takes
  the CSV path; the name is used AS TYPED (cs_savename would append
  `.csd`). The save dialog keeps its flow (the file-exists warning,
  then P)artial/A)all + the corners) minus the password prompts — CSV
  has none.
- **Export**: plain 7-bit ASCII. Numerics as **17 significant digits**
  (`cs_csv_num` in cells.c — the precision an IEEE-754 double holds,
  per the user: "this is also the precision of IEEE754 which is used
  by many spreadsheets"): TRUNCATED (the engine identity — never a
  digit above the true value), trailing zeros trimmed, plain decimal
  in [1e-4, 1e17) and scientific outside (the `%.17g` rule). Text as
  typed with the RFC quote-doubling and quoted commas; `?n?` error
  cells export as the token; the derived string type exports its
  strres. Embedded CR/LF **flatten to a space on both sides** (the
  grid cannot hold a newline; flagged: the round trip stays
  consistent, tools that preserve the break do not). Empty cells as
  empty fields, trimmed to the **used range** (the trailing empty
  fields/rows dropped; leading/embedded empties stay — positions);
  CRLF separators, no trailing CRLF.
- **Import**: a UTF-8 BOM skipped (`csv_open_bom`), bytes >127
  dropped, a field is **numeric only when the whole field parses as a
  pure numeric literal** ("123 pages" stays text; the RFC's quotes
  are delimiters, NOT type markers — the user's confirmed rule),
  text commits with **no formula evaluation**, no recalculation; the
  load anchors at the requested corner; `;L` replaces, `;M`
  overlays. **Two-pass like the CSD loader**: over-tall/wide →
  READ ERROR, over-budget → MEMORY IS TOO LOW, the sheet untouched;
  the clear happens after pass 1.
- Bugs found while building: the placement parsed INTO the cell's own
  `val` — cs_set_num's cs_free_cell zeroes it first, so every numeric
  became 0 (the self-pointer class; the parse target is a local
  now); the All-mode export first emitted a 255×127 comma desert
  before the used-range trim.
- Pins: 13 exact `cs_csv_num` cases + 1/3, 2/3 through the engine,
  the export's exact bytes, the import battery (the BOM, the utf8
  drop, the quoted numerics, `+7`, `1.5E-3`, `123 pages`, the doubled
  quotes, the newline flatten, the ragged rows, the 300-row
  refusal). PTY-verified: `;S t_pty.csv A` → `12.5,hello` (the used
  range); `;L` the csv → A1=12.50, B1=`hello` text.

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
