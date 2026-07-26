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

#define VC_STATUS_NAME "*git-status*"
#define VC_DIFF_NAME   "*git-diff*"
#define VC_LOG_NAME    "*git-log*"
#define VC_SHOW_NAME   "*git-show*"

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
	int plen = 0;

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
		int hlen, i;

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
