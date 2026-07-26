/* ============================ project commands ===========================
 *
 * C-x p prefix — operations scoped to the whole project (detected root),
 * replacing the old C-x v vc-mode prefix.  The git view commands (status,
 * diff, log) also live here now, alongside:
 *
 *   C-x p v  vc-dir               (project VC overview)
 *   C-x p s  git status
 *   C-x p d  git diff
 *   C-x p l  git log
 *   C-x p g  project grep          (recursive from the project root)
 *   C-x p f  project find-file     (fuzzy-match any file in the project)
 *
 * The project root is found by editor_find_project_root() (shared with
 * xref): walk up from the current file's directory for .git / Makefile /
 * package.json / Cargo.toml / .hg / .svn / TAGS.
 */

#include "def.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---- project grep ---------------------------------------------------- *
 *
 * Reuses the *grep* buffer machinery from vc.c (grep_populate reads
 * grep_cmd), but runs grep from the project root so it covers the whole
 * tree, and pre-fills the pattern with the word at point. */

extern char grep_cmd[];            /* defined in vc.c, read by grep_populate */
extern void grep_populate(void);   /* defined in vc.c */
extern struct editor_syntax grep_syntax_rec;  /* defined in vc.c */

/* Run `grep -rnH -- <pattern> <root>` and show matches in *grep*. */
void project_grep(int fd)
{
	char pattern[256];
	char root[1024];
	char *p;

	if (!editor_word_at_point(pattern, sizeof pattern))
		pattern[0] = '\0';
	if (editor_read_line(fd, "project grep: ", pattern, sizeof pattern) < 0 ||
	    !pattern[0])
		return;

	/* Strip a leading '--' so the user can't inject grep options. */
	p = pattern;
	while (p[0] == '-' && p[1] == '-') p += 2;
	if (!p[0]) return;

	if (!editor_find_project_root(root, sizeof root))
		snprintf(root, sizeof root, ".");

	snprintf(grep_cmd, 512, "grep -rnH -- '%s' %s", p, root);
	buf_open_special(GREP_NAME, &grep_syntax_rec, grep_populate,
	                 "project grep — RET to open match, q to close.");
	/* re-highlight rows inserted while syntax was NULL */
	{
		int i;
		for (i = 0; i < editor.numrows; i++)
			editor_update_row(&editor.row[i]);
	}
}

/* ---- project find-file ----------------------------------------------- *
 *
 * Walk the project root, collect every regular file (skipping .git /
 * node_modules / build / dist / target / hidden dirs), and present a
 * fuzzy-matching picker in the echo area.  Enter opens the selected file. */

#define PROJ_MAX_FILES 4096
static char proj_files[PROJ_MAX_FILES][256];
static int  proj_nfiles;

static void proj_walk(const char *dir)
{
	DIR *dp;
	struct dirent *e;
	char path[1024];

	if (proj_nfiles >= PROJ_MAX_FILES) return;
	dp = opendir(dir);
	if (!dp) return;
	while ((e = readdir(dp)) != NULL && proj_nfiles < PROJ_MAX_FILES) {
		struct stat st;
		if (e->d_name[0] == '.') continue;  /* hidden + . and .. */
		snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
		if (stat(path, &st) != 0) continue;
		if (S_ISDIR(st.st_mode)) {
			if (!strcmp(e->d_name, "node_modules") ||
			    !strcmp(e->d_name, "build") ||
			    !strcmp(e->d_name, "dist") ||
			    !strcmp(e->d_name, "target") ||
			    !strcmp(e->d_name, ".git"))
				continue;
			proj_walk(path);
		} else if (S_ISREG(st.st_mode)) {
			/* Store the path relative to the project root so the picker
			 * shows "src/kbd.c" rather than an absolute path. */
			snprintf(proj_files[proj_nfiles], sizeof proj_files[0],
			         "%s", path);
			proj_nfiles++;
		}
	}
	closedir(dp);
}

/* Strip a leading "<root>/" from each stored path so the list shows
 * project-relative names.  rootlen is strlen(root). */
static void proj_make_relative(const char *root, int rootlen)
{
	int i;
	for (i = 0; i < proj_nfiles; i++) {
		if (strncmp(proj_files[i], root, rootlen) == 0 &&
		    proj_files[i][rootlen] == '/') {
			memmove(proj_files[i], proj_files[i] + rootlen + 1,
			        strlen(proj_files[i]) - rootlen);
		}
	}
}

void project_find_file(int fd)
{
	char root[1024];
	const char *rootp = ".";
	char query[64];
	int qlen = 0, sel = 0, c, i;
	char msg[512];
	int off, plen;
	const int max_show = PICKER_MAX_ENTRIES;
	const char *names[PICKER_MAX_ENTRIES];
	int idx[PICKER_MAX_ENTRIES];

	proj_nfiles = 0;
	if (editor_find_project_root(root, sizeof root)) rootp = root;
	proj_walk(rootp);
	proj_make_relative(rootp, (int)strlen(rootp));

	if (proj_nfiles == 0) {
		editor_set_status_message("No files found under project root");
		return;
	}

	query[0] = '\0';
	plen = (int)strlen("project find file: ");

	while (1) {
		int matches = 0, shown, total = 0;

		/* Prefix matches first, then substring — same ranking as the
		 * buffer/named-command pickers. */
		for (i = 0; i < proj_nfiles && total < PROJ_MAX_FILES; i++) {
			if (editor_picker_match_rank(proj_files[i], query) != 0)
				continue;
			if (matches < max_show) {
				names[matches] = proj_files[i];
				idx[matches]   = i;
			}
			matches++; total++;
		}
		if (qlen > 0) {
			for (i = 0; i < proj_nfiles && total < PROJ_MAX_FILES; i++) {
				if (editor_picker_match_rank(proj_files[i], query) != 1)
					continue;
				if (matches < max_show + 0) {
					if (matches < max_show) {
						names[matches] = proj_files[i];
						idx[matches]   = i;
					}
					matches++;
				}
				total++;
			}
		}
		shown = matches > max_show ? max_show : matches;
		if (sel >= shown && shown > 0) sel = shown - 1;

		off = 0;
		editor_msg_appendf(msg, sizeof(msg), &off, "project find file: %s ", query);
		editor_picker_render(msg, sizeof(msg), &off, names, shown, total, sel);
		editor_set_status_message("%s", msg);
		editor.echo_cursor_col = plen + qlen + 1;
		editor_refresh_screen();

		c = editor_read_key(fd);
		if (c == DEL_KEY || c == CTRL_H || c == BACKSPACE) {
			if (qlen > 0) query[--qlen] = '\0';
			sel = 0;
		} else if (c == ARROW_RIGHT || c == CTRL_F) {
			if (shown > 0) sel = (sel + 1) % shown;
		} else if (c == ARROW_LEFT || c == CTRL_B) {
			if (shown > 0) sel = (sel - 1 + shown) % shown;
		} else if (c == ENTER) {
			editor.echo_cursor_col = 0;
			if (shown > 0 && sel >= 0 && sel < shown) {
				char path[1100];
				snprintf(path, sizeof path, "%s/%s", rootp, names[idx[sel]]);
				buf_open_path(path, 0);
				editor_goto_line_direct(1, 1);
				editor_set_status_message("%s", editor.filename ? editor.filename : "[new]");
			} else {
				editor_set_status_message("");
			}
			return;
		} else if (c == ESC || c == CTRL_G) {
			editor.echo_cursor_col = 0;
			editor_set_status_message("");
			return;
		} else if (isprint(c) && qlen < (int)sizeof(query) - 1) {
			query[qlen++] = c;
			query[qlen]   = '\0';
			sel = 0;
		}
	}
}

/* project_select is a placeholder — find-file opens directly from the
 * picker (no separate *project* buffer).  Kept for symmetry with the
 * other modes and future expansion. */
void project_select(void) {}
