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
  calculator (no store, 12 decimals shown).
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
- Save format is `CSD3` (magic "CSD3", per value: sign byte, exp10 LE i16,
  ndig byte, 22 digit bytes). Older magics (`CSD1`, `CSD2`) are refused
  with the manual's `READ ERROR`.
- Preserved message strings (do not reword; cross-checked against BOTH
  manuals' error lists): `?n?`, `Error <n>-> %s  hit space`,
  `Room for %d entries`, `MEMORY IS TOO LOW`, `ERROR: would delete
  ref(s) at %s`, `bad coord`, `bad range coord`, `Width must be 3 - 63`,
  `Prec. must be 0 - 12`, `can't create`, `can't open` (print output),
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
   =36.6900...).
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
- Trigonometry inputs/outputs are **degrees** (Sorcim-era convention; the
  trig set is not documented in either manual).
- The `?`-calculator shows 12 decimals with no zero-stripping (literal
  reading of 1982 manual 4.1).
- Precision range is `0 - 12` (Appendix C) even though one chapter says
  "0 to 10 places" — Appendix C wins.

## Source map

| file | responsibility |
|---|---|
| `src/num.c/h` | BCD engine: Csn type, add/sub/mul/div/scale, parse/canon/fixed/sci formats, pow, trig (degrees), ln/exp/log/sqrt, round30 |
| `src/parser.c` | expression grammar: numbers, refs/ranges, `SUM/CNT/AVG/MAX/MIN`, math+linear functions, conditionals, `%` postfix, `**`, `\` comments |
| `src/cells.c` | cell store, coords, ref scanners, `cs_fmt_num`, REGR state |
| `src/eval.c` | `cs_recalc_*` (manual-only), commit path incl. `?` calculator, comments |
| `src/screen.c` | layout, menu/status rows, `place_cursor`, prompts/messages |
| `src/command.c` | `;`-commands,	goto, format (0..12), delete dependency check |
| `src/fileio.c` | CSD3 save/load/merge, password dialogs |
| `src/print.c` | reports to .TXT (typed name, or `.TXT` auto-extension), PRINTER = `CALCSTAR.PRN` spool, `>name` = `.DTA` records, display; page breaks |
| `src/help.c` | help pages |
| `src/main.c` | key loop (arrows, `@` refs, errpend), CLI arg load, `--test` |
