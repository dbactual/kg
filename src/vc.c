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

#define VC_STATUS_NAME "*git-status*"
#define VC_DIFF_NAME   "*git-diff*"
#define VC_LOG_NAME    "*git-log*"
#define VC_SHOW_NAME   "*git-show*"
#define VC_DIR_NAME    "*vc-dir*"
#define VC_FILEDIFF_NAME "*vc-diff*"

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

/* Run `cmd` and copy its first output line (trimmed of the trailing
 * newline) into `out`, NUL-terminated.  On failure or empty output,
 * out[0] is set to '\0'.  Used by vc_dir_populate to gather the summary
 * fields (toplevel, branch, upstream, remote url). */
static void vc_git_first(const char *cmd, char *out, int outsize)
{
	char *buf;
	int n = 0, i;

	out[0] = '\0';
	buf = shell_run(cmd, NULL, 0, &n);
	if (!buf) return;
	for (i = 0; i < n && buf[i] != '\n' && i < outsize - 1; i++)
		out[i] = buf[i];
	out[i] = '\0';
	free(buf);
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

/* Map a porcelain v1 two-char status code to an Emacs VC-dir word. */
static const char *vc_status_word(char x, char y)
{
	if (x == '?' && y == '?') return "unregistered";
	if (x == 'A' || y == 'A') return "added";
	if (x == 'D' || y == 'D') return "removed";
	if (x == 'R' || y == 'R') return "renamed";
	if (x == 'C' || y == 'C') return "copied";
	if (x == 'U' || y == 'U' || x == 'A' || x == 'D') {
		if (x == 'U' || y == 'U') return "conflict";
	}
	return "edited";  /* M or anything else */
}

/* Path of the file whose per-file diff is built by vc_filediff_populate();
 * set by vc_dir_diff() before opening the buffer. */
static char vc_filediff_path[512];

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
	snprintf(vc_filediff_path, sizeof(vc_filediff_path), "%s", path);
	buf_open_special(VC_FILEDIFF_NAME, &diff_syntax_rec, vc_filediff_populate,
	                 "file diff — RET to jump to hunk, q to close.");
	vc_rehighlight();
}

/* Build the *vc-dir* buffer: a summary header followed by the changed
 * files grouped by directory, in the layout vcdir_syntax expects:
 *     "     <status-word><pad>path"      (file line, path at col 25)
 *     "<25 spaces><dir>/"               (directory header)            */
static void vc_dir_populate(void)
{
	char line[512], val[512];
	char workdir[512], branch[128], tracking[128], remote[128];
	char remote_name[128];
	int len, i, j;
	const char *home;

	/* ---- Summary ---- */
	vc_git_first("git rev-parse --show-toplevel", val, sizeof val);
	home = getenv("HOME");
	if (home && home[0] && strlen(val) >= strlen(home) &&
	    strncmp(val, home, strlen(home)) == 0 && val[strlen(home)] == '/') {
		snprintf(workdir, sizeof workdir, "~%s/", val + strlen(home));
	} else if (val[0]) {
		snprintf(workdir, sizeof workdir, "%s/", val);
	} else {
		snprintf(workdir, sizeof workdir, "(unknown)");
	}

	vc_git_first("git rev-parse --abbrev-ref HEAD", branch, sizeof branch);
	if (!branch[0] || strcmp(branch, "HEAD") == 0) {
		char sh[16];
		vc_git_first("git rev-parse --short HEAD", sh, sizeof sh);
		snprintf(branch, sizeof branch, "HEAD (detached at %s)", sh);
	}

	vc_git_first("git rev-parse --abbrev-ref @{upstream}", tracking, sizeof tracking);
	if (!tracking[0]) snprintf(tracking, sizeof tracking, "Not tracking");

	/* Remote url from the upstream's remote name (part before '/'). */
	remote[0] = '\0';
	{
		const char *slash = strchr(tracking, '/');
		if (slash && slash > tracking) {
			int rl = slash - tracking;
			char cmd[160];
			if (rl >= (int)sizeof(remote_name)) rl = (int)sizeof(remote_name) - 1;
			memcpy(remote_name, tracking, rl);
			remote_name[rl] = '\0';
			snprintf(cmd, sizeof cmd, "git remote get-url %s", remote_name);
			vc_git_first(cmd, remote, sizeof remote);
		}
	}
	if (!remote[0]) snprintf(remote, sizeof remote, "No remote");

	{
		char stashcmd[] = "git stash list | wc -l";
		char stashn[16];
		vc_git_first(stashcmd, stashn, sizeof stashn);
		/* wc -l may yield leading spaces; trim */
		{
			char *p = stashn;
			while (*p == ' ' || *p == '\t') p++;
			if (*p == '0' && p[1] == '\0')
				snprintf(val, sizeof val, "Nothing stashed");
			else
				snprintf(val, sizeof val, "%s stash entr%s",
				         p, atoi(p) == 1 ? "y" : "ies");
		}
	}

	len = snprintf(line, sizeof line, "VC backend : Git");
	editor_insert_row(editor.numrows, line, len);
	len = snprintf(line, sizeof line, "Working dir: %s", workdir);
	editor_insert_row(editor.numrows, line, len);
	len = snprintf(line, sizeof line, "Branch     : %s", branch);
	editor_insert_row(editor.numrows, line, len);
	len = snprintf(line, sizeof line, "Tracking   : %s", tracking);
	editor_insert_row(editor.numrows, line, len);
	len = snprintf(line, sizeof line, "Remote     : %s", remote);
	editor_insert_row(editor.numrows, line, len);
	len = snprintf(line, sizeof line, "Stash      : %s", val);
	editor_insert_row(editor.numrows, line, len);
	editor_insert_row(editor.numrows, "", 0);  /* blank separator */

	/* ---- Changed files grouped by directory ---- */
	{
		#define VC_MAX_FILES 256
		static char paths[VC_MAX_FILES][256];
		static char words[VC_MAX_FILES][16];
		int nfiles = 0;
		char *out;
		int out_len = 0, start;

		out = shell_run("git status --porcelain", NULL, 0, &out_len);
		if (!out) {
			editor_insert_row(editor.numrows, "(vc: git status failed)", 23);
			return;
		}

		start = 0;
		for (i = 0; i <= out_len && nfiles < VC_MAX_FILES; i++) {
			if (i == out_len || out[i] == '\n') {
				const char *s = out + start;
				int linelen = i - start;
				const char *p;
				int plen, arrow;
				start = i + 1;
				if (linelen < 3) continue;

				/* status word from XY (cols 0-1) */
				snprintf(words[nfiles], sizeof words[nfiles], "%s",
				         vc_status_word(s[0], s[1]));

				/* path begins at col 3; renames "old -> new" → new */
				p = s + 3;
				plen = linelen - 3;
				{
					const char *ar = NULL;
					for (arrow = 0; arrow + 4 <= plen; arrow++)
						if (p[arrow]==' ' && p[arrow+1]=='-' &&
						    p[arrow+2]=='>' && p[arrow+3]==' ') { ar = p + arrow + 4; break; }
					if (ar) { plen = (s + linelen) - ar; p = ar; }
				}
				if (plen <= 0) continue;
				/* strip surrounding quotes */
				if (plen >= 2 && p[0] == '"' && p[plen-1] == '"') { p++; plen -= 2; }
				if (plen <= 0) continue;
				if (plen >= (int)sizeof(paths[nfiles])) plen = (int)sizeof(paths[nfiles]) - 1;
				memcpy(paths[nfiles], p, plen);
				paths[nfiles][plen] = '\0';
				nfiles++;
			}
		}
		free(out);

		/* insertion sort by path so directories group together */
		for (i = 1; i < nfiles; i++) {
			for (j = i; j > 0 && strcmp(paths[j-1], paths[j]) > 0; j--) {
				char tmp[256];
				strcpy(tmp, paths[j]);   strcpy(paths[j], paths[j-1]); strcpy(paths[j-1], tmp);
				strcpy(tmp, words[j]);   strcpy(words[j], words[j-1]); strcpy(words[j-1], tmp);
			}
		}

		/* emit directory headers + file lines */
		{
			char curdir[256] = "";
			for (i = 0; i < nfiles; i++) {
				const char *slash = strrchr(paths[i], '/');
				char dir[256];
				int dl;
				if (slash) {
					dl = slash - paths[i];
					if (dl >= (int)sizeof(dir)) dl = (int)sizeof(dir) - 1;
					memcpy(dir, paths[i], dl);
					dir[dl] = '\0';
				} else {
					strcpy(dir, ".");
				}
				if (strcmp(dir, curdir) != 0) {
					strcpy(curdir, dir);
					len = snprintf(line, sizeof line, "%25s%s/", "", dir);
					editor_insert_row(editor.numrows, line, len);
				}
				len = snprintf(line, sizeof line, "     %-20s%s",
				               words[i], paths[i]);
				editor_insert_row(editor.numrows, line, len);
			}
			if (nfiles == 0) {
				len = snprintf(line, sizeof line, "(no changed files)");
				editor_insert_row(editor.numrows, line, len);
			}
		}
	}
}

void vc_open_dir(void)
{
	buf_open_special(VC_DIR_NAME, &vcdir_syntax, vc_dir_populate,
	                 "VC-dir — RET to open file, TAB to diff, q to close.");
	vc_rehighlight();
}

/* Extract the file path from a *vc-dir* file line at point into `out`
 * (NUL-terminated).  Returns 1 if the line is a file line, 0 otherwise
 * (summary line, directory header, or blank). */
static int vc_dir_path_at_point(char *out, int outsize)
{
	int filerow = editor.rowoff + editor.cy;
	const char *s;
	int len;
	const char *t;
	int tl;

	if (filerow < 0 || filerow >= editor.numrows) return 0;
	s = editor.row[filerow].chars;
	len = editor.row[filerow].size;
	if (len < 26) return 0;
	/* file line: cols 0-4 blank, col 5 non-blank */
	if (!(s[0]==' ' && s[1]==' ' && s[2]==' ' && s[3]==' ' && s[4]==' ' && s[5]!=' '))
		return 0;

	t = trim_field(s + 25, len - 25, &tl);
	if (tl <= 0) return 0;
	if (tl >= outsize) tl = outsize - 1;
	memcpy(out, t, tl);
	out[tl] = '\0';
	/* strip surrounding quotes (quoted paths) */
	if (tl >= 2 && out[0] == '"' && out[tl-1] == '"') {
		memmove(out, out + 1, tl - 2);
		out[tl - 2] = '\0';
	}
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
