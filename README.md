[![2][]][1] [![4][]][3] [![6][]][5] <a href="https://www.flaticon.com/free-icons/kg" title="Kg icons created by alekseyvanin - Flaticon"><img src="doc/kg.png" width=100 align="right"></a>

# Light Weight UTF-8 Terminal Text Editor

A spiritual descendant of [mg][mg] (Micro Emacs) — same Emacs heritage,
about half the lines of code.  See mg's README for the wider
Emacs-in-a-terminal family kg comes from.

kg is a small, fast terminal text editor with pure Emacs keybindings.
Suitable for editing system files or quick fixes on remote systems where
a full GUI editor is not available.

With syntax highlighting for many languages, multiple buffers, split
windows, incremental search, and multi-level undo, kg punches above its
weight while staying dependency-free — no curses, just standard VT100
escape sequences.

## Changes in this fork

The `updates` branch diverges from `origin/master` (upstream
[troglobit/kg][mg]) with the following improvements, grouped by area.
Each entry lists the key bindings or commands affected.

### Syntax highlighting
- **24-bit true color** with Emacs-accurate font-lock hues, light/dark
  palettes, and **OSC 11 background auto-detection** (also honors
  `KG_BG`/`COLORFGBG).  Replaced the 256-color cube, which was too
  garish for Emacs colours.
- **Go** (`.go`) language support.
- **`.env` files** are highlighted as shell scripts.
- `git status`/`diff` syntax highlighters for `vc-mode` buffers.

### Incremental search
- **Every** match on screen is highlighted, not just the current one.
  The current match is bold magenta; other matches get a dim teal
  background so the active one stands out.
- **`C-s`/`C-r` with an empty prompt recalls the last search term**
  and searches immediately, like GNU Emacs.
- **Arrow keys end the search** and move point from the match
  (previously they repeated the search forward/backward).
- **`C-g`, `ESC`, and `Enter`** all record the query as the prior
  search term for later recall.
- Fixed isearch cursor position on tab-indented lines.
- Fixed the match highlight SGR being truncated (lost the first
  character of the match) and background bleeding past the match.

### Tab completion
- **`TAB` is context-sensitive**, like Emacs:
  - active region → **indent-rigidly** by 4 spaces (region stays
    selected for further `TAB`s);
  - minibuffer prompt → pass `TAB` to the prompt;
  - word prefix before point → **`dabbrev-expand`**: scan the buffer
    for the nearest word starting with that prefix and replace the
    prefix; repeated `TAB` cycles through candidates;
  - otherwise → literal tab.
- Any non-`TAB` key resets the dabbrev cycle.

### Project & xref
- **`C-x p` prefix** (replaces `C-x v`) for project operations:
  - `C-x p s` git status, `C-x p d` git diff, `C-x p l` git log,
    `C-x p v` vc-dir (status + recent commit graph);
  - `C-x p g` project grep (from project root);
  - `C-x p f` fuzzy file finder.
- **`M-.`** finds definitions by regex across files **on disk** (not
  open buffers), with per-language patterns; rejects indented
  call-site false positives (column-0 + keyword check).  **`M-,`**
  pops the mark ring to jump back.
- **`M-x grep`** with `file:line` jump; uses **ripgrep** when
  available; pre-fills the pattern with the word at point.
- Project root detection walks up from the current file's directory
  for `.git`/`Makefile`/`package.json`/`Cargo.toml`/`.hg`/`.svn`/
  `TAGS`.  Backup/cruft files (`*.~`, `.#*`, `*.swp`, `.orig`,
  `.rej`, `.bak`, `.merge`, `core`) are excluded from file scans.

### vc-mode (git)
- `C-x p s`/`d`/`l`/`v` open `*git-status*`, `*vc-diff*`, `*git-log*`,
  and `*vc-dir*` buffers with **Enter-to-jump** to the file/commit.
- `TAB` in `*git-log*` opens `git show` for the commit; `TAB`/`q` in
  `*git-show*` returns to the log.  `q` in `*vc-diff*` closes it and
  returns to the prior buffer.  `g` in `*vc-dir*` refreshes.
- `*vc-dir*` shows `git status --porcelain` plus the recent commit
  log graph.

### Mouse support
- SGR mouse reporting with **click-to-position** and **1-line wheel
  scroll**.  Fixed click-after-scroll using the live row offset
  (previously the first click after a scroll jumped back to the top).

### Editor improvements
- **`C-c g`** prompts for a line number and jumps there (goto-line),
  matching `M-g`/`M-x goto-line`.
- **Buffer list** (`C-x C-b`) pre-selects the current buffer and
  wraps the cursor top↔bottom across file rows.
- **`M-arrow`** keys → word/paragraph motion (was window switching).
- **`M-q`** (fill-paragraph) restores the cursor to the same position
  in the reflowed text.
- **`M-w`/`C-w`** copy the region to the **system clipboard**
  (`pbcopy`/`xsel`/`wl-copy`) in addition to the kill ring.
- **Mode line** shows the full file path with `~` for `$HOME`.
- **Splash screen** on startup when no file is given.

## Features

<a href="doc/screenshot.png"><img align="right" src="doc/screenshot.png" width=360 title="kg in action"></a>

- Pure Emacs-style keybindings
- Syntax highlighting for many programming languages, including
  hex/binary/octal integer literals
- Multiple buffers with a shared, multi-entry kill ring (M-y
  yank-pop cycles through prior kills after C-y, like Emacs)
- Split-window support
- Visual mark mode: the region renders in reverse video as you move
- Shift-select and the CUA clipboard trio (Shift-Delete / Ctrl-Insert
  / Shift-Insert) alongside the Emacs C-w / M-w / C-y
- Rectangle commands (C-x SPC, C-x r {k,y,d,c,t})
- Multiple cursors: parallel editing at several positions (C-c m {n,a}
marks next/all occurrences of the word at point, C-c m {j,k} adds a
cursor on the line below/above, C-g clears).  Typing, Backspace, RET,
movement, and C-y yank apply at every cursor; one C-_ undoes the whole
stroke.  See docs/plans/multiple-cursors.md.
- Incremental search and query-replace (M-%)
- Multi-level undo (C-_)
- Paragraph reflow to 72 columns (M-q)
- Keyboard macros (C-x ( / C-x ) / C-x e)
- M-x, C-x C-f, and C-x b all share an ido-style picker: substring
  matching, already-open files pushed to the back of the file picker
- Detects external changes to open files; auto-revert is on by default
  (M-x global-auto-revert-mode toggles; modified buffers are never
  clobbered — they show (changed) in the mode line instead)
- Shell commands (M-!) and pipe-region-through-command (M-|)
- Comment-dwim (M-;)
- Word-case bindings (M-u / M-l / M-c)
- Open line (C-o) and join-line (M-^)
- Quoted-insert (C-q) for literal Tab/Esc/control bytes
- Universal-argument (C-u) for repeated commands
- Auto-indent and bracket autocomplete
- Suspend to background (C-z)
- Built-in help in a scrollable *help* buffer (C-h)
- No dependencies (not even curses)
- Uses standard VT100 escape sequences
- Graceful terminal resize handling

## Usage

```
kg [-RVh] [file ...]
```

| Option | Description                  |
|--------|------------------------------|
| `-R`   | Open file(s) read-only       |
| `-V`   | Print version and exit       |
| `-h`   | Print this help and exit     |

Multiple files can be opened at once, each in its own buffer.  See the
[man page][7] for more in-depth information as well as the full key
binding reference.

## Building and Installing

```bash
make
sudo make install          # installs to /usr/local/bin and /usr/local/share/man/man1
```

Override the prefix or use DESTDIR for staged installs:

```bash
make install prefix=/usr
make install DESTDIR=/tmp/pkg
```

To uninstall:

```bash
sudo make uninstall
```

## Origin & References

kg is based on [kilo][0] by Salvatore Sanfilippo (antirez), the original
minimal text editor that demonstrates how to build a functional editor
without dependencies in about 1000 lines of C code.

The name "kg" is a nod to [mg][mg] (Micro Emacs), suggesting "kilo-gram"
— a minimal implementation with Emacs keybindings.  mg's README is the
place to read up on the broader lineage if the heritage matters to you.

[0]:  https://github.com/antirez/kilo
[mg]: https://github.com/troglobit/mg
[1]: https://en.wikipedia.org/wiki/BSD_licenses
[2]: https://img.shields.io/badge/License-BSD%202--Clause-green.svg
[3]: https://github.com/troglobit/kg/actions/workflows/build.yml/
[4]: https://github.com/troglobit/kg/actions/workflows/build.yml/badge.svg
[5]: https://github.com/troglobit/kg/releases
[6]: https://img.shields.io/github/v/release/troglobit/kg?include_prereleases
[7]: https://man.troglobit.com/man1/kg.1.html
