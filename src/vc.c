/* ============================ vc-mode ================================
 *
 * Minimal version-control viewer for git repositories.
 *
 *   C-x v s / M-x vc-status  → *git-status* buffer (git status --porcelain)
 *   C-x v d / M-x vc-diff     → *git-diff*   buffer (git diff)
 *
 * Both buffers are read-only special buffers built with buf_open_special,
 * the same machinery as *Buffer List*.  Pressing Enter on a line jumps to
 * the referenced file:line in a real buffer:
 *
 *   *git-status*  "XY path"           → open path at line 1
 *   *git-diff*    inside a hunk       → open the hunk's file at the
 *                                       corresponding new-line number
 *
 * Output is captured via shell_run() (the same helper M-! uses), so no
 * tty is shared with git — it must be non-interactive, which `git status`
 * and `git diff` are.
 */

#include "def.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static struct editor_syntax gitstatus_syntax = {
	"GitStatus", NULL, NULL, "", "", "", SHL_GITSTATUS
};
static struct editor_syntax diff_syntax_rec = {
	"Diff", NULL, NULL, "", "", "", SHL_DIFF
};
static struct editor_syntax gitlog_syntax = {
	"GitLog", NULL, NULL, "", "", "", SHL_GITLOG
};
static struct editor_syntax vcdir_syntax = {
	"VC-dir", NULL, NULL, "", "", "", SHL_VCDIR
};
static struct editor_syntax grep_syntax_rec = {
	"Grep", NULL, NULL, "", "", "", SHL_GREP
};

#define VC_STATUS_NAME "*git-status*"
#define VC_DIFF_NAME   "*git-diff*"
#define VC_LOG_NAME    "*git-log*"
#define VC_SHOW_NAME   "*git-show*"
#define VC_DIR_NAME    "*vc-dir*"
#define VC_FILEDIFF_NAME "*vc-diff*"
#define GREP_NAME      "*grep*"

/* Forward decl: defined with the Enter handlers below, but vc_dir_select /
 * vc_dir_diff (built earlier) need it. */
static const char *trim_field(const char *s, int len, int *outlen);

/* Run `cmd` and insert its stdout into the current (just-reset) buffer one
 * row per line.  On failure, leave a single error row. */
static void vc_insert_command_output(const char *cmd)
{
	char *out;
	int out_len = 0, start, i;

	out = shell_run(cmd, NULL, 0, &out_len);
	if (!out) {
		editor_insert_row(editor.numrows, "(vc: command failed)", 20);
		return;
	}

	start = 0;
	for (i = 0; i <= out_len; i++) {
		if (i == out_len || out[i] == '\n') {
			editor_insert_row(editor.numrows, out + start, i - start);
			start = i + 1;
		}
	}
	free(out);
}

static void vc_status_populate(void)
{
	vc_insert_command_output("git status --porcelain=v1 -b");
}

static void vc_diff_populate(void)
{
	vc_insert_command_output("git diff");
}

/* buf_open_special attaches the syntax record only after populate() has
 * inserted the rows, so the rows were created with editor.syntax == NULL
 * and have no per-character highlight.  Walk them again so the diff/status
 * colouring takes effect. */
static void vc_rehighlight(void)
{
	int i;
	for (i = 0; i < editor.numrows; i++)
		editor_update_row(&editor.row[i]);
}

void vc_open_status(void)
{
	buf_open_special(VC_STATUS_NAME, &gitstatus_syntax, vc_status_populate,
	                 "git status — RET to open file, q to close.");
	vc_rehighlight();
}

void vc_open_diff(void)
{
	buf_open_special(VC_DIFF_NAME, &diff_syntax_rec, vc_diff_populate,
	                 "git diff — RET to jump to hunk, q to close.");
	vc_rehighlight();
}

static void vc_log_populate(void)
{
	vc_insert_command_output("git log");
}

void vc_open_log(void)
{
	buf_open_special(VC_LOG_NAME, &gitlog_syntax, vc_log_populate,
	                 "git log — RET to show commit, q to close.");
	vc_rehighlight();
}

/* The commit hash being shown by vc_show_populate(); set by
 * vc_log_select() before it triggers buf_open_special(). */
static char vc_show_hash[64];

static void vc_show_populate(void)
{
	char cmd[96];

	snprintf(cmd, sizeof(cmd), "git show %s", vc_show_hash);
	vc_insert_command_output(cmd);
}

/* Open `git show <hash>` in a *git-show* buffer with diff highlighting.
 * Used by vc_log_select(). */
static void vc_open_show(const char *hash)
{
	snprintf(vc_show_hash, sizeof(vc_show_hash), "%s", hash);
	buf_open_special(VC_SHOW_NAME, &diff_syntax_rec, vc_show_populate,
	                 "git show — RET to jump to hunk, q to close.");
	vc_rehighlight();
}

/* ---- VC-dir (C-x v v) -------------------------------------------------- */

/* Path of the file whose per-file diff is built by vc_filediff_populate();
 * set by vc_dir_diff() before opening the buffer. */
static char vc_filediff_path[512];

/* Filename of the buffer that was active when *vc-diff* was opened, so
 * TAB/q in *vc-diff* can return to it (likely *vc-dir*, but not necessarily
 * — the diff could be opened another way in the future).  Empty if no
 * prior buffer was recorded. */
static char vc_filediff_prev[256];

static void vc_filediff_populate(void)
{
	char cmd[600];
	char *out;
	int out_len = 0, start, i;

	snprintf(cmd, sizeof(cmd), "git diff HEAD -- %s", vc_filediff_path);
	out = shell_run(cmd, NULL, 0, &out_len);

	/* Untracked files have no diff vs HEAD; show the whole file as
	 * added via --no-index so the buffer isn't empty and Enter-to-jump
	 * still works (the output carries a real "diff --git a/P b/P"
	 * header). */
	if (out && out_len == 0) {
		free(out);
		out = NULL;
		snprintf(cmd, sizeof(cmd), "git diff --no-index /dev/null %s",
		         vc_filediff_path);
		out = shell_run(cmd, NULL, 0, &out_len);
	}

	if (!out) {
		editor_insert_row(editor.numrows, "(vc: command failed)", 20);
		return;
	}
	start = 0;
	for (i = 0; i <= out_len; i++) {
		if (i == out_len || out[i] == '\n') {
			editor_insert_row(editor.numrows, out + start, i - start);
			start = i + 1;
		}
	}
	free(out);
}

/* Open a per-file diff for `path` in a *vc-diff* buffer (diff-highlighted,
 * Enter jumps to the hunk's file:line via vc_diff_select). */
static void vc_open_filediff(const char *path)
{
	/* Remember the buffer we're leaving so TAB/q can return to it. */
	if (editor.filename)
		snprintf(vc_filediff_prev, sizeof vc_filediff_prev, "%s", editor.filename);
	else
		vc_filediff_prev[0] = '\0';

	snprintf(vc_filediff_path, sizeof(vc_filediff_path), "%s", path);
	buf_open_special(VC_FILEDIFF_NAME, &diff_syntax_rec, vc_filediff_populate,
	                 "file diff — RET to jump to hunk, TAB/q to close.");
	vc_rehighlight();
}

/* Build the *vc-dir* buffer: a summary header followed by the changed
 * files grouped by directory, in the layout vcdir_syntax expects:
 *     "     <status-word><pad>path"      (file line, path at col 25)
 *     "<25 spaces><dir>/"               (directory header)            */
/* *vc-dir* shows `git status --porcelain=v1 -b` verbatim: the branch
 * header ("## branch...upstream [ahead N]") on the first line, then the
 * changed files as "XY path".  Enter opens a file line, TAB opens a
 * per-file diff; both parse the path out of the porcelain line (see
 * vc_dir_path_at_point).  The branch header is left as a non-file line,
 * so Enter/Tab on it do nothing. */
static void vc_dir_populate(void)
{
	vc_insert_command_output("git status --porcelain=v1 -b");
}

void vc_open_dir(void)
{
	buf_open_special(VC_DIR_NAME, &vcdir_syntax, vc_dir_populate,
	                 "VC-dir — RET to open file, TAB to diff, q to close.");
	vc_rehighlight();
}

/* Extract the file path from a *vc-dir* line at point into `out`
 * (NUL-terminated).  A *vc-dir* line is `git status --short` output:
 *   "XY path"          (XY = 2-char porcelain status, path at col 3)
 *   "XY old -> new"    (rename — take the new side)
 *   "?? path"          (untracked)
 * Returns 1 if the line is a file line, 0 otherwise (blank or too short). */
static int vc_dir_path_at_point(char *out, int outsize)
{
	int filerow = editor.rowoff + editor.cy;
	const char *s, *p;
	int len, plen;
	const char *arrow;

	if (filerow < 0 || filerow >= editor.numrows) return 0;
	s = editor.row[filerow].chars;
	len = editor.row[filerow].size;
	if (len < 4) return 0;            /* need "XY " + at least one path char */
	/* Branch header "## ..." is not a file line. */
	if (s[0] == '#' && s[1] == '#') return 0;

	p = s + 3;                        /* skip "XY " */
	plen = len - 3;

	/* Rename: "old -> new" — take the right-hand side. */
	arrow = NULL;
	{
		int i;
		for (i = 0; i + 4 <= plen; i++)
			if (p[i]==' ' && p[i+1]=='-' && p[i+2]=='>' && p[i+3]==' ') {
				arrow = p + i + 4; break;
			}
	}
	if (arrow) {
		p = arrow;
		plen = (s + len) - arrow;
	}
	if (plen <= 0) return 0;

	/* Strip surrounding quotes (git quotes paths with special chars). */
	if (plen >= 2 && p[0] == '"' && p[plen-1] == '"') { p++; plen -= 2; }
	if (plen <= 0) return 0;

	if (plen >= outsize) plen = outsize - 1;
	memcpy(out, p, plen);
	out[plen] = '\0';
	if (!out[0]) return 0;
	return 1;
}

void vc_dir_select(void)
{
	char path[256];

	if (editor.syntax != &vcdir_syntax) return;
	if (!vc_dir_path_at_point(path, sizeof path)) return;
	{
		int slot = buf_open_path(path, 0);
		if (slot < 0) return;
		editor_goto_line_direct(1, 1);
		editor_set_status_message("%s", editor.filename ? editor.filename : "[new]");
	}
}

void vc_dir_diff(void)
{
	char path[256];

	if (editor.syntax != &vcdir_syntax) return;
	if (!vc_dir_path_at_point(path, sizeof path)) return;
	vc_open_filediff(path);
	editor_set_status_message("git diff HEAD -- %s", path);
}

/* Close the per-file *vc-diff* buffer and return to the buffer that was
 * active when the diff was opened (usually *vc-dir*).  Bound to both TAB
 * and q in *vc-diff*.  Kills the diff buffer and restores the recorded
 * prior buffer if it is still open; otherwise buf_kill's nearest-buffer
 * fallback takes us somewhere sane. */
void vc_filediff_close(int fd)
{
	char prev[256];
	int slot;

	if (editor.syntax != &diff_syntax_rec) return;
	if (!editor.filename || strcmp(editor.filename, VC_FILEDIFF_NAME) != 0) return;

	/* Snapshot the prior buffer before buf_kill can touch buflist. */
	snprintf(prev, sizeof prev, "%s", vc_filediff_prev);
	vc_filediff_prev[0] = '\0';

	slot = prev[0] ? buf_find_by_filename(prev) : -1;
	buf_kill(fd);
	if (slot >= 0 && buflist[slot].active) {
		buf_open_path(prev, 1);
		/* Restore vc-dir's status hint, or echo the filename we returned to. */
		if (strcmp(prev, VC_DIR_NAME) == 0)
			editor_set_status_message("VC-dir — RET to open file, TAB to diff, q to close.");
		else
			editor_set_status_message("%s", editor.filename ? editor.filename : "[new]");
	}
}

/* ---- Grep (M-x grep) --------------------------------------------------- */

/* The grep command line to run, captured by grep_open() before opening
 * the buffer (buf_open_special's populate callback takes no args). */
static char grep_cmd[512];

static void grep_populate(void)
{
	vc_insert_command_output(grep_cmd);
}

/* Prompt for a search pattern, run `grep -rnH -- <pattern> .` from the
 * current directory, and show the matches in a read-only *grep* buffer.
 * Enter on a "file:line:text" line opens that file at that line
 * (grep_select).  For a custom grep invocation use M-! instead. */
void grep_open(int fd)
{
	char pattern[256];
	char *p;

	/* Pre-fill with the word at point so the common case — grep the
	 * identifier under the cursor — needs just an Enter. */
	if (!editor_word_at_point(pattern, sizeof pattern))
		pattern[0] = '\0';
	if (editor_read_line(fd, "grep pattern: ", pattern, sizeof pattern) < 0 ||
	    !pattern[0])
		return;

	/* Strip a leading '--' so the user can't accidentally inject grep
	 * options through the pattern; everything else is passed verbatim,
	 * so regex metacharacters work. */
	p = pattern;
	while (p[0] == '-' && p[1] == '-') p += 2;
	if (!p[0]) return;

	snprintf(grep_cmd, sizeof grep_cmd, "grep -rnH -- '%s' .", p);
	buf_open_special(GREP_NAME, &grep_syntax_rec, grep_populate,
	                 "grep — RET to open match, q to close.");
	vc_rehighlight();
}

/* Parse the "file:line:text" line at point and jump to that file:line.
 * Validates that the bytes between the first and second colons are all
 * digits, so grep's own header lines and "Binary file ... matches" are
 * ignored. */
void grep_select(void)
{
	int filerow = editor.rowoff + editor.cy;
	const char *s;
	int len, i, colon1, colon2, linenum;
	char path[512];
	int plen;

	if (editor.syntax != &grep_syntax_rec) return;
	if (filerow < 0 || filerow >= editor.numrows) return;
	s = editor.row[filerow].chars;
	len = editor.row[filerow].size;
	if (len <= 0) return;

	colon1 = -1;
	for (i = 0; i < len; i++) if (s[i] == ':') { colon1 = i; break; }
	if (colon1 <= 0) return;
	colon2 = -1;
	for (i = colon1 + 1; i < len; i++) if (s[i] == ':') { colon2 = i; break; }
	if (colon2 < 0) return;
	linenum = 0;
	for (i = colon1 + 1; i < colon2; i++) {
		if (!isdigit((unsigned char)s[i])) return;
		linenum = linenum * 10 + (s[i] - '0');
	}
	if (linenum < 1) return;

	plen = colon1;
	if (plen >= (int)sizeof path) plen = (int)sizeof path - 1;
	memcpy(path, s, plen);
	path[plen] = '\0';

	{
		int slot = buf_open_path(path, 0);
		if (slot < 0) return;
		editor_goto_line_direct(linenum, 1);
		editor_set_status_message("%s:%d", editor.filename ? editor.filename : "[new]", linenum);
	}
}

/* ---- Enter handlers ---------------------------------------------------- */

/* Trim leading/trailing whitespace from [s,s+len) into a static buffer.
 * Returns the trimmed pointer and writes the trimmed length to *outlen. */
static const char *trim_field(const char *s, int len, int *outlen)
{
	static char buf[1024];
	int i;

	while (len > 0 && isspace((unsigned char)s[0])) { s++; len--; }
	while (len > 0 && isspace((unsigned char)s[len-1])) len--;
	if (len <= 0) { *outlen = 0; return ""; }
	if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
	for (i = 0; i < len; i++) buf[i] = s[i];
	buf[len] = '\0';
	*outlen = len;
	return buf;
}

void vc_status_select(void)
{
	int filerow = editor.rowoff + editor.cy;
	const char *line;
	int len, pathoff, pathlen;
	const char *path;

	if (editor.syntax != &gitstatus_syntax) return;
	if (filerow < 0 || filerow >= editor.numrows) return;

	line = editor.row[filerow].chars;
	len  = editor.row[filerow].size;

	/* Branch header "## ..." — nothing to open. */
	if (len >= 3 && strncmp(line, "## ", 3) == 0) return;
	/* Porcelain v1: "XY path" with a fixed 2-char status code, then a
	 * space, then the path.  For renames "R" the path is "old -> new";
	 * take the right-hand side. */
	if (len < 3) return;
	pathoff = 3;  /* skip "XY " */
	if (pathoff >= len) return;

	path = trim_field(line + pathoff, len - pathoff, &pathlen);
	if (pathlen == 0) return;

	/* Rename: "oldname -> newname" — open newname. */
	{
		const char *arrow = strstr(path, " -> ");
		if (arrow) {
			path = trim_field(arrow + 4, pathlen - (arrow + 4 - path), &pathlen);
			if (pathlen == 0) return;
		}
	}

	/* Strip surrounding quotes (git quotes paths with special chars). */
	if (pathlen >= 2 && path[0] == '"' && path[pathlen-1] == '"') {
		path++;
		pathlen -= 2;
	}

	{
		char pathbuf[1024];
		int slot;
		if (pathlen >= (int)sizeof(pathbuf)) pathlen = (int)sizeof(pathbuf) - 1;
		memcpy(pathbuf, path, pathlen);
		pathbuf[pathlen] = '\0';
		slot = buf_open_path(pathbuf, 0);
		if (slot < 0) return;
		editor_goto_line_direct(1, 1);
		editor_set_status_message("%s", editor.filename ? editor.filename : "[new]");
	}
}

void vc_diff_select(void)
{
	int filerow = editor.rowoff + editor.cy;
	int i, row;
	const char *path = NULL;
	int pathlen = 0;
	int new_line = 0;      /* line in the new file to jump to */
	int saw_hunk = 0;
	int plus_count = 0;    /* '+' lines seen since the hunk header */
	char pathbuf[1024];

	if (editor.syntax != &diff_syntax_rec) return;
	if (filerow < 0 || filerow >= editor.numrows) return;

	/* Scan upward: find the nearest "diff --git a/P b/P" (file) and the
	 * nearest "@@ -a,b +c,d @@" (hunk) at or above point.  The jump
	 * target is c + (number of "+" or context lines between the hunk
	 * header and point).  git diff counts a hunk's new-side lines as:
	 *   the header line, then each line beginning with '+' or ' '
	 *   advances the new-line counter by 1; '-' lines don't. */
	for (row = filerow; row >= 0; row--) {
		const char *s = editor.row[row].chars;
		int slen = editor.row[row].size;

		if (!saw_hunk && slen >= 2 && s[0] == '@' && s[1] == '@') {
			const char *plus = strstr(s, "+");
			if (plus) {
				new_line = atoi(plus + 1);  /* c in "+c,d" */
				saw_hunk = 1;
				/* Lines from row+1 .. filerow contribute to new_line.
				 * plus_count is accumulated below once we know we're
				 * past the header; do it now in order. */
				for (i = row + 1; i <= filerow; i++) {
					const char *t = editor.row[i].chars;
					int tlen = editor.row[i].size;
					if (tlen > 0 && (t[0] == '+' || t[0] == ' '))
						plus_count++;
				}
				break;
			}
		}
	}

	if (!saw_hunk) {
		editor_set_status_message("Not inside a diff hunk");
		return;
	}

	/* Find the file: scan upward for "diff --git a/P b/P". */
	for (row = filerow; row >= 0; row--) {
		const char *s = editor.row[row].chars;
		int slen = editor.row[row].size;
		const char *b;

		if (slen < 11 || strncmp(s, "diff --git ", 11) != 0) continue;
		/* "diff --git a/PATH b/PATH" — take the b/ side. */
		b = strstr(s, " b/");
		if (!b) continue;
		path = b + 3;
		pathlen = slen - (b + 3 - s);
		/* There may be trailing " " content for some diff formats; git's
		 * standard form ends the line at the path.  Trim whitespace. */
		{
			const char *p = trim_field(path, pathlen, &pathlen);
			if (pathlen <= 0) continue;
			memcpy(pathbuf, p, pathlen);
			pathbuf[pathlen] = '\0';
		}
		break;
	}

	if (pathlen <= 0) {
		editor_set_status_message("No file header above this hunk");
		return;
	}

	new_line += plus_count;
	if (new_line < 1) new_line = 1;

	{
		int slot = buf_open_path(pathbuf, 0);
		if (slot < 0) return;
		editor_goto_line_direct(new_line, 1);
		editor_set_status_message("%s:%d", editor.filename ? editor.filename : "[new]", new_line);
	}
}

void vc_log_select(void)
{
	int filerow = editor.rowoff + editor.cy;
	int row;
	char hash[64];

	if (editor.syntax != &gitlog_syntax) return;
	if (filerow < 0 || filerow >= editor.numrows) return;

	/* Scan upward for the nearest "commit <hex>" header. */
	for (row = filerow; row >= 0; row--) {
		const char *s = editor.row[row].chars;
		int slen = editor.row[row].size;
		int hlen;

		if (slen < 8 || strncmp(s, "commit ", 7) != 0) continue;
		/* Copy the hex token (up to 40 chars) after "commit ". */
		s += 7; slen -= 7;
		hlen = 0;
		while (hlen < slen && hlen < 40 && isxdigit((unsigned char)s[hlen]))
			hlen++;
		if (hlen < 7) break; /* not a real hash line */
		if (hlen >= (int)sizeof(hash)) hlen = (int)sizeof(hash) - 1;
		memcpy(hash, s, hlen);
		hash[hlen] = '\0';
		vc_open_show(hash);
		editor_set_status_message("git show %s", hash);
		return;
	}
	editor_set_status_message("No commit above point");
}
