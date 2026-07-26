/* ============================ xref-find-definitions =======================
 *
 * A regex-fallback "jump to definition" (M-.) with no external dependencies.
 *
 * Extracts the identifier at point, then scans the current buffer (and, if
 * the current file exists on disk, the whole file) for lines that look like
 * a definition of that identifier in the current language.  Patterns are
 * language-dependent and intentionally simple — anchored at line start, with
 * a couple of metacharacters — so this catches the common case (top-level
 * function/struct/class defs) without a tags file or a language server.
 *
 *   single match   → jump straight there
 *   multiple       → open a *xref* buffer listing file:line: text, Enter
 *                    on a line jumps to it (reuses the grep file:line parse)
 *   no match       → "No definition found for '<name>'"
 *
 * See xref_select() for the jump handler; SHL_XREF drives the Enter dispatch.
 */

#include "def.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct editor_syntax xref_syntax = {
	"Xref", NULL, NULL, "", "", "", SHL_XREF
};

#define XREF_NAME "*xref*"
#define XREF_MAX_MATCHES 256
#define XREF_MARK_RING 64          /* max saved positions for M-, */

/* A found definition.  file is a static buffer (overwritten per match);
 * xref_populate copies it into the buffer rows so it's fine to reuse. */
struct xref_match {
	char file[256];     /* buflist[i].filename or a disk path */
	int  line;          /* 1-based */
	char text[160];     /* the matching line, trimmed */
};

static struct xref_match xref_matches[XREF_MAX_MATCHES];
static int xref_nmatches;

/* ---- mark ring (M-, jumps back) --------------------------------------- *
 *
 * Each M-. jump pushes where point was before the jump; M-, pops the most
 * recent and jumps back there.  Separate from the region mark (which is
 * clobbered by selection commands), so xref jumps and region ops don't
 * interfere. */
struct xref_mark {
	char file[256];
	int  line;   /* 1-based */
	int  col;    /* 1-based */
};
static struct xref_mark xref_ring[XREF_MARK_RING];
static int xref_ring_len = 0;

/* Push the current position (filename + 1-based line/col) onto the ring. */
static void xref_push_mark(void)
{
	int row, col;
	if (xref_ring_len >= XREF_MARK_RING) {
		/* drop oldest to make room */
		memmove(&xref_ring[0], &xref_ring[1],
		        (XREF_MARK_RING - 1) * sizeof xref_ring[0]);
		xref_ring_len = XREF_MARK_RING - 1;
	}
	row = editor.rowoff + editor.cy;
	col = editor.coloff + editor.cx;
	snprintf(xref_ring[xref_ring_len].file,
	         sizeof xref_ring[0].file, "%s",
	         editor.filename ? editor.filename : "");
	xref_ring[xref_ring_len].line = row + 1;
	xref_ring[xref_ring_len].col  = col + 1;
	xref_ring_len++;
}

/* M-,: jump back to the last M-. departure point. */
void xref_pop_mark_ring(void)
{
	struct xref_mark m;
	int slot;

	if (xref_ring_len <= 0) {
		editor_set_status_message("No xref mark to pop");
		return;
	}
	xref_ring_len--;
	m = xref_ring[xref_ring_len];
	slot = buf_open_path(m.file, 0);
	if (slot < 0) return;
	editor_goto_line_direct(m.line, m.col);
	editor_set_status_message("Back to %s:%d", m.file, m.line);
}

/* ---- identifier at point ----------------------------------------------- */

/* Fill out[] with the identifier (alnum + _) under the cursor and return
 * its length, or 0 if point isn't on an identifier character. */
static int xref_word_at_point(char *out, int outsize)
{
	int filerow = editor.rowoff + editor.cy;
	int filecol = editor.coloff + editor.cx;
	erow *row;
	int start, end, len;

	if (filerow < 0 || filerow >= editor.numrows) return 0;
	row = &editor.row[filerow];
	if (filecol < 0 || filecol > row->size) return 0;

	/* If point is just past an identifier (common: cursor at end of word),
	 * step back one so we still grab it. */
	if (filecol >= row->size || !(isalnum((unsigned char)row->chars[filecol]) ||
	                              row->chars[filecol] == '_')) {
		if (filecol > 0 && (isalnum((unsigned char)row->chars[filecol-1]) ||
		                    row->chars[filecol-1] == '_'))
			filecol--;
		else
			return 0;
	}

	start = filecol;
	while (start > 0 && (isalnum((unsigned char)row->chars[start-1]) ||
	                     row->chars[start-1] == '_'))
		start--;
	end = filecol;
	while (end < row->size && (isalnum((unsigned char)row->chars[end]) ||
	                           row->chars[end] == '_'))
		end++;

	len = end - start;
	if (len <= 0 || len >= outsize) return 0;
	memcpy(out, row->chars + start, len);
	out[len] = '\0';
	return len;
}

/* ---- language-dependent definition patterns ---------------------------- *
 *
 * Each pattern is matched against the start of a line.  Metacharacters:
 *   %s  one or more whitespace
 *   %S  zero or more non-newline chars (a greedy "any prefix" — used to
 *       skip a return type like "static int" before the function name)
 *   %w  the identifier we're looking for (word-boundary checked after)
 * Everything else is a literal.  Patterns are tried in order; the first that
 * matches wins (so put more specific forms first).  A NULL list means "no
 * patterns for this language" — the finder then falls back to a generic
 * shape. */

static const char *xref_patterns_C[] = {
	"%s* struct %w ",          /* struct Foo { / struct Foo ; */
	"%s* struct %w{",
	"%s* enum %w ",
	"%s* enum %w{",
	"%s* #define %w",
	"%s* typedef %S %w;",      /* typedef ... Foo; */
	"%s* %S %w(",              /* [static int] foo(...)  (the common case) */
	NULL
};

static const char *xref_patterns_Python[] = {
	"%s* def %w(",
	"%s* class %w(",
	"%s* class %w:",
	NULL
};

static const char *xref_patterns_Shell[] = {
	"%s* %w()",
	"%s* %w() {",
	"%s* function %w",
	NULL
};

static const char *xref_patterns_Rust[] = {
	"%s* fn %w(",
	"%s* struct %w",
	"%s* enum %w",
	"%s* trait %w",
	"%s* impl %w",
	NULL
};

static const char *xref_patterns_Java[] = {
	"%s* class %w",
	"%s* interface %w",
	"%s* enum %w",
	"%s* %S %w(",              /* method */
	NULL
};

static const char *xref_patterns_JS[] = {
	"%s* function %w(",
	"%s* class %w",
	"%s* %w(",
	"%s* %w =",
	"%s* %w:",
	NULL
};

/* Pick the pattern list for the current buffer's syntax name. */
static const char **xref_patterns_for(const char *lang)
{
	if (!lang) return NULL;
	if (!strcmp(lang, "C") || !strcmp(lang, "C#") || !strcmp(lang, "PHP"))
		return xref_patterns_C;
	if (!strcmp(lang, "Python")) return xref_patterns_Python;
	if (!strcmp(lang, "Shell")) return xref_patterns_Shell;
	if (!strcmp(lang, "Rust"))  return xref_patterns_Rust;
	if (!strcmp(lang, "Java") || !strcmp(lang, "TypeScript") ||
	    !strcmp(lang, "Swift") || !strcmp(lang, "Dart"))
		return xref_patterns_Java;
	if (!strcmp(lang, "JavaScript") || !strcmp(lang, "React") ||
	    !strcmp(lang, "Vue") || !strcmp(lang, "Angular") ||
	    !strcmp(lang, "Svelte"))
		return xref_patterns_JS;
	return NULL;
}

/* Match one pattern against the start of line `s` (len chars).  name is the
 * identifier.  Returns 1 on match.
 * %S is "any prefix": it tries the rest of the pattern at every position
 * where the following %w token appears in the line (so "%S %w(" finds the
 * name-plus-open-paren anywhere after leading whitespace). */
static int xref_match_pattern(const char *pat, const char *s, int len,
                              const char *name)
{
	int i = 0, j = 0, nlen = (int)strlen(name);

	while (pat[j]) {
		if (pat[j] == '%') {
			char c = pat[j+1];
			j += 2;
			if (c == 's') {
				/* one or more whitespace */
				if (i >= len || !isspace((unsigned char)s[i])) return 0;
				while (i < len && isspace((unsigned char)s[i])) i++;
			} else if (c == 'w') {
				/* the identifier, with a word boundary after */
				if (i + nlen > len) return 0;
				if (memcmp(s + i, name, nlen) != 0) return 0;
				if (i + nlen < len &&
				    (isalnum((unsigned char)s[i+nlen]) || s[i+nlen] == '_'))
					return 0;
				i += nlen;
			} else if (c == 'S') {
				/* greedy-ish "any prefix": scan forward for a position where
				 * the remainder of the pattern matches.  The remainder must
				 * start with %w (our only use), so jump to each occurrence
				 * of `name` as a word and try the tail from there. */
				const char *tail = pat + j;   /* remainder after %S */
				int k;
				/* skip the " " literal that usually follows %S in our pats */
				/* (patterns write "%S %w(" meaning prefix + space + name) */
				for (k = i; k + nlen <= len; k++) {
					if (memcmp(s + k, name, nlen) != 0) continue;
					/* word boundary before (not a word char) */
					if (k > 0 && (isalnum((unsigned char)s[k-1]) || s[k-1] == '_'))
						continue;
					/* word boundary after */
					if (k + nlen < len &&
					    (isalnum((unsigned char)s[k+nlen]) || s[k+nlen] == '_'))
						continue;
					/* try the tail from here; tail is e.g. "%w(" — but %w
					 * already consumed by the scan, so match the literal
					 * part after %w in the tail. */
					{
						const char *t = tail;
						int ii = k + nlen;
						/* skip %w in tail (we already matched name) */
						if (t[0] == '%' && t[1] == 'w') t += 2;
						if (xref_match_pattern(t, s + ii, len - ii, name))
							return 1;
					}
				}
				return 0;
			} else {
				return 0;  /* unknown metachar */
			}
		} else {
			if (i >= len || s[i] != pat[j]) return 0;
			i++; j++;
		}
	}
	return 1;
}

/* Does `line` look like a definition of `name` in language `lang`? */
static int xref_line_is_def(const char *line, int len, const char *name,
                            const char *lang)
{
	const char **pats;
	int k, i;
	int has_paren_call;

	/* Trim trailing whitespace to inspect the line's end. */
	while (len > 0 && isspace((unsigned char)line[len-1])) len--;

	pats = xref_patterns_for(lang);
	if (pats) {
		for (k = 0; pats[k]; k++)
			if (xref_match_pattern(pats[k], line, len, name))
				goto matched;
	}
	/* Generic fallback: "<name>(" somewhere after a word boundary, with the
	 * line ending in ')' or ') {' (a definition body), not ';' (a call or
	 * forward declaration) and not inside an assignment. */
	{
		int nlen = (int)strlen(name);
		for (i = 0; i + nlen <= len; i++) {
			if (memcmp(line + i, name, nlen) != 0) continue;
			if (i > 0 && (isalnum((unsigned char)line[i-1]) || line[i-1] == '_'))
				continue;
			if (i + nlen < len &&
			    (isalnum((unsigned char)line[i+nlen]) || line[i+nlen] == '_'))
				continue;
			if (i + nlen < len && line[i+nlen] == '(')
				goto matched;
		}
	}
	return 0;

matched:
	/* Reject indented lines that don't start with a definition keyword.
	 * Top-level definitions (C functions, struct/enum, #define, typedef,
	 * Rust fn/struct, Shell funcs) sit at column 0; call sites live inside
	 * function bodies and are indented.  Keyword-prefixed forms (def/fn/
	 * function/struct/enum/class/trait/impl/interface/typedef/#define/
	 * static) are accepted at any indentation so Python methods and nested
	 * Rust items still resolve. */
	{
		static const char *kw[] = {
			"def", "fn", "function", "struct", "enum", "class",
			"trait", "impl", "interface", "typedef", "#define",
			"static", "public", "private", "protected", "final",
			"inline", "extern", "const", NULL
		};
		if (line[0] == ' ' || line[0] == '\t') {
			int p = 0, kw_i, is_kw = 0;
			while (p < len && isspace((unsigned char)line[p])) p++;
			for (kw_i = 0; kw[kw_i]; kw_i++) {
				size_t kl = strlen(kw[kw_i]);
				if (p + (int)kl <= len && memcmp(line + p, kw[kw_i], kl) == 0 &&
				    (p + (int)kl == len || !isalnum((unsigned char)line[p+kl])))
					{ is_kw = 1; break; }
			}
			if (!is_kw) return 0;
		}
	}
	/* Reject lines ending with ';' that contain a parenthesised call — a
	 * statement (call or forward declaration), not a definition body.
	 * #define / typedef end with ';' legitimately but are matched by their
	 * own keyword-prefixed patterns and would have is_kw above; the bare
	 * ';' reject only bites the generic %w( shape. */
	has_paren_call = 0;
	for (i = 0; i < len; i++) if (line[i] == '(') { has_paren_call = 1; break; }
	if (has_paren_call && len > 0 && line[len-1] == ';')
		return 0;
	return 1;
}

/* ---- search ---------------------------------------------------------- *
 *
 * Scan files on disk for definition lines.  Open buffers are not scanned
 * — M-. reflects the committed tree, so the result list isn't polluted by
 * in-progress edits. */

/* Does `path` end with one of the current language's extension patterns?
 * Delegates to syntax.c's matcher so xref stays in sync with HLDB. */
#define xref_path_matches_lang(path, lang) syntax_path_matches_lang(path, lang)

/* Read `path` line by line and collect definition matches.  Bounded by
 * XREF_MAX_MATCHES; each line is checked against xref_line_is_def. */
static void xref_scan_file(const char *path, const char *name, const char *lang)
{
	FILE *fp;
	char line[512];
	int lineno = 0, len;

	fp = fopen(path, "r");
	if (!fp) return;
	while (fgets(line, sizeof line, fp) &&
	       xref_nmatches < XREF_MAX_MATCHES) {
		lineno++;
		len = (int)strlen(line);
		while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) len--;
		if (xref_line_is_def(line, len, name, lang)) {
			snprintf(xref_matches[xref_nmatches].file,
			         sizeof xref_matches[0].file, "%s", path);
			xref_matches[xref_nmatches].line = lineno;
			{
				int t = 0, s = 0;
				while (s < len && isspace((unsigned char)line[s])) s++;
				while (t < (int)sizeof(xref_matches[0].text) - 1 && s + t < len)
					xref_matches[xref_nmatches].text[t] = line[s+t], t++;
				xref_matches[xref_nmatches].text[t] = '\0';
			}
			xref_nmatches++;
		}
	}
	fclose(fp);
}

/* Recursive directory walk from `dir`, scanning files whose extension
 * matches the current language.  Skips .git, hidden dirs, and the usual
 * build/output dirs.  Bounded by a file count so a huge tree won't stall. */
#define XREF_MAX_FILES 4000
static int xref_file_count;

static void xref_walk_dir(const char *dir, const char *name, const char *lang)
{
	DIR *dp;
	struct dirent *e;
	char path[1024];

	if (xref_file_count >= XREF_MAX_FILES) return;
	dp = opendir(dir);
	if (!dp) return;
	while ((e = readdir(dp)) != NULL && xref_file_count < XREF_MAX_FILES) {
		struct stat st;

		if (e->d_name[0] == '.') continue;  /* hidden + . and .. */
		snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
		if (stat(path, &st) != 0) continue;
		if (S_ISDIR(st.st_mode)) {
			/* skip common non-source dirs */
			if (!strcmp(e->d_name, "node_modules") ||
			    !strcmp(e->d_name, "build") ||
			    !strcmp(e->d_name, "dist") ||
			    !strcmp(e->d_name, "target") ||
			    !strcmp(e->d_name, ".git"))
				continue;
			xref_walk_dir(path, name, lang);
		} else if (S_ISREG(st.st_mode)) {
			xref_file_count++;
			if (xref_path_matches_lang(path, lang))
				xref_scan_file(path, name, lang);
		}
	}
	closedir(dp);
}

/* ---- *xref* buffer --------------------------------------------------- */

static void xref_populate(void)
{
	char line[256];
	int i, len;

	for (i = 0; i < xref_nmatches; i++) {
		len = snprintf(line, sizeof line, "%s:%d: %s",
		               xref_matches[i].file, xref_matches[i].line,
		               xref_matches[i].text);
		editor_insert_row(editor.numrows, line, len);
	}
}

static void xref_rehighlight(void)
{
	int i;
	for (i = 0; i < editor.numrows; i++)
		editor_update_row(&editor.row[i]);
}

/* Find the project root above the current file: the nearest ancestor
 * directory containing a project marker (.git, Makefile, package.json,
 * Cargo.toml, .hg, .svn, TAGS).  Writes the absolute path to rootbuf and
 * returns 1, or returns 0 (rootbuf untouched) if no marker is found. */
static int xref_find_root(char *rootbuf, int rootsize)
{
	char dir[1024];
	char *slash;

	if (!editor.filename) return 0;
	/* Start from the current file's directory. */
	snprintf(dir, sizeof dir, "%s", editor.filename);
	slash = strrchr(dir, '/');
	if (!slash) return 0;
	*slash = '\0';   /* truncate to dir */

	/* Walk up, checking each ancestor for a marker. */
	while (1) {
		static const char *markers[] = {
			".git", "Makefile", "package.json", "Cargo.toml",
			".hg", ".svn", "TAGS", NULL
		};
		int i;
		for (i = 0; markers[i]; i++) {
			char probe[1100];
			struct stat st;
			snprintf(probe, sizeof probe, "%s/%s", dir, markers[i]);
			if (stat(probe, &st) == 0) {
				snprintf(rootbuf, rootsize, "%s", dir);
				return 1;
			}
		}
		/* Go up one level. */
		slash = strrchr(dir, '/');
		if (!slash) {
			/* No slash left: the current dir is a bare name like "lib".
			 * Try "." (the parent) as the next level up. */
			if (!strcmp(dir, ".")) break;
			strcpy(dir, ".");
			continue;
		}
		if (slash == dir) break;   /* at filesystem root, stop */
		*slash = '\0';
	}
	return 0;
}

/* Public: run the search and either jump (single match) or open *xref*. */
void xref_find_definitions(void)
{
	char name[128];
	const char *lang;

	if (!editor.syntax) lang = NULL;
	else lang = editor.syntax->name;

	if (!xref_word_at_point(name, sizeof name)) {
		editor_set_status_message("No identifier at point");
		return;
	}

	xref_nmatches = 0;
	/* Walk files on disk from the project root (detected by walking up
	 * for .git/Makefile/package.json/Cargo.toml/.hg/.svn/TAGS) so M-.
	 * from src/foo.c finds defs in any sibling dir; fall back to the
	 * current file's directory, then ".", if no marker is found.  Only
	 * files on disk are scanned — open buffers are not, so M-. always
	 * reflects the committed tree and the result list isn't polluted by
	 * in-progress edits. */
	if (lang) {
		char rootbuf[1024];
		const char *dir = ".";
		if (xref_find_root(rootbuf, sizeof rootbuf)) {
			dir = rootbuf;
		} else if (editor.filename) {
			const char *slash = strrchr(editor.filename, '/');
			if (slash) {
				int dl = slash - editor.filename;
				if (dl >= (int)sizeof rootbuf) dl = (int)sizeof rootbuf - 1;
				memcpy(rootbuf, editor.filename, dl);
				rootbuf[dl] = '\0';
				dir = rootbuf;
			}
		}
		xref_file_count = 0;
		xref_walk_dir(dir, name, lang);
	}

	if (xref_nmatches == 0) {
		editor_set_status_message("No definition found for '%s'", name);
		return;
	}
	if (xref_nmatches == 1) {
		xref_push_mark();
		int slot = buf_open_path(xref_matches[0].file, 0);
		if (slot < 0) return;
		editor_goto_line_direct(xref_matches[0].line, 1);
		editor_set_status_message("%s:%d", xref_matches[0].file, xref_matches[0].line);
		return;
	}
	/* Multiple: show the *xref* buffer.  Carrying the match list through a
	 * static is safe because populate runs before any further edits.
	 * Push the current position now so M-, after the user picks a match
	 * unwinds: pick -> jump (pushes *xref* pos), M-, -> back to *xref*,
	 * M-, -> back to here (the original M-. site). */
	xref_push_mark();
	buf_open_special(XREF_NAME, &xref_syntax, xref_populate,
	                 "xref — RET to jump, q to close.");
	xref_rehighlight();
}

/* Enter in *xref*: parse "file:line: text" and jump (same shape as grep). */
void xref_select(void)
{
	int filerow = editor.rowoff + editor.cy;
	const char *s;
	int len, i, colon1, colon2, linenum;
	char path[512];
	int plen;

	if (editor.syntax != &xref_syntax) return;
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
		xref_push_mark();
		int slot = buf_open_path(path, 0);
		if (slot < 0) return;
		editor_goto_line_direct(linenum, 1);
		editor_set_status_message("%s:%d", path, linenum);
	}
}
