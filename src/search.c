/* ============================ Find / Replace =============================== */

#include "def.h"

#define KILO_QUERY_LEN 256

#define RESTORE_HL do { \
	if (saved_hl) { \
		memcpy(editor.row[saved_hl_line].hl, saved_hl, editor.row[saved_hl_line].rsize); \
		free(saved_hl); \
		saved_hl = NULL; \
	} \
} while (0)

/* Smart case: an all-lowercase query folds case, a query with any uppercase
 * letter searches case-sensitively, like GNU Emacs. */
/* The most recent successfully completed incremental-search query.
 * C-s/C-r with an empty prompt recalls this, like GNU Emacs. */
static char last_search_query[KILO_QUERY_LEN + 1] = "";

/* Multi-row highlight save/restore for incremental search.
 * Every visible match is highlighted, so more than one row's hl
 * array may be modified per iteration.  hl_stack holds, for each
 * touched row, a snapshot of its original hl bytes so they can be
 * restored before the next iteration or on exit.  */
struct hl_save { int row; unsigned char *data; int len; };

static void isearch_hl_save(struct hl_save *st, int *n, int maxn, int row)
{
	erow *r;
	int i;

	if (row < 0 || row >= editor.numrows) return;
	for (i = 0; i < *n; i++)        /* already saved? */
		if (st[i].row == row) return;
	if (*n >= maxn) return;          /* stack full: skip extra */
	r = &editor.row[row];
	if (!r->hl || !r->rsize) return;
	st[*n].row = row;
	st[*n].len = r->rsize;
	st[*n].data = malloc(r->rsize);
	memcpy(st[*n].data, r->hl, r->rsize);
	(*n)++;
}

static void isearch_hl_restore(struct hl_save *st, int *n)
{
	int i;
	for (i = 0; i < *n; i++) {
		erow *r = &editor.row[st[i].row];
		if (r->hl && r->rsize == st[i].len)
			memcpy(r->hl, st[i].data, st[i].len);
		free(st[i].data);
	}
	*n = 0;
}

static int query_has_upper(const char *q, int qlen)
{
	int i;

	for (i = 0; i < qlen; i++)
		if (isupper((unsigned char)q[i]))
			return 1;
	return 0;
}

/* strstr, optionally folding case. */
static char *case_strstr(const char *hay, const char *needle, int fold)
{
	if (!fold)
		return strstr(hay, needle);
	if (!*needle)
		return (char *)hay;

	for (; *hay; hay++) {
		const char *h = hay, *n = needle;

		while (*h && *n &&
		       tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
			h++;
			n++;
		}
		if (!*n)
			return (char *)hay;
	}
	return NULL;
}

/* Rightmost occurrence of needle whose end falls at or before `limit`, so a
 * reverse search lands on the match before point (not one straddling it) and
 * repeats step backward, like GNU Emacs. */
static char *isearch_find_last_before(const char *s, const char *needle,
				      int limit, int qlen, int fold)
{
	char *best = NULL;
	char *match = (char *)s;

	while ((match = case_strstr(match, needle, fold)) != NULL) {
		if (match - s + qlen > limit)
			break;
		best = match;
		match++;
	}
	return best;
}

/* Scan the rows from (start_row, start_col) in `direction`, wrapping once
 * through the buffer, for `query`.  On a hit fills *match_row/_col/_len and
 * returns 1; returns 0 when nothing matches.  Columns index row->render. */
static int isearch_find_match(int start_row, int start_col, int direction,
			      const char *query, int qlen, int fold,
			      int *match_row, int *match_col, int *match_len)
{
	int current, i;

	if (editor.numrows == 0 || qlen == 0) return 0;
	if (start_row < 0) start_row = 0;
	else if (start_row >= editor.numrows) start_row = editor.numrows - 1;

	current = start_row;
	for (i = 0; i < editor.numrows; i++) {
		erow *row = &editor.row[current];
		int col = (i == 0) ? start_col : (direction > 0 ? 0 : row->rsize);
		char *match;

		if (col < 0) col = 0;
		else if (col > row->rsize) col = row->rsize;

		if (direction > 0)
			match = case_strstr(row->render + col, query, fold);
		else
			match = isearch_find_last_before(row->render, query, col, qlen, fold);

		if (match) {
			*match_row = current;
			*match_col = match - row->render;
			*match_len = qlen;
			return 1;
		}

		current += direction;
		if (current < 0) current = editor.numrows - 1;
		else if (current == editor.numrows) current = 0;
	}
	return 0;
}

/* A motion or set-mark command typed during incremental search ends the
 * search and runs from the match, the way Emacs hands off to the command
 * instead of beeping.  Returns 1 if c was such a key, 0 otherwise.  These
 * motions mirror editor_process_keypress(); keep the two in step. */
static int isearch_handoff_key(int c)
{
	switch (c) {
	case KEY_NULL:            /* C-SPC: set mark, leave point at the match */
		editor_set_mark();
		return 1;
	case CTRL_A:
	case HOME_KEY:
		editor_move_cursor(HOME_KEY);
		break;
	case CTRL_E:
	case END_KEY:
		editor_move_cursor(END_KEY);
		break;
	case CTRL_B:
	case ARROW_LEFT:
		editor_move_cursor(ARROW_LEFT);
		break;
	case CTRL_F:
	case ARROW_RIGHT:
		editor_move_cursor(ARROW_RIGHT);
		break;
	case CTRL_N:
	case ARROW_DOWN:
		editor_move_cursor(ARROW_DOWN);
		break;
	case CTRL_P:
	case ARROW_UP:
		editor_move_cursor(ARROW_UP);
		break;
	case CTRL_D:
		editor_del_forward_char();
		break;
	case CTRL_HOME:
	case CTRL_PAGE_UP:
	case ALT_LT:
		editor_move_to_beginning();
		break;
	case CTRL_END:
	case CTRL_PAGE_DOWN:
	case ALT_GT:
		editor_move_to_end();
		break;
	case ALT_B:
	case CTRL_ARROW_LEFT:
		editor_move_word_backward();
		break;
	case ALT_F:
	case CTRL_ARROW_RIGHT:
		editor_move_word_forward();
		break;
	case ALT_M:
		editor_move_to_indentation();
		break;
	case ALT_A:
		editor_move_sentence_backward();
		break;
	case ALT_E:
		editor_move_sentence_forward();
		break;
	case ALT_LBRACE:
	case CTRL_ARROW_UP:
		editor_move_paragraph_backward();
		break;
	case ALT_RBRACE:
	case CTRL_ARROW_DOWN:
		editor_move_paragraph_forward();
		break;
	default:
		return 0;
	}
	editor_set_status_message("");
	return 1;
}

void editor_find(int fd, int direction)
{
	char query[KILO_QUERY_LEN+1] = {0};
	int saved_cx = editor.cx, saved_cy = editor.cy;
	int saved_coloff = editor.coloff, saved_rowoff = editor.rowoff;
	int start_row = editor.rowoff + editor.cy;
	int start_col = 0;
	int last_match_row = -1, last_match_col = -1;
	int find_next = 0; /* if 1 search next, if -1 search prev. */
	int qlen = 0;
	/* Multi-row highlight stack: one entry per touched row.  Cap at
	 * a generous limit so a pathological query in a huge buffer does
	 * not allocate unbounded memory; the current match is always
	 * highlighted regardless. */
	struct hl_save *hl_stack;
	int hl_n = 0, hl_cap;
	int r;

	hl_cap = editor.numrows;
	if (hl_cap < 1) hl_cap = 1;
	if (hl_cap > 8192) hl_cap = 8192;
	hl_stack = malloc((size_t)hl_cap * sizeof *hl_stack);

	/* Anchor the search at point so a fresh query, and reverse search in
	 * particular, starts where the cursor is rather than at the top.  The
	 * scan indexes row->render, so anchor in render columns too. */
	if (start_row >= 0 && start_row < editor.numrows)
		start_col = chars_to_render_col(&editor.row[start_row],
						editor.coloff + editor.cx);

	while (1) {
		int c;

		editor_set_status_message("I-search: %s", query);
		editor_refresh_screen();

		c = editor_read_key(fd);
		if (c == DEL_KEY || c == CTRL_H || c == BACKSPACE) {
			if (qlen != 0) query[--qlen] = '\0';
			last_match_row = last_match_col = -1;
			find_next = direction;
		} else if (c == ESC || c == ENTER || c == CTRL_G) {
			if (c == ESC) {
				editor.cx = saved_cx; editor.cy = saved_cy;
				editor.coloff = saved_coloff; editor.rowoff = saved_rowoff;
			}
			if (qlen > 0)
				strcpy(last_search_query, query);
			isearch_hl_restore(hl_stack, &hl_n);
			editor_set_status_message("");
			free(hl_stack);
			return;
		} else if (c == CTRL_S) {
			if (qlen == 0 && last_search_query[0]) {
				strcpy(query, last_search_query);
				qlen = (int)strlen(query);
				last_match_row = last_match_col = -1;
			}
			direction = find_next = 1;
		} else if (c == CTRL_R) {
			if (qlen == 0 && last_search_query[0]) {
				strcpy(query, last_search_query);
				qlen = (int)strlen(query);
				last_match_row = last_match_col = -1;
			}
			direction = find_next = -1;
		} else if (isprint(c)) {
			if (qlen < KILO_QUERY_LEN) {
				query[qlen++] = c;
				query[qlen] = '\0';
				last_match_row = last_match_col = -1;
				find_next = direction;
			}
		} else if (isearch_handoff_key(c)) {
			if (qlen > 0)
				strcpy(last_search_query, query);
			isearch_hl_restore(hl_stack, &hl_n);
			free(hl_stack);
			return;
		}

		/* Search occurrence. */
		if (find_next) {
			int current = start_row, col = start_col;
			int match_row, match_col, match_len;
			int fold = !query_has_upper(query, qlen);

			/* Repeat from just past the last hit; a fresh query
			 * (last_match_row == -1) restarts from point. */
			if (last_match_row != -1) {
				current = last_match_row;
				col = last_match_col + (direction > 0 ? 1 : 0);
			}
			find_next = 0;

			/* Restore any rows highlighted last iteration, then
			 * mark every match in the buffer.  The current match
			 * (the one point lands on) gets HL_MATCH_CURRENT so it
			 * stands out from the other HL_MATCH hits. */
			isearch_hl_restore(hl_stack, &hl_n);

			if (isearch_find_match(current, col, direction, query, qlen, fold,
					       &match_row, &match_col, &match_len)) {
				last_match_row = match_row;
				last_match_col = match_col;

				/* Highlight all matches across the whole buffer.
				 * Skip empty queries (shouldn't happen here). */
				if (qlen > 0) {
					for (r = 0; r < editor.numrows; r++) {
						erow *row = &editor.row[r];
						int off = 0;
						if (!row->hl || !row->rsize)
							continue;
						while (off + qlen <= row->rsize) {
							char *m = case_strstr(
								row->render + off,
								query, fold);
							if (!m) break;
							int mc = (int)(m - row->render);
							int is_cur = (r == match_row &&
								mc == match_col);
							isearch_hl_save(hl_stack,
								&hl_n, hl_cap, r);
							memset(row->hl + mc,
								is_cur ? HL_MATCH_CURRENT
								       : HL_MATCH,
								match_len);
							off = mc + match_len;
						}
					}
				}

				/* Land point at the far end of the match in the
				 * search direction: end when going forward, start
				 * when going back, like Emacs isearch.  The match
				 * was found in row->render (tab-expanded), so
				 * convert the render offset to a chars byte
				 * offset before positioning — otherwise tabs
				 * inflate the column and point lands past the
				 * match end. */
				{
					int render_col = match_col +
						(direction > 0 ? match_len : 0);
					int chars_col = render_col_to_chars(
						&editor.row[match_row], render_col);
					editor_reveal_position_centered(match_row,
						chars_col);
				}
			}
		}
	}
}

void editor_query_replace(int fd)
{
	char search[KILO_QUERY_LEN+1] = {0};
	char replace[KILO_QUERY_LEN+1] = {0};
	char *saved_hl = NULL;
	int saved_hl_line = -1;
	int slen, rlen, fold;
	int filerow, match_col;
	int count = 0, replace_all = 0;

	if (editor_readonly_blocked())
		return;

	if (editor_read_line(fd, "Query replace: ", search, sizeof(search)) < 0 || !search[0])
		return;
	if (editor_read_line(fd, "Replace with: ", replace, sizeof(replace)) < 0)
		return;

	slen = strlen(search);
	rlen = strlen(replace);
	fold = !query_has_upper(search, slen);
	filerow   = editor.rowoff + editor.cy;
	match_col = editor.coloff + editor.cx;

	while (filerow < editor.numrows) {
		char *match = case_strstr(editor.row[filerow].chars + match_col, search, fold);
		int c;

		if (!match) {
			filerow++;
			match_col = 0;
			continue;
		}
		match_col = match - editor.row[filerow].chars;

		editor_goto_line_direct(filerow + 1, match_col + 1);

		/* Highlight the match.  Convert the chars offset to a render
		 * offset so the highlight lands correctly even when tabs precede
		 * the match on the line. */
		RESTORE_HL;
		{
			erow *row = &editor.row[filerow];
			if (row->hl) {
				int i, rcol = 0;
				for (i = 0; i < match_col; i++)
					rcol += (row->chars[i] == '\t') ? (8 - rcol % 8) : 1;
				saved_hl_line = filerow;
				saved_hl = malloc(row->rsize);
				memcpy(saved_hl, row->hl, row->rsize);
				if (rcol + slen <= row->rsize)
					memset(row->hl + rcol, HL_MATCH, slen);
			}
		}

		if (!replace_all) {
			editor_set_status_message(
				"Replace \"%s\" with \"%s\"? (y/n/!/q)", search, replace);
			editor_refresh_screen();
			c = editor_read_key(fd);
		} else {
			c = 'y';
		}

		if (c == ESC || c == CTRL_G || c == 'q')
			break;
		if (c == '!') {
			replace_all = 1;
			c = 'y';
		}

		if (c == 'y' || c == ENTER) {
			erow *row = &editor.row[filerow];
			char matched[KILO_QUERY_LEN + 1];
			int i;

			/* Record the text actually matched, not the query: under
			 * case folding they differ, and undo must restore what was
			 * really there. */
			memcpy(matched, row->chars + match_col, slen);
			matched[slen] = '\0';

			/* Undo in two steps: YANK_TEXT (popped first) deletes the
			 * inserted replacement, then KILL_TEXT restores the original. */
			undo_push(UNDO_KILL_TEXT, filerow, match_col, 0, matched, slen);
			undo_push(UNDO_YANK_TEXT, filerow, match_col, 0, replace, rlen);

			suppress_undo = 1;
			for (i = 0; i < slen; i++)
				editor_row_del_char(row, match_col);
			for (i = 0; i < rlen; i++)
				editor_row_insert_char(row, match_col + i, (unsigned char)replace[i]);
			suppress_undo = 0;

			match_col += rlen;
			count++;
		} else {
			match_col++;
		}
	}

	RESTORE_HL;
	editor_set_status_message(count ? "Replaced %d occurrence%s." : "No replacements made.",
				  count, count == 1 ? "" : "s");
}
