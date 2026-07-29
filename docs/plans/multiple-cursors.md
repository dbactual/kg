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
3. Apply the edit at each cursor, highest first. Descending order means
   an edit never shifts the positions of cursors not yet processed --
   no marker machinery needed. Newlines (shift rows down) and same-row
   insertions (shift cols right) both fall out of this for free.
4. Each cursor's new position = the natural end of its own edit.
5. Dedupe overlaps, extract the primary, redraw.

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
