# Plan: minimal vc-mode for kg

Goal: `git status` and `git diff` shown in special read-only buffers,
with `Enter` on a line jumping to the right file:line. Reuse existing
primitives (`shell_run`, `buf_open_special`, `editor_goto_line_direct`,
the buffer-list select pattern). No new subsystems.

## Commit 1 — Syntax highlighters for diff and git-status

Files: `src/def.h`, `src/syntax.c`

- `def.h`: add flags `SHL_IBUFFER (1<<4)`, `SHL_DIFF (1<<5)`,
  `SHL_GITSTATUS (1<<6)`.
- `syntax.c`: add `static void diff_syntax(erow *row)` and
  `static void gitstatus_syntax(erow *row)`; dispatch them from
  `editor_update_syntax` by flag, next to the MARKDOWN/MAKEFILE cases.
  - diff: `+`→green, `-`→red, `@@`→cyan, `diff `/`index `/`+++ `/`--- `→magenta.
  - gitstatus: `## `→cyan whole line; else color the 2-char XY code
    (`??`→red, `A`→green, `D`→red, `M`→yellow, `R`→magenta, else cyan).
- Set `ibuffer_syntax.flags = SHL_IBUFFER` in `bufmgr.c` so kbd.c can
  dispatch Enter by flag instead of by static pointer.

STOP and wait for validation.

## Commit 2 — bufmgr helpers for opening/finding buffers

Files: `src/bufmgr.c`, `src/def.h`

- Make `buf_open_special` non-static; declare it in `def.h`.
- Add `int buf_find_by_filename(const char *fn)` (returns slot or -1).
- Add `int buf_open_path(const char *path, int readonly)`:
  find existing → switch; else load into a free slot (mirrors
  `buf_open_file_ro`'s tail). Returns slot or -1. Refactor
  `buf_open_file_ro` to call `buf_open_path` after reading the prompt.

STOP and wait for validation.

## Commit 3 — vc.c: status/diff buffers, Enter-to-jump, bindings

Files: `src/vc.c` (new), `src/Makefile` (add vc.o to SRCS),
`src/def.h`, `src/kbd.c`, `src/cmd.c`, `src/help.c`

- `vc.c`:
  - static `diff_syntax` and `gitstatus_syntax` records (flags set).
  - `vc_status_populate()`: `shell_run("git status --porcelain=v1 -b")`,
    insert rows.
  - `vc_diff_populate()`: `shell_run("git diff")`, insert rows.
  - `vc_open_status()` / `vc_open_diff()`: call `buf_open_special`
    with the right syntax + populate, then re-highlight every row
    (rows were inserted while syntax was NULL) and leave cursor at top.
  - `vc_status_select()`: parse `XY path` at point; for renames take
    the ` -> ` right side; strip quotes; `buf_open_path(path,0)` then
    `editor_goto_line_direct(1,1)`.
  - `vc_diff_select()`: scan up for `diff --git a/P b/P` (take b/ side)
    and the nearest `@@ -a,b +c,d @@` above point; count `+`/` ` lines
    from the hunk header to point → target = c + count;
    `buf_open_path(path,0)` then `editor_goto_line_direct(target,1)`.
- `kbd.c`: in the read-only Enter branch, dispatch by
  `editor.syntax->flags`: `SHL_IBUFFER`→`buf_ibuffer_select`,
  `SHL_GITSTATUS`→`vc_status_select`, `SHL_DIFF`→`vc_diff_select`.
  Add a `C-x v` sub-prefix: `d`→`vc_open_diff`, `s`→`vc_open_status`.
- `cmd.c`: add `vc-diff` and `vc-status` to the M-x table (CMD_NONE).
- `help.c`: add `C-x v d/s  vc diff/status` to the help table.

Build with `make`. Manually test in a git repo: `C-x v s` shows status,
`Enter` opens the file; `C-x v d` shows diff, `Enter` jumps to the
hunk's line in the file.

STOP and wait for validation.
