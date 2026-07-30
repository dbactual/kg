/* yank.c - Kill ring for copy/paste operations.
 *
 * A real ring with up to KILL_RING_MAX entries, newest first.
 * killring.text / killring.len always mirror the newest entry so
 * existing code that reads them directly keeps working. */

#include "def.h"

/* Global kill ring */
struct kill_ring killring = {NULL, 0, NULL, NULL, 0};

/* Sync the flat text/len fields with ring entry 0. */
static void kill_ring_sync(void)
{
	killring.text = (killring.count > 0) ? killring.entries[0] : NULL;
	killring.len  = (killring.count > 0) ? killring.lens[0]    : 0;
}

/* Initialize the kill ring */
void kill_ring_init(void)
{
	killring.text = NULL;
	killring.len = 0;
	killring.entries = NULL;
	killring.lens = NULL;
	killring.count = 0;
}

/* Free the kill ring */
void kill_ring_free(void)
{
	int i;

	if (killring.entries) {
		for (i = 0; i < killring.count; i++)
			free(killring.entries[i]);
		free(killring.entries);
		killring.entries = NULL;
	}
	if (killring.lens) {
		free(killring.lens);
		killring.lens = NULL;
	}
	killring.count = 0;
	killring.text = NULL;
	killring.len = 0;
}

/* Push a new entry to the front of the ring.  Drops the oldest entry
 * when the ring is full (KILL_RING_MAX). */
void kill_ring_set(char *text, int len)
{
	char *dup;

	if (len <= 0) return;

	dup = malloc(len + 1);
	if (!dup) return;
	memcpy(dup, text, len);
	dup[len] = '\0';

	/* Grow arrays if needed. */
	if (killring.count == 0) {
		killring.entries = malloc(KILL_RING_MAX * sizeof(char *));
		killring.lens    = malloc(KILL_RING_MAX * sizeof(int));
		if (!killring.entries || !killring.lens) {
			free(dup);
			return;
		}
	} else if (killring.count >= KILL_RING_MAX) {
		/* Ring full: drop the oldest entry. */
		free(killring.entries[killring.count - 1]);
		killring.count--;
	}

	/* Shift existing entries down and insert at front. */
	memmove(killring.entries + 1, killring.entries,
		killring.count * sizeof(char *));
	memmove(killring.lens + 1, killring.lens,
		killring.count * sizeof(int));
	killring.entries[0] = dup;
	killring.lens[0]    = len;
	killring.count++;
	kill_ring_sync();
}

/* Append text to the newest entry (for consecutive kills like C-k C-k). */
void kill_ring_append(char *text, int len)
{
	char *new_text;

	if (len <= 0) return;

	if (killring.count == 0) {
		kill_ring_set(text, len);
		return;
	}

	new_text = realloc(killring.entries[0], killring.lens[0] + len + 1);
	if (!new_text) return;

	memcpy(new_text + killring.lens[0], text, len);
	new_text[killring.lens[0] + len] = '\0';
	killring.entries[0] = new_text;
	killring.lens[0] += len;
	kill_ring_sync();
}

/* Get the newest entry (returns NULL if empty) */
char *kill_ring_get(void)
{
	return killring.text;
}

/* Get entry at ring index (0 = newest).  Returns NULL if out of range. */
char *kill_ring_get_at(int idx, int *out_len)
{
	if (idx < 0 || idx >= killring.count) return NULL;
	if (out_len) *out_len = killring.lens[idx];
	return killring.entries[idx];
}

/* Set mark at current cursor position without echoing to the minibuffer.
 * Used by shift-select and rectangle commands where a status message
 * would be noisy. */
void editor_set_mark_silent(void)
{
	editor.mark_set = 1;
	editor.mark_row = editor.rowoff + editor.cy;
	editor.mark_col = editor.coloff + editor.cx;
	editor.mark_highlight = 1;
}

/* Set mark at current cursor position (C-Space, explicit set-mark). */
void editor_set_mark(void)
{
	editor_set_mark_silent();
	editor_set_status_message("Mark set");
}

/* Swap cursor and mark positions (C-x C-x).
 * Scrolls only as needed to keep the target visible, rather than centering,
 * so the user's view context is preserved when the mark is on-screen. */
void editor_exchange_point_and_mark(void)
{
	int cur_row, cur_col, mark_row, mark_col;

	if (!editor.mark_set) {
		editor_set_status_message("No mark set");
		return;
	}

	cur_row  = editor.rowoff + editor.cy;
	cur_col  = editor.coloff + editor.cx;
	mark_row = editor.mark_row;
	mark_col = editor.mark_col;

	editor_cursor_goto(mark_row, mark_col);

	editor.mark_row = cur_row;
	editor.mark_col = cur_col;
	editor.mark_highlight = 1;
	editor_set_status_message("Mark exchanged");
}

/* qsort comparator: byte-wise (case-sensitive) line order, like sort(1). */
static int sort_lines_cmp(const void *a, const void *b)
{
	const erow *ra = a;
	const erow *rb = b;

	return strcmp(ra->chars, rb->chars);
}

/* Sort the lines the region spans into byte order, as one undo step (M-x
 * sort-lines).  Needs an active mark; a line whose only coverage is point
 * sitting at its column 0 is left out, like GNU Emacs. */
void editor_sort_lines(void)
{
	int cur_row = editor.rowoff + editor.cy;
	int cur_col = editor.coloff + editor.cx;
	int start_row, end_row, end_col;
	int nlines, orig_len, i;
	char *orig;
	erow *tmp;

	if (editor_readonly_blocked())
		return;
	if (!editor.mark_set) {
		editor_set_status_message("No mark set");
		return;
	}

	if (editor.mark_row < cur_row ||
	    (editor.mark_row == cur_row && editor.mark_col < cur_col)) {
		start_row = editor.mark_row;
		end_row   = cur_row;
		end_col   = cur_col;
	} else {
		start_row = cur_row;
		end_row   = editor.mark_row;
		end_col   = editor.mark_col;
	}
	if (end_col == 0 && end_row > start_row)
		end_row--;

	/* A stale mark can point past a shrunken buffer; clamp so the row
	 * range stays in bounds (a start past EOF then falls out via nlines). */
	if (end_row >= editor.numrows)
		end_row = editor.numrows - 1;

	nlines = end_row - start_row + 1;
	if (nlines < 2)
		return;

	orig = editor_rows_to_string(&editor.row[start_row], nlines, &orig_len);
	if (!orig)
		return;

	tmp = malloc(nlines * sizeof(erow));
	if (!tmp) {
		free(orig);
		editor_set_status_message("Out of memory");
		return;
	}
	memcpy(tmp, &editor.row[start_row], nlines * sizeof(erow));
	qsort(tmp, nlines, sizeof(erow), sort_lines_cmp);
	/* Write the rows back in sorted order, fix each idx, then re-highlight
	 * so multiline syntax state (block comments, fenced code) re-propagates
	 * through the new order -- idx must be current before that runs. */
	for (i = 0; i < nlines; i++) {
		editor.row[start_row + i] = tmp[i];
		editor.row[start_row + i].idx = start_row + i;
		editor_update_row(&editor.row[start_row + i]);
	}
	free(tmp);

	/* Restore the pre-sort rows as one step; numrows is unchanged, so this
	 * reuses the rectangle-overwrite undo that snapshots a row range. */
	undo_push(UNDO_RECT_OVERWRITE, start_row, 0, editor.numrows, orig, orig_len);
	free(orig);

	editor.mark_highlight = 0;
	editor.rect_mode = 0;
	editor_snap_cx_to_row();
	editor.dirty = 1;
	editor_set_status_message("Sorted %d lines", nlines);
}

/* Get text from region (between mark and point) */
char *editor_get_region_text(int *out_len)
{
	int start_row, start_col, end_row, end_col;
	int cur_row = editor.rowoff + editor.cy;
	int cur_col = editor.coloff + editor.cx;
	int total_len = 0;
	char *text;
	int pos = 0;
	int row;

	if (!editor.mark_set) return NULL;

	/* Determine which position comes first */
	if (editor.mark_row < cur_row || (editor.mark_row == cur_row && editor.mark_col < cur_col)) {
		start_row = editor.mark_row;
		start_col = editor.mark_col;
		end_row = cur_row;
		end_col = cur_col;
	} else {
		start_row = cur_row;
		start_col = cur_col;
		end_row = editor.mark_row;
		end_col = editor.mark_col;
	}

	/* Calculate total length needed */
	for (row = start_row; row <= end_row && row < editor.numrows; row++) {
		if (row == start_row && row == end_row) {
			/* Single line region */
			total_len += end_col - start_col;
		} else if (row == start_row) {
			/* First line */
			total_len += editor.row[row].size - start_col + 1; /* +1 for newline */
		} else if (row == end_row) {
			/* Last line */
			total_len += end_col;
		} else {
			/* Middle lines */
			total_len += editor.row[row].size + 1; /* +1 for newline */
		}
	}

	if (total_len == 0) return NULL;

	/* Allocate and copy text */
	text = malloc(total_len + 1);
	if (!text) return NULL;

	for (row = start_row; row <= end_row && row < editor.numrows; row++) {
		int copy_start = (row == start_row) ? start_col : 0;
		int copy_end = (row == end_row) ? end_col : editor.row[row].size;
		int copy_len;

		if (copy_end > editor.row[row].size) copy_end = editor.row[row].size;
		if (copy_start > editor.row[row].size) copy_start = editor.row[row].size;

		copy_len = copy_end - copy_start;
		if (copy_len > 0) {
			memcpy(text + pos, editor.row[row].chars + copy_start, copy_len);
			pos += copy_len;
		}

		/* Add newline except for last line */
		if (row < end_row)
			text[pos++] = '\n';
	}

	text[pos] = '\0';
	*out_len = pos;
	return text;
}

/* Cut (save==1) or delete (save==0) the linear region.  Cursor lands at
 * the start of the region; undo restores it as a single step. */
static void region_kill_or_delete(int save)
{
	int start_row, start_col;
	int cur_row = editor.rowoff + editor.cy;
	int cur_col = editor.coloff + editor.cx;
	char *text;
	int len, i;

	if (editor_readonly_blocked())
		return;

	if (!editor.mark_set) {
		editor_set_status_message("No mark set");
		return;
	}

	text = editor_get_region_text(&len);
	if (!text) {
		editor_set_status_message("Empty region");
		return;
	}

	if (save) {
		kill_ring_set(text, len);
		copy_to_clipboard(text, len);
	}

	if (editor.mark_row < cur_row || (editor.mark_row == cur_row && editor.mark_col < cur_col)) {
		start_row = editor.mark_row;
		start_col = editor.mark_col;
	} else {
		start_row = cur_row;
		start_col = cur_col;
	}

	editor_cursor_goto(start_row, start_col);
	editor.coloff = 0;
	editor.cx = start_col;

	undo_push(UNDO_KILL_TEXT, start_row, start_col, 0, text, len);

	suppress_undo = 1;
	for (i = 0; i < len; i++)
		editor_del_forward_char();
	suppress_undo = 0;

	/* Drop the highlight and any transient-region machinery, but keep
	 * mark_set so C-x C-x after a region command can still bounce back
	 * to where the region started (matches Emacs, the C-g teardown,
	 * and the first-edit teardown in kbd.c). */
	editor.mark_highlight = 0;
	editor.rect_mode = 0;
	free(text);
	editor_set_status_message(save ? "Region killed" : "Region deleted");
}

void editor_kill_region(void)   { region_kill_or_delete(1); }
void editor_delete_region(void) { region_kill_or_delete(0); }

/* Copy region - saves to kill ring without removing */
void editor_copy_region(void)
{
	char *text;
	int len;

	if (!editor.mark_set) {
		editor_set_status_message("No mark set");
		return;
	}

	text = editor_get_region_text(&len);
	if (!text) {
		editor_set_status_message("Empty region");
		return;
	}

	kill_ring_set(text, len);
	copy_to_clipboard(text, len);
	editor.mark_highlight = 0;
	editor.rect_mode = 0;
	editor_snap_cx_to_row();
	free(text);
	editor_set_status_message("Region copied");
}

/* Delete key dispatch: consume an active region (rect or linear) without
 * saving, otherwise just delete the character ahead. */
void editor_delete_region_or_char(void)
{
	if (editor_readonly_blocked())
		return;

	if (editor.mark_set && editor.mark_highlight) {
		if (editor.rect_mode)
			editor_delete_rect();
		else
			editor_delete_region();
		return;
	}
	editor_del_forward_char();
}

/* Yank (paste) from kill ring.  Yanks the newest entry and records
 * the yank position/length so a following M-y can replace it. */
void editor_yank(void)
{
	int filerow = editor.rowoff + editor.cy;
	int filecol = editor.coloff + editor.cx;
	char *text = kill_ring_get();

	if (editor_readonly_blocked())
		return;

	if (!text) {
		editor_set_status_message("Kill ring is empty");
		return;
	}

	/* Record single undo operation for entire yank */
	undo_push(UNDO_YANK_TEXT, filerow, filecol, 0, text, killring.len);

	editor_insert_text_raw(text, killring.len);

	/* Record yank state for M-y. */
	editor.yank_active  = 1;
	editor.last_yank_row = filerow;
	editor.last_yank_col = filecol;
	editor.last_yank_len = killring.len;
	editor.last_yank_idx = 0;

	editor_set_status_message("Yanked");
}

/* Yank-pop (M-y): replace the last yank with the next-older kill-ring
 * entry.  Only valid immediately after C-y or a previous M-y. */
void editor_yank_pop(void)
{
	int next_idx, new_len;
	char *new_text;

	if (editor_readonly_blocked())
		return;

	if (!editor.yank_active) {
		editor_set_status_message("Previous command was not a yank");
		return;
	}

	if (killring.count < 2) {
		editor_set_status_message("Kill ring has only one entry");
		return;
	}

	/* Advance to the next-older entry, wrapping around. */
	next_idx = (editor.last_yank_idx + 1) % killring.count;
	new_text = kill_ring_get_at(next_idx, &new_len);
	if (!new_text)
		return;

	/* Push a single undo record for the whole yank-pop: the old text
	 * (to restore on undo) in text/len, the new text length (to delete
	 * on undo) in c. */
	{
		char *old_text = kill_ring_get_at(editor.last_yank_idx, NULL);
		undo_push(UNDO_YANK_POP, editor.last_yank_row, editor.last_yank_col,
			  new_len, old_text, editor.last_yank_len);
	}

	/* Delete the previously yanked text. */
	editor_cursor_goto(editor.last_yank_row, editor.last_yank_col);
	suppress_undo = 1;
	{
		int i;
		for (i = 0; i < editor.last_yank_len; i++)
			editor_del_forward_char();
	}
	suppress_undo = 0;

	/* Insert the older entry at the same position. */
	editor_insert_text_raw(new_text, new_len);

	/* Update yank state. */
	editor.last_yank_len = new_len;
	editor.last_yank_idx = next_idx;

	editor_set_status_message("Yank-pop (entry %d of %d)",
				  next_idx + 1, killring.count);
}
