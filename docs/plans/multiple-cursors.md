# Multiple cursors for kg

Status: in progress.

## Goal

Parallel editing at several buffer positions at once, the way VS Code /
kakoune do it -- NOT the Emacs `multiple-cursors` package, which replays
keystrokes at fake cursors and breaks on prompts, isearch, yank-pop, and
undo.

## Model

- State: `editor.mc_row[]` / `editor.mc_col[]` (buffer-absolute byte
  positions, max 64), `editor.mc_count`. The primary cursor stays
  `editor.cx/cy`. MC is active whenever `mc_count > 0`.
- `C-g` clears the extra cursors (kg's universal cancel).
- MC state is per-buffer: saved/restored in the buffer slot like mark
  and rect_mode.

## Execution

For a parallel-capable keystroke:

1. Merge the primary cursor into the set.
2. Sort descending by (row, col).
3. Apply the edit at each cursor, highest first. Descending order keeps
   the UNPROCESSED cursors valid (they sit above-left of each edit).
4. Each cursor's new position = the natural end of its own edit.
5. After each edit, adjust the already-computed RESULT positions for
   that edit's effect (see "Position adjustment" below).
6. Dedupe overlaps, extract the primary, redraw.

## Position adjustment

Descending order protects the unprocessed cursors, but NOT the result
positions of already-processed ones: splitting a higher line pushes a
lower result down a row, deleting a column left of a higher result pulls
it left.  So every edit records its effect (kind + coordinates) and
mc_run applies it to all previously computed results before the next
cursor is processed:

  kind 1  column edit: bytes right of a point shift (insert/delete char)
  kind 2  line split: rows below +1; same-row text right of the split
          moves to (row+1, col-split)
  kind 3  join-prev (backspace at BOL): row r merges onto r-1
  kind 4  join-next (delete at EOL): row r+1 merges onto r
  kind 5  text insert (yank): K newlines, last-line length L

Without this, RET (and joins) left secondary cursors on stale positions
-- e.g. two cursors, RET, and the lower cursor stayed on the blank line
instead of following its text down.  Regression tests:
mc-newline-cursor-tracking, mc-newline-eol-tracking,
mc-backspace-join, mc-same-row-backspace (each fails with the
adjustment disabled).

## Parallel command set (v1)

Only these act on all cursors; everything else acts on the primary:

- self-insert (printable chars)
- Backspace and C-d (delete-char)
- RET (newline, raw -- no auto-indent staircase)
- C-y (yank: broadcast kill-ring head to every cursor; multi-line yanks
  split rows; no mark set; M-y is disabled under MC)
- Movement: C-f C-b C-n C-p C-a C-e (each cursor clamps to its own row)

Undo: every MC keystroke is wrapped in an explicit undo boundary pair,
so one C-_ undoes the whole stroke.

## Creating cursors

- `C-c m n`  mark next occurrence of word-at-point (or region text)
- `C-c m a`  mark all occurrences in the buffer
- `C-c m j`  add cursor on line below
- `C-c m k`  add cursor on line above
- `C-g`      drop all extra cursors

## Rendering

Secondary cursors are painted as cells with the HL_MATCH_CURRENT face
(vivid magenta, already used for the active isearch match) by overlaying
the row's hl[] before draw. The mode line shows `MC:n` when active.

## Explicitly out of scope

- Per-cursor regions and per-cursor kill rings
- Word ops (M-f/M-b/M-d), kill-word in parallel
- M-y, isearch, query-replace under MC (primary-only, forever)
- Click-to-add-cursor (mouse exists; maybe v2)

## Overlap with existing features

- Column editing (same column every line): use `C-x r t` string-rectangle.
- Replace every occurrence blindly: use `M-%` query-replace.
- MC fills the gap: scattered interactive same-text editing where you
  watch each edit as you make it.
