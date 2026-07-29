/* undo.c - Simple undo/redo functionality */

#include "def.h"
#include <time.h>

#define MAX_UNDO_SIZE 1000

/* Global undo stack */
struct undo_stack undostack = {NULL, 0, MAX_UNDO_SIZE, -1};

/* Timestamp of the last real (non-boundary) op pushed, for time-based
 * grouping.  0 means "no prior op". */
static time_t undo_last_time = 0;
/* The type-class of the last real op, for type-change boundaries. */
static int undo_last_class = -1;  /* 0=insert-char, 1=delete-char, 2=other */
/* When non-zero, undo_push skips the auto-boundary (caller groups ops). */
int undo_inhibit_autoboundary = 0;

/* Classify an op into a grouping class.  Consecutive ops of the same
 * class within the time threshold are collapsed into one undo step.
 *   UNDO_INSERT_CHAR -> 0 (insert)
 *   UNDO_DELETE_CHAR -> 1 (delete)
 *   everything else  -> 2 (compound: always its own group)
 */
static int undo_class(enum undo_type type)
{
	switch (type) {
	case UNDO_INSERT_CHAR: return 0;
	case UNDO_DELETE_CHAR: return 1;
	default:               return 2;
	}
}

/* Push an explicit undo boundary.  Compound ops (kill, yank, reflow,
 * rectangle) call this before pushing their record so they always form
 * their own undo step, separate from any surrounding char-level edits. */
void undo_push_boundary(void)
{
	struct undo_op *op;

	if (suppress_undo) return;
	/* Don't push two boundaries in a row. */
	if (undostack.head && undostack.head->type == UNDO_BOUNDARY) return;

	op = malloc(sizeof(struct undo_op));
	if (!op) return;
	op->type = UNDO_BOUNDARY;
	op->row = op->col = op->c = 0;
	op->text = NULL;
	op->len = 0;
	op->next = undostack.head;
	undostack.head = op;
	undostack.size++;
}

/* Initialize the undo stack */
void undo_init(void)
{
	undostack.head = NULL;
	undostack.size = 0;
	undostack.max_size = MAX_UNDO_SIZE;
	undostack.clean_size = -1;  /* -1 means never saved clean */
	undo_last_time = 0;
	undo_last_class = -1;
}

/* Free the entire undo stack */
void undo_free(void)
{
	struct undo_op *op = undostack.head;

	while (op) {
		struct undo_op *next = op->next;
		if (op->text) free(op->text);
		free(op);
		op = next;
	}
	undostack.head = NULL;
	undostack.size = 0;
	undo_last_time = 0;
	undo_last_class = -1;
}

/* Push an undo operation onto the stack.  Automatically inserts an
 * undo boundary before the op when the grouping class changes (insert
 * vs delete vs compound) or when more than UNDO_GROUP_SECS seconds have
 * elapsed since the last op -- mirroring Emacs' undo grouping. */
#define UNDO_GROUP_SECS 5

void undo_push(enum undo_type type, int row, int col, int c, char *text, int len)
{
	struct undo_op *op;

	/* Skip if undo recording is suppressed */
	if (suppress_undo) return;

	/* Auto-boundary: compound ops always start a new group; char ops
	 * start a new group when the class changes or time has elapsed.
	 * A caller that pushes several compound ops as ONE logical group
	 * (multiple-cursor edit: cursor-state + buffer-restore) sets
	 * undo_inhibit_autoboundary and manages boundaries itself. */
	{
		int cls = undo_class(type);
		time_t now = time(NULL);

		if (undo_inhibit_autoboundary) {
			/* Caller manages grouping; skip the auto-boundary. */
		} else if (cls == 2) {
			/* Compound op: always boundary before. */
			undo_push_boundary();
		} else if (undo_last_class >= 0 && undo_last_class != cls) {
			/* Type changed (insert <-> delete): boundary. */
			undo_push_boundary();
		} else if (undo_last_time > 0 && now - undo_last_time > UNDO_GROUP_SECS) {
			/* Time gap: boundary. */
			undo_push_boundary();
		}
		undo_last_class = cls;
		undo_last_time = now;
	}

	/* Create new undo operation */
	op = malloc(sizeof(struct undo_op));
	if (!op) return;

	op->type = type;
	op->row = row;
	op->col = col;
	op->c = c;
	op->text = NULL;
	op->len = 0;

	/* Copy text if provided */
	if (text && len > 0) {
		op->text = malloc(len + 1);
		if (op->text) {
			memcpy(op->text, text, len);
			op->text[len] = '\0';
			op->len = len;
		}
	}

	/* Add to front of stack */
	op->next = undostack.head;
	undostack.head = op;
	undostack.size++;

	/* Trim stack if too large */
	if (undostack.size > undostack.max_size) {
		struct undo_op *curr = undostack.head;
		struct undo_op *prev = NULL;
		int count = 0;

		/* Find the last operation to keep */
		while (curr && count < undostack.max_size - 1) {
			prev = curr;
			curr = curr->next;
			count++;
		}

		/* Free the rest */
		if (prev) {
			prev->next = NULL;
			while (curr) {
				struct undo_op *next = curr->next;
				if (curr->text) free(curr->text);
				free(curr);
				curr = next;
				undostack.size--;
			}
		}
	}
}

/* Perform undo operation */
void editor_undo(void)
{
	struct undo_op *op;

	if (editor_readonly_blocked())
		return;

	if (!undostack.head) {
		editor_set_status_message("Nothing to undo");
		return;
	}

	/* If the top of the stack is a boundary (e.g. left over from a
	 * compound op), pop it first -- it doesn't represent work to undo. */
	if (undostack.head->type == UNDO_BOUNDARY) {
		op = undostack.head;
		undostack.head = op->next;
		undostack.size--;
		free(op);
		if (!undostack.head) {
			editor_set_status_message("Nothing to undo");
			return;
		}
	}

	/* Pop and apply ops until we hit a boundary (or empty stack).
	 * One C-_ undoes one whole group. */
	do {
		op = undostack.head;
		undostack.head = op->next;
		undostack.size--;

	/* Position cursor at operation location */
	editor_cursor_goto(op->row, op->col);

	/* Perform the reverse operation */
	switch (op->type) {
	case UNDO_BOUNDARY:
		/* Should not be reached: boundaries are popped before the
		 * apply loop and at its end, never applied. */
		break;
	case UNDO_INSERT_CHAR:
		/* Reverse: delete the character */
		if (op->row < editor.numrows) {
			erow *row = &editor.row[op->row];
			if (op->col < row->size) {
				editor_row_del_char(row, op->col);
				editor.dirty++;
			}
		}
		break;

	case UNDO_DELETE_CHAR:
		/* Reverse: insert the character */
		if (op->row < editor.numrows) {
			erow *row = &editor.row[op->row];
			editor_row_insert_char(row, op->col, op->c);
			editor.dirty++;
		}
		break;

	case UNDO_INSERT_LINE:
		/* Reverse: delete the line */
		if (op->row < editor.numrows) {
			editor_del_row(op->row);
			editor.dirty++;
		}
		break;

	case UNDO_DELETE_LINE:
		/* Reverse: insert the line */
		if (op->text) {
			editor_insert_row(op->row, op->text, op->len);
			editor.dirty++;
		}
		break;

	case UNDO_SPLIT_LINE:
		/* Reverse: truncate row at split point, append saved rest, delete row+1.
		 * Using saved op->text rather than live row+1 content because row+1 may
		 * have an auto-indent prefix that was not part of the original text. */
		if (op->row < editor.numrows) {
			erow *row = &editor.row[op->row];
			int col = op->col;

			/* A stale column past the row would truncate outside the
			 * allocation. */
			if (col < 0) col = 0;
			if (col > row->size) col = row->size;
			row->size = col;
			row->chars[col] = '\0';
			if (op->text && op->len > 0)
				editor_row_append_string(row, op->text, op->len);
			else
				editor_update_row(row);
			if (op->row + 1 < editor.numrows)
				editor_del_row(op->row + 1);
			editor.dirty++;
		}
		break;

	case UNDO_JOIN_LINE:
		/* Reverse: split the line.  The joined-away row may have been
		 * empty (op->text NULL, op->len 0 -- undo_push stores no payload
		 * for a zero-length string), so re-insert it as "". */
		if (op->row < editor.numrows) {
			erow *row;
			int col = op->col;

			/* Insert new line after current; this realloc's editor.row,
			 * so fetch the row pointer afterwards. */
			editor_insert_row(op->row + 1, op->text ? op->text : "", op->len);
			row = &editor.row[op->row];
			if (col < 0) col = 0;
			if (col > row->size) col = row->size;
			row->size = col;
			row->chars[col] = '\0';
			editor_update_row(row);
			editor.dirty++;
		}
		break;

	case UNDO_KILL_TEXT:
		/* Reverse: re-insert the killed text at the original position.
		 * Cursor is already set to (op->row, op->col) above. */
		if (op->text && op->len > 0)
			editor_insert_text_raw(op->text, op->len);
		break;

	case UNDO_YANK_TEXT:
		/* Reverse: delete the yanked text forward from (op->row, op->col).
		 * Cursor is already set to (op->row, op->col) above. */
		if (op->text && op->len > 0) {
			int i;
			suppress_undo = 1;
			for (i = 0; i < op->len; i++)
				editor_del_forward_char();
			suppress_undo = 0;
		}
		break;

	case UNDO_YANK_POP:
		/* Reverse: delete the new text (op->c chars forward), then
		 * re-insert the old text (op->text, op->len).  Single undo
		 * step restores the previous yank. */
		if (op->row < editor.numrows) {
			if (op->c > 0) {
				int i;
				suppress_undo = 1;
				for (i = 0; i < op->c; i++)
					editor_del_forward_char();
				suppress_undo = 0;
			}
			if (op->text && op->len > 0)
				editor_insert_text_raw(op->text, op->len);
		}
		break;

	case UNDO_RECT_OVERWRITE: {
		/* op->row = first row affected
		 * op->c   = numrows before the operation
		 * op->text = original content of rows [row, row+N), '\n'-joined,
		 *            where N = lines in op->text (0 if empty).
		 * Replay: trim back to original numrows, then restore each row. */
		int orig_numrows = op->c;
		char *p = op->text;
		char *end = op->text ? op->text + op->len : NULL;
		int i = 0;

		suppress_undo = 1;
		while (editor.numrows > orig_numrows)
			editor_del_row(editor.numrows - 1);
		if (op->text && op->len > 0) {
			while (p <= end) {
				char *nl = (p < end) ? memchr(p, '\n', end - p) : NULL;
				int line_len = nl ? (nl - p) : (end - p);
				int target = op->row + i;

				if (target < editor.numrows)
					editor_del_row(target);
				editor_insert_row(target, p, line_len);
				if (!nl) break;
				p = nl + 1;
				i++;
			}
		}
		suppress_undo = 0;
		editor.dirty++;
		break;
	}

	case UNDO_MC_CURSORS: {
		/* op->row/op->col = pre-edit primary position; op->text = the
		 * pre-edit secondary cursors serialized as "r,c r,c ...".
		 * Restore the cursor set so multiple-cursor editing continues
		 * where it left off.  (editor_undo already did cursor_goto for
		 * the primary via op->row/op->col.) */
		editor.mc_count = 0;
		if (op->text && op->len > 0) {
			char *p = op->text;
			char *end = op->text + op->len;
			while (p < end && editor.mc_count < MC_MAX) {
				int rr = 0, cc = 0;
				while (p < end && *p >= '0' && *p <= '9')
					rr = rr * 10 + (*p++ - '0');
				if (p < end && *p == ',') p++;
				while (p < end && *p >= '0' && *p <= '9')
					cc = cc * 10 + (*p++ - '0');
				if (p < end && *p == ' ') p++;
				editor.mc_row[editor.mc_count] = rr;
				editor.mc_col[editor.mc_count] = cc;
				editor.mc_count++;
			}
		}
		/* Clamp restored positions to the (restored) buffer. */
		{
			int i;
			for (i = 0; i < editor.mc_count; i++) {
				if (editor.mc_row[i] >= editor.numrows)
					editor.mc_row[i] = editor.numrows - 1;
				if (editor.mc_row[i] < 0) editor.mc_row[i] = 0;
				if (editor.numrows > 0 &&
				    editor.mc_col[i] > editor.row[editor.mc_row[i]].size)
					editor.mc_col[i] = editor.row[editor.mc_row[i]].size;
			}
		}
		break;
	}

	case UNDO_REFLOW_PARA: {
		/* op->row = paragraph start row
		 * op->col = number of reflowed rows to delete
		 * op->text = original lines joined with '\n' */
		char *line_start, *nl, *end;
		int r;

		suppress_undo = 1;
		for (r = 0; r < op->col; r++) {
			if (op->row < editor.numrows)
				editor_del_row(op->row);
		}
		if (op->text) {
			r = op->row;
			line_start = op->text;
			end = op->text + op->len;
			while (line_start < end) {
				nl = memchr(line_start, '\n', end - line_start);
				if (nl) {
					editor_insert_row(r++, line_start, nl - line_start);
					line_start = nl + 1;
				} else {
					editor_insert_row(r++, line_start, end - line_start);
					break;
				}
			}
		}
		suppress_undo = 0;
		editor.dirty++;
		break;
	}
	}

	/* Free the operation */
	if (op->text) free(op->text);
	free(op);

	/* Continue popping until we hit a boundary or empty stack. */
	op = undostack.head;
	} while (op && op->type != UNDO_BOUNDARY);

	/* If we stopped at a boundary, pop it too (it's consumed). */
	if (op && op->type == UNDO_BOUNDARY) {
		undostack.head = op->next;
		undostack.size--;
		free(op);
	}

	/* Check if we've undone back to the saved state */
	if (undostack.size == undostack.clean_size)
		editor.dirty = 0;

	editor_set_status_message("Undo");
}

/* Mark current state as clean (called after save) */
void undo_mark_clean(void)
{
	undostack.clean_size = undostack.size;
}
