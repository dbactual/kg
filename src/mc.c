/* mc.c - Multiple cursors (C-c m {n,a,j,k}, C-g to clear)
 *
 * A small parallel-edit core: a set of buffer-absolute secondary cursor
 * positions, and a curated list of primitives that apply at every cursor
 * at once.  This is the VS Code / kakoune model, NOT Emacs
 * multiple-cursors (which replays keystrokes at fake cursors and breaks
 * on prompts, isearch, yank-pop, and undo).
 *
 * Execution model for a parallel keystroke:
 *   1. merge the primary cursor into the set
 *   2. sort descending by (row, col)
 *   3. apply the edit at each cursor, highest first, so an edit never
 *      shifts the positions of cursors not yet processed (no markers)
 *   4. each cursor's new position = the natural end of its own edit
 *   5. dedupe overlaps, extract the primary, redraw
 *
 * Every parallel keystroke is wrapped in an explicit undo boundary pair,
 * so one C-_ undoes the whole stroke.
 */

#include "def.h"

/* Parallel editing is live only once the user says "begin" (or used an
 * auto-select command).  While collecting, movement and typing stay
 * single-cursor so the user can navigate freely between adds. */
int mc_active(void)
{
	return editor.mc_count > 0 && editor.mc_editing;
}

/* Nonzero when cursors are collected but editing has not begun: they
 * render (dim face) but no command runs in parallel. */
int mc_collecting(void)
{
	return editor.mc_count > 0 && !editor.mc_editing;
}

void mc_clear(void)
{
	if (editor.mc_count > 0 || editor.mc_editing) {
		editor.mc_count = 0;
		editor.mc_editing = 0;
		editor_set_status_message("Cursors cleared");
	}
}

/* --- cursor set helpers ---------------------------------------------- */

/* Compare two (row, col) positions: negative if a is above-left of b. */
static int mc_pos_cmp(int a_row, int a_col, int b_row, int b_col)
{
	if (a_row != b_row) return a_row - b_row;
	return a_col - b_col;
}

/* Add a cursor at (row, col), clamping col to the row's size.  Skips
 * positions already present (or at the primary cursor).  Returns 1 when
 * a cursor was actually added. */
static int mc_add(int row, int col)
{
	int i;

	if (row < 0 || row >= editor.numrows) return 0;
	if (col > editor.row[row].size) col = editor.row[row].size;
	if (col < 0) col = 0;

	/* Don't duplicate the primary cursor. */
	if (row == editor.rowoff + editor.cy &&
	    col == editor.coloff + editor.cx)
		return 0;
	for (i = 0; i < editor.mc_count; i++)
		if (editor.mc_row[i] == row && editor.mc_col[i] == col)
			return 0;
	if (editor.mc_count >= MC_MAX) return 0;

	editor.mc_row[editor.mc_count] = row;
	editor.mc_col[editor.mc_count] = col;
	editor.mc_count++;
	return 1;
}

/* Add the primary cursor's current position to the collection WITHOUT
 * skipping it (mc_add refuses positions equal to the primary, which is
 * right for the auto-select commands but wrong here: the whole point of
 * add-here is to record where point is).  Returns 1 when added. */
static int mc_add_here_pos(void)
{
	int row = editor.rowoff + editor.cy;
	int col = editor.coloff + editor.cx;
	int i;

	if (row < 0 || row >= editor.numrows) return 0;
	if (col > editor.row[row].size) col = editor.row[row].size;
	if (col < 0) col = 0;
	for (i = 0; i < editor.mc_count; i++)
		if (editor.mc_row[i] == row && editor.mc_col[i] == col)
			return 0;
	if (editor.mc_count >= MC_MAX) return 0;
	editor.mc_row[editor.mc_count] = row;
	editor.mc_col[editor.mc_count] = col;
	editor.mc_count++;
	return 1;
}

/* Remove duplicate positions (keep first occurrence). */
static void mc_dedupe(void)
{
	int i, j;

	for (i = 0; i < editor.mc_count; i++) {
		for (j = i + 1; j < editor.mc_count; j++) {
			if (editor.mc_row[i] == editor.mc_row[j] &&
			    editor.mc_col[i] == editor.mc_col[j]) {
				memmove(&editor.mc_row[j], &editor.mc_row[j+1],
					(editor.mc_count - j - 1) * sizeof(int));
				memmove(&editor.mc_col[j], &editor.mc_col[j+1],
					(editor.mc_count - j - 1) * sizeof(int));
				editor.mc_count--;
				j--;
			}
		}
	}
}

/* Build the full working set: primary + secondaries, sorted descending
 * by (row, col).  out_primary_idx receives the index of the primary
 * within the sorted arrays.  Returns the total count. */
static int mc_build_sorted(int *rows, int *cols, int *out_primary_idx)
{
	int n;
	int pr = editor.rowoff + editor.cy;
	int pc = editor.coloff + editor.cx;
	int i, j;

	n = 1;
	rows[0] = pr; cols[0] = pc;
	for (i = 0; i < editor.mc_count; i++) {
		/* The primary participates in the edit as itself; skip any
		 * collected cursor sitting at the same spot, or the edit
		 * would run twice there. */
		if (editor.mc_row[i] == pr && editor.mc_col[i] == pc)
			continue;
		rows[n] = editor.mc_row[i];
		cols[n] = editor.mc_col[i];
		n++;
	}
	/* Insertion sort descending. */
	for (i = 1; i < n; i++) {
		int r = rows[i], c = cols[i];
		j = i - 1;
		while (j >= 0 && mc_pos_cmp(rows[j], cols[j], r, c) < 0) {
			rows[j+1] = rows[j]; cols[j+1] = cols[j];
			j--;
		}
		rows[j+1] = r; cols[j+1] = c;
	}
	*out_primary_idx = -1;
	for (i = 0; i < n; i++)
		if (rows[i] == pr && cols[i] == pc) { *out_primary_idx = i; break; }
	return n;
}

/* Write the sorted set back: primary goes to editor, rest to mc[]. */
static void mc_store_sorted(int *rows, int *cols, int n, int primary_idx)
{
	int i, k = 0;

	/* Primary's final position. */
	editor_cursor_goto(rows[primary_idx], cols[primary_idx]);
	for (i = 0; i < n; i++) {
		if (i == primary_idx) continue;
		editor.mc_row[k] = rows[i];
		editor.mc_col[k] = cols[i];
		k++;
	}
	editor.mc_count = k;
	mc_dedupe();
}

/* --- parallel primitives --------------------------------------------- */

/* Edit-effect bookkeeping for position adjustment.  mc_run processes
 * cursors in descending (row, col) order, which keeps the UNPROCESSED
 * cursors valid (they sit above-left of each edit).  But the RESULT
 * positions of already-processed cursors can still be shifted by a later
 * edit: splitting a higher line pushes a lower result down a row, and
 * inserting a char on the same row pushes a higher result right a
 * column.  Each edit function records its effect here; mc_run then
 * adjusts every previously computed result before moving on.
 *
 * Kinds:
 *   0  no effect
 *   1  column edit on (adj_row): bytes at [adj_col ...) shift by adj_d
 *   2  line split at (adj_row, adj_col): rows below move down 1; same-row
 *      text right of adj_col moves to (adj_row+1, col-adj_col)
 *   3  join-prev: row adj_row merged onto row-1 at byte adj_merge
 *   4  join-next: row adj_row+1 merged onto adj_row at byte adj_merge
 *   5  text insert at (adj_row, adj_col): adj_d newlines, last-line
 *      length adj_merge
 */
static int mc_adj_kind;
static int mc_adj_row, mc_adj_col, mc_adj_d, mc_adj_merge;

static void mc_adjust_prior(int *rows, int *cols, int count)
{
	int j;

	if (mc_adj_kind == 0) return;
	for (j = 0; j < count; j++) {
		switch (mc_adj_kind) {
		case 1:
			if (rows[j] == mc_adj_row && cols[j] >= mc_adj_col) {
				cols[j] += mc_adj_d;
				if (cols[j] < 0) cols[j] = 0;
			}
			break;
		case 2:
			if (rows[j] > mc_adj_row) {
				rows[j]++;
			} else if (rows[j] == mc_adj_row && cols[j] > mc_adj_col) {
				rows[j]++;
				cols[j] -= mc_adj_col;
			}
			break;
		case 3:
			if (rows[j] == mc_adj_row) {
				rows[j]--;
				cols[j] += mc_adj_merge;
			} else if (rows[j] > mc_adj_row) {
				rows[j]--;
			}
			break;
		case 4:
			if (rows[j] == mc_adj_row + 1) {
				rows[j]--;
				cols[j] += mc_adj_merge;
			} else if (rows[j] > mc_adj_row + 1) {
				rows[j]--;
			}
			break;
		case 5:
			if (rows[j] > mc_adj_row) {
				rows[j] += mc_adj_d;
			} else if (rows[j] == mc_adj_row && cols[j] > mc_adj_col) {
				rows[j] += mc_adj_d;
				cols[j] = cols[j] - mc_adj_col + mc_adj_merge;
			}
			break;
		}
	}
}

/* Apply one edit at absolute (row, col), leaving the "cursor" at the
 * natural end of the edit.  These bypass the window-relative editor
 * machinery and work directly on rows, so they are safe to run at many
 * positions in turn.  Each returns the new (row, col) via out params. */

static void mc_insert_char_at(int row, int col, int c, int *orow, int *ocol)
{
	erow *r;

	while (row >= editor.numrows)
		editor_insert_row(editor.numrows, "", 0);
	r = &editor.row[row];
	if (col > r->size) col = r->size;
	editor_row_insert_char(r, col, c);
	mc_adj_kind = 1; mc_adj_row = row; mc_adj_col = col; mc_adj_d = 1;
	*orow = row;
	*ocol = col + 1;
}

static void mc_backspace_at(int row, int col, int arg, int *orow, int *ocol)
{
	erow *r;

	(void)arg;

	if (row >= editor.numrows) { *orow = row; *ocol = col; return; }
	r = &editor.row[row];
	if (col > r->size) col = r->size;

	if (col > 0) {
		/* Delete one glyph backward: find its start byte by skipping
		 * UTF-8 continuation bytes, then delete the whole glyph. */
		int start = col - 1;
		int n;
		while (start > 0 && utf8_is_cont((unsigned char)r->chars[start]))
			start--;
		n = col - start;
		while (n--) editor_row_del_char(r, start);
		mc_adj_kind = 1; mc_adj_row = row; mc_adj_col = col;
		mc_adj_d = -(col - start);
		*orow = row;
		*ocol = start;
	} else if (row > 0) {
		/* At BOL: join this row onto the end of the previous row. */
		erow *p = &editor.row[row-1];
		int prevlen = p->size;
		p->chars = realloc(p->chars, p->size + r->size + 1);
		memcpy(p->chars + p->size, r->chars, r->size);
		p->size += r->size;
		p->chars[p->size] = '\0';
		editor_update_row(p);
		editor_del_row(row);
		mc_adj_kind = 3; mc_adj_row = row; mc_adj_merge = prevlen;
		*orow = row - 1;
		*ocol = prevlen;
	} else {
		*orow = row; *ocol = col;
	}
}

static void mc_del_forward_at(int row, int col, int arg, int *orow, int *ocol)
{
	erow *r;

	(void)arg;

	if (row >= editor.numrows) { *orow = row; *ocol = col; return; }
	r = &editor.row[row];
	if (col > r->size) col = r->size;
	if (col < r->size) {
		/* Delete one glyph forward: its width is 1 + the number of
		 * UTF-8 continuation bytes that follow the start byte. */
		int n = 1, pos = col + 1;
		while (pos < r->size && utf8_is_cont((unsigned char)r->chars[pos])) {
			n++; pos++;
		}
		while (n--) editor_row_del_char(r, col);
		mc_adj_kind = 1; mc_adj_row = row; mc_adj_col = col; mc_adj_d = -n;
	} else if (row + 1 < editor.numrows) {
		/* At EOL: pull the next row up (join). */
		erow *next = &editor.row[row+1];
		int merged = r->size;
		r->chars = realloc(r->chars, r->size + next->size + 1);
		memcpy(r->chars + r->size, next->chars, next->size);
		r->size += next->size;
		r->chars[r->size] = '\0';
		editor_update_row(r);
		editor_del_row(row + 1);
		mc_adj_kind = 4; mc_adj_row = row; mc_adj_merge = merged;
	}
	*orow = row;
	*ocol = (col > r->size) ? r->size : col;
}

static void mc_newline_at(int row, int col, int arg, int *orow, int *ocol)
{
	erow *r;
	int rest_len;

	(void)arg;

	while (row >= editor.numrows)
		editor_insert_row(editor.numrows, "", 0);
	r = &editor.row[row];
	if (col > r->size) col = r->size;
	rest_len = r->size - col;
	/* Raw split: no auto-indent, so a parallel RET doesn't build a
	 * staircase of inherited indentation. */
	editor_insert_row(row + 1, r->chars + col, rest_len);
	r = &editor.row[row];
	r->chars[col] = '\0';
	r->size = col;
	editor_update_row(r);
	mc_adj_kind = 2; mc_adj_row = row; mc_adj_col = col;
	*orow = row + 1;
	*ocol = 0;
}

/* Insert a (possibly multi-line) string at (row, col). */
static void mc_insert_text_at(int row, int col, const char *text, int len,
			      int *orow, int *ocol)
{
	int cr = row, cc = col;
	int i;
	int newlines = 0, last_len = 0;

	for (i = 0; i < len; i++) {
		if (text[i] == '\n') {
			newlines++;
			last_len = 0;
			mc_newline_at(cr, cc, 0, &cr, &cc);
		} else {
			last_len++;
			mc_insert_char_at(cr, cc, text[i], &cr, &cc);
		}
	}
	/* Summarize the whole insert as one adjustment (the per-char statics
	 * set above only describe the last char, not the whole text). */
	mc_adj_kind = 5; mc_adj_row = row; mc_adj_col = col;
	mc_adj_d = newlines; mc_adj_merge = last_len;
	*orow = cr;
	*ocol = cc;
}

/* Snapshot the whole buffer as a '\n'-joined string for undo.  MC edits
 * can touch any rows and change the row count, so a full snapshot is the
 * robust undo record.  Caller frees. */
static char *mc_snapshot_buffer(int *out_len)
{
	int total = 0;
	int r;
	char *buf, *p;

	for (r = 0; r < editor.numrows; r++) {
		total += editor.row[r].size;
		if (r < editor.numrows - 1) total++;
	}
	buf = malloc(total + 1);
	if (!buf) { *out_len = 0; return NULL; }
	p = buf;
	for (r = 0; r < editor.numrows; r++) {
		memcpy(p, editor.row[r].chars, editor.row[r].size);
		p += editor.row[r].size;
		if (r < editor.numrows - 1) *p++ = '\n';
	}
	*p = '\0';
	*out_len = total;
	return buf;
}

/* Run a parallel edit: build the sorted set, apply `fn` at every
 * cursor, store the results.  Pushes a single full-buffer undo record
 * (UNDO_RECT_OVERWRITE restores rows and trims to the original row
 * count), wrapped in boundaries, so one C-_ undoes the whole stroke. */
typedef void (*mc_edit_fn)(int row, int col, int arg, int *orow, int *ocol);

static int mc_run(mc_edit_fn fn, int arg)
{
	int rows[MC_MAX + 1], cols[MC_MAX + 1];
	int primary_idx, n, i;
	int orig_numrows, snap_len;
	char *snap;

	if (editor_readonly_blocked())
		return 1;
	if (!mc_active())
		return 0;   /* not handled; caller falls back to single */

	/* Capture the pre-edit primary position BEFORE building the set, so
	 * undo returns the cursor there (not to the buffer origin). */
	int pre_primary_row = editor.rowoff + editor.cy;
	int pre_primary_col = editor.coloff + editor.cx;

	n = mc_build_sorted(rows, cols, &primary_idx);

	orig_numrows = editor.numrows;
	snap = mc_snapshot_buffer(&snap_len);

	/* Serialize the pre-edit secondary cursor set for the undo record. */
	char mcbuf[MC_MAX * 24];
	int mcbl = 0;
	mcbuf[0] = '\0';
	{
		int i;
		for (i = 0; i < editor.mc_count; i++)
			mcbl += snprintf(mcbuf + mcbl, sizeof(mcbuf) - mcbl,
					 "%d,%d ", editor.mc_row[i],
					 editor.mc_col[i]);
	}

	/* Group cursor-state + buffer-restore as ONE undo step: inhibit the
	 * auto-boundary that compound ops normally trigger, and manage the
	 * boundaries explicitly.  Push cursor-state first (deepest) so it is
	 * replayed LAST and the primary lands on its pre-edit position. */
	undo_push_boundary();
	undo_inhibit_autoboundary = 1;
	undo_push(UNDO_MC_CURSORS, pre_primary_row, pre_primary_col, 0,
		  mcbuf, mcbl);
	undo_push(UNDO_RECT_OVERWRITE, 0, 0, orig_numrows,
		  snap ? snap : (char *)"", snap_len);
	undo_inhibit_autoboundary = 0;
	free(snap);

	suppress_undo = 1;
	for (i = 0; i < n; i++) {
		mc_adj_kind = 0;
		fn(rows[i], cols[i], arg, &rows[i], &cols[i]);
		/* Fix up the results computed so far for this edit's effect. */
		mc_adjust_prior(rows, cols, i);
	}
	suppress_undo = 0;
	undo_push_boundary();

	mc_store_sorted(rows, cols, n, primary_idx);
	editor.dirty++;
	return 1;
}

/* --- public parallel commands (return 1 if handled) ------------------- */

int editor_mc_self_insert(int c)
{
	return mc_run(mc_insert_char_at, c);
}

int editor_mc_backspace(void)
{
	return mc_run(mc_backspace_at, 0);
}

int editor_mc_del_forward(void)
{
	return mc_run(mc_del_forward_at, 0);
}

int editor_mc_newline(void)
{
	return mc_run(mc_newline_at, 0);
}

/* Adapter for the two-arg string insert. */
static const char *mc_yank_text;
static int         mc_yank_len;
static void mc_yank_adapter(int row, int col, int arg, int *orow, int *ocol)
{
	(void)arg;
	mc_insert_text_at(row, col, mc_yank_text, mc_yank_len, orow, ocol);
}

int editor_mc_yank(void)
{
	char *text = kill_ring_get();

	if (!mc_active()) return 0;
	if (editor_readonly_blocked()) return 1;
	if (!text) {
		editor_set_status_message("Kill ring is empty");
		return 1;
	}
	mc_yank_text = text;
	mc_yank_len  = killring.len;
	/* No mark set under MC; each cursor ends after its inserted text. */
	mc_run(mc_yank_adapter, 0);
	editor_set_status_message("Yanked");
	return 1;
}

/* --- movement --------------------------------------------------------- */

/* Move one absolute position like editor_move_cursor but without any
 * window/scroll state.  Clamps to the target row's content. */
static void mc_move_one(int *row, int *col, int key)
{
	erow *r;

	if (*row < 0) *row = 0;
	if (*row >= editor.numrows) *row = editor.numrows - 1;
	if (*row < 0) return;
	r = &editor.row[*row];
	if (*col > r->size) *col = r->size;
	if (*col < 0) *col = 0;

	switch (key) {
	case ARROW_LEFT:
		if (*col > 0) {
			int n = 1, pos = *col - 1;
			while (pos > 0 && utf8_is_cont((unsigned char)r->chars[pos])) {
				n++; pos--;
			}
			*col -= n;
		} else if (*row > 0) {
			(*row)--;
			*col = editor.row[*row].size;
		}
		break;
	case ARROW_RIGHT:
		if (*col < r->size) {
			int n = 1, pos = *col + 1;
			while (pos < r->size && utf8_is_cont((unsigned char)r->chars[pos])) {
				n++; pos++;
			}
			*col += n;
		} else if (*row + 1 < editor.numrows) {
			(*row)++;
			*col = 0;
		}
		break;
	case ARROW_UP:
		if (*row > 0) {
			(*row)--;
			if (*col > editor.row[*row].size)
				*col = editor.row[*row].size;
		}
		break;
	case ARROW_DOWN:
		if (*row + 1 < editor.numrows) {
			(*row)++;
			if (*col > editor.row[*row].size)
				*col = editor.row[*row].size;
		}
		break;
	case HOME_KEY:
		*col = 0;
		break;
	case END_KEY:
		*col = r->size;
		break;
	}
	if (*col < 0) *col = 0;
}

int editor_mc_move(int key)
{
	int rows[MC_MAX + 1], cols[MC_MAX + 1];
	int primary_idx, n, i;

	if (!mc_active()) return 0;

	n = mc_build_sorted(rows, cols, &primary_idx);
	for (i = 0; i < n; i++)
		mc_move_one(&rows[i], &cols[i], key);
	mc_store_sorted(rows, cols, n, primary_idx);
	return 1;
}

/* --- creating cursors ------------------------------------------------- */

/* Furthest-along cursor position (primary or any secondary), used so
 * repeated add-below / mark-next walk forward instead of re-adding the
 * same line. */
static void mc_furthest(int *row, int *col)
{
	int i;

	*row = editor.rowoff + editor.cy;
	*col = editor.coloff + editor.cx;
	for (i = 0; i < editor.mc_count; i++) {
		if (mc_pos_cmp(editor.mc_row[i], editor.mc_col[i],
			       *row, *col) > 0) {
			*row = editor.mc_row[i];
			*col = editor.mc_col[i];
		}
	}
}

/* Highest (top-most) cursor position, for add-above. */
static void mc_topmost(int *row, int *col)
{
	int i;

	*row = editor.rowoff + editor.cy;
	*col = editor.coloff + editor.cx;
	for (i = 0; i < editor.mc_count; i++) {
		if (mc_pos_cmp(editor.mc_row[i], editor.mc_col[i],
			       *row, *col) < 0) {
			*row = editor.mc_row[i];
			*col = editor.mc_col[i];
		}
	}
}

/* C-c m c: drop a cursor at point and enter (or stay in) collection
 * mode.  In collection mode only navigation keys keep collecting; any
 * other key flips to edit mode (handled in kbd.c).  Pressing add while
 * editing drops back to collection so the user can reposition and add
 * more; editing resumes on the next non-navigation key. */
void editor_mc_add_here(void)
{
	if (mc_add_here_pos()) {
		editor.mc_editing = 0;   /* (back to) collection mode */
		editor_set_status_message("Cursor %d added (navigate, C-c m c to add, any edit key to begin)",
					  editor.mc_count);
	} else {
		editor_set_status_message("Already a cursor here");
	}
}

void editor_mc_cursor_below(void)
{
	int pr, pc;

	mc_furthest(&pr, &pc);
	if (pr + 1 < editor.numrows) {
		int col = pc;
		if (col > editor.row[pr+1].size) col = editor.row[pr+1].size;
		if (mc_add(pr + 1, col)) {
			editor.mc_editing = 1;   /* auto-select edits immediately */
			editor_set_status_message("MC:%d", editor.mc_count + 1);
		}
	}
}

void editor_mc_cursor_above(void)
{
	int pr, pc;

	mc_topmost(&pr, &pc);
	if (pr > 0) {
		int col = pc;
		if (col > editor.row[pr-1].size) col = editor.row[pr-1].size;
		if (mc_add(pr - 1, col)) {
			editor.mc_editing = 1;
			editor_set_status_message("MC:%d", editor.mc_count + 1);
		}
	}
}

/* Find the next occurrence of `word` after (from_row, from_col),
 * wrapping around the buffer, and add a cursor at its START.  Returns 1
 * if one was added. */
static int mc_mark_occurrence(const char *word, int wlen,
			      int from_row, int from_col, int all)
{
	int r, added = 0;
	int start_row = from_row;

	for (r = from_row; r < editor.numrows; r++) {
		erow *row = &editor.row[r];
		int c = (r == from_row) ? from_col + 1 : 0;
		while (c + wlen <= row->size) {
			if (memcmp(row->chars + c, word, wlen) == 0) {
				if (mc_add(r, c)) {
					added++;
					if (!all) return added;
				}
			}
			c++;
		}
	}
	/* Wrap to the top for the single (non-all) case. */
	if (!all && !added) {
		for (r = 0; r <= start_row && r < editor.numrows; r++) {
			erow *row = &editor.row[r];
			int c = 0;
			while (c + wlen <= row->size) {
				if (memcmp(row->chars + c, word, wlen) == 0) {
					if (mc_add(r, c))
						return 1;
				}
				c++;
			}
		}
	}
	return added;
}

void editor_mc_mark_next(void)
{
	char word[256];
	int wlen;
	int from_row, from_col;

	if (!editor_word_at_point(word, sizeof(word))) {
		editor_set_status_message("No word at point");
		return;
	}
	wlen = (int)strlen(word);

	/* Search from the furthest-along existing cursor so repeated C-c m n
	 * walks forward through the occurrences instead of re-finding the
	 * first one. */
	mc_furthest(&from_row, &from_col);

	if (mc_mark_occurrence(word, wlen, from_row, from_col, 0)) {
		editor.mc_editing = 1;
		editor_set_status_message("MC:%d", editor.mc_count + 1);
	} else {
		editor_set_status_message("No more occurrences");
	}
}

void editor_mc_mark_all(void)
{
	char word[256];
	int wlen, n;

	if (!editor_word_at_point(word, sizeof(word))) {
		editor_set_status_message("No word at point");
		return;
	}
	wlen = (int)strlen(word);
	/* Scan the whole buffer for every occurrence. */
	n = 0;
	{
		int r;
		for (r = 0; r < editor.numrows; r++) {
			erow *row = &editor.row[r];
			int c = 0;
			while (c + wlen <= row->size) {
				if (memcmp(row->chars + c, word, wlen) == 0) {
					if (mc_add(r, c))
						n++;
				}
				c++;
			}
		}
	}
	if (n > 0) {
		editor.mc_editing = 1;
		editor_set_status_message("MC:%d", editor.mc_count + 1);
	} else {
		editor_set_status_message("No other occurrences");
	}
}
