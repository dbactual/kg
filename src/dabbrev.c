/* ============================ dabbrev-expand =============================
 *
 * Dynamic abbreviation expansion on TAB — mirrors the Emacs behaviour the
 * user had: when point is just after a word prefix, TAB scans the buffer
 * for the nearest word that starts with that prefix and replaces the prefix
 * with the full word.  Repeated TAB presses cycle to the next candidate.
 * Any non-TAB key resets the cycle (see dabbrev_reset).
 *
 * Used by the TAB dispatch in kbd.c, which also handles region indent and
 * minibuffer/path completion before falling through to here.
 */

#include "def.h"
#include <ctype.h>
#include <string.h>

#define DA_MAX_CANDS 64
#define DA_MAX_WORD  128

/* Cycle state.  All empty/zero when no expansion is in progress. */
static char da_prefix[DA_MAX_WORD];
static int  da_prefix_len;
static int  da_prefix_row;   /* filerow where the prefix being expanded starts */
static int  da_prefix_col;   /* filecol where the prefix starts */
static char da_cands[DA_MAX_CANDS][DA_MAX_WORD];
static int  da_cand_len[DA_MAX_CANDS];
static int  da_ncands;
static int  da_idx;          /* current candidate index */

/* Reset the dabbrev cycle — called on any non-TAB key so a fresh press
 * starts a new expansion. */
void dabbrev_reset(void)
{
	da_prefix_len = 0;
	da_ncands = 0;
	da_idx = 0;
}

/* Is c a word character for dabbrev (alnum + _)? */
static int da_is_word(int c)
{
	return isalnum((unsigned char)c) || c == '_';
}

/* Extract the word prefix immediately before point into out (NUL-terminated),
 * return its length, and record the prefix's start row/col.  Returns 0 if
 * there is no word prefix before point (point is at BOL or the previous char
 * isn't a word char). */
static int dabbrev_prefix(char *out, int outsize)
{
	int filerow = editor.rowoff + editor.cy;
	int filecol = editor.coloff + editor.cx;
	erow *row;
	int end, start;

	if (filerow < 0 || filerow >= editor.numrows) return 0;
	row = &editor.row[filerow];
	if (filecol <= 0 || filecol > row->size) return 0;
	if (!da_is_word((unsigned char)row->chars[filecol - 1])) return 0;

	end = filecol;
	start = end;
	while (start > 0 && da_is_word((unsigned char)row->chars[start - 1]))
		start--;
	if (end - start <= 0) return 0;
	if (end - start >= outsize) return 0;

	memcpy(out, row->chars + start, end - start);
	out[end - start] = '\0';
	da_prefix_row = filerow;
	da_prefix_col = start;
	return end - start;
}

/* Add a candidate word to da_cands[] if it's not already present and it
 * isn't identical to the prefix.  Truncates to DA_MAX_WORD-1. */
static void da_add_cand(const char *word, int wlen, const char *prefix, int plen)
{
	int i;
	char tmp[DA_MAX_WORD];

	if (wlen <= 0) return;
	if (wlen == plen && memcmp(word, prefix, plen) == 0) return;  /* == prefix */
	if (wlen >= DA_MAX_WORD) wlen = DA_MAX_WORD - 1;
	memcpy(tmp, word, wlen);
	tmp[wlen] = '\0';

	for (i = 0; i < da_ncands; i++)
		if ((int)strlen(da_cands[i]) == wlen &&
		    memcmp(da_cands[i], tmp, wlen) == 0)
			return;  /* duplicate */
	if (da_ncands >= DA_MAX_CANDS) return;
	memcpy(da_cands[da_ncands], tmp, wlen);
	da_cands[da_ncands][wlen] = '\0';
	da_cand_len[da_ncands] = wlen;
	da_ncands++;
}

/* Scan the buffer for words starting with `prefix`, collecting candidates
 * in distance order from the cursor: rows above the cursor (nearest first,
 * walking up), then rows below (nearest first, walking down).  The current
 * row is scanned too, matches before point first then after. */
static void dabbrev_collect(const char *prefix, int plen)
{
	int cur = editor.rowoff + editor.cy;
	int cur_col = editor.coloff + editor.cx;
	int r, i, wstart;

	da_ncands = 0;

	/* Current row: words before point, then after point. */
	if (cur >= 0 && cur < editor.numrows) {
		erow *row = &editor.row[cur];
		/* before point (walk left-to-right, add in order) */
		i = 0;
		while (i < cur_col) {
			while (i < cur_col && !da_is_word((unsigned char)row->chars[i])) i++;
			wstart = i;
			while (i < cur_col && da_is_word((unsigned char)row->chars[i])) i++;
			if (i > wstart && i - wstart >= plen &&
			    memcmp(row->chars + wstart, prefix, plen) == 0)
				da_add_cand(row->chars + wstart, i - wstart, prefix, plen);
		}
		/* after point */
		i = cur_col;
		while (i < row->size) {
			while (i < row->size && !da_is_word((unsigned char)row->chars[i])) i++;
			wstart = i;
			while (i < row->size && da_is_word((unsigned char)row->chars[i])) i++;
			if (i > wstart && i - wstart >= plen &&
			    memcmp(row->chars + wstart, prefix, plen) == 0)
				da_add_cand(row->chars + wstart, i - wstart, prefix, plen);
		}
	}

	/* Rows above the cursor, nearest first. */
	for (r = cur - 1; r >= 0; r--) {
		erow *row = &editor.row[r];
		i = 0;
		while (i < row->size) {
			while (i < row->size && !da_is_word((unsigned char)row->chars[i])) i++;
			wstart = i;
			while (i < row->size && da_is_word((unsigned char)row->chars[i])) i++;
			if (i > wstart && i - wstart >= plen &&
			    memcmp(row->chars + wstart, prefix, plen) == 0)
				da_add_cand(row->chars + wstart, i - wstart, prefix, plen);
		}
	}

	/* Rows below the cursor, nearest first. */
	for (r = cur + 1; r < editor.numrows; r++) {
		erow *row = &editor.row[r];
		i = 0;
		while (i < row->size) {
			while (i < row->size && !da_is_word((unsigned char)row->chars[i])) i++;
			wstart = i;
			while (i < row->size && da_is_word((unsigned char)row->chars[i])) i++;
			if (i > wstart && i - wstart >= plen &&
			    memcmp(row->chars + wstart, prefix, plen) == 0)
				da_add_cand(row->chars + wstart, i - wstart, prefix, plen);
		}
	}
}

/* Replace the prefix at (da_prefix_row, da_prefix_col) of length
 * da_prefix_len with `text` (len chars).  Moves point to the end of the
 * inserted text. */
static void dabbrev_replace(const char *text, int len)
{
	int i;
	/* Delete the current prefix (or previous candidate) backwards. */
	for (i = 0; i < da_prefix_len; i++)
		editor_del_char();
	/* Insert the new candidate. */
	editor_insert_text_raw(text, len);
	/* Remember the new length so the next press can delete it again. */
	da_prefix_len = len;
}

/* The public entry point.  Returns 1 if an expansion happened (so the
 * caller knows TAB was consumed), 0 if there was no prefix to expand (so
 * the caller can fall back to inserting a literal tab). */
int dabbrev_expand(void)
{
	char prefix[DA_MAX_WORD];

	/* Fresh press (no active cycle): extract the prefix and collect. */
	if (da_prefix_len == 0) {
		int plen = dabbrev_prefix(prefix, sizeof prefix);
		if (plen <= 0) return 0;  /* no word before point */
		dabbrev_collect(prefix, plen);
		if (da_ncands == 0) {
			editor_set_status_message("No dabbrev expansion for '%s'", prefix);
			return 1;  /* consumed, but nothing to insert */
		}
		memcpy(da_prefix, prefix, plen);
		da_prefix[plen] = '\0';
		da_prefix_len = plen;   /* length of the prefix currently in the buffer */
		da_idx = 0;
	} else {
		/* Cycle to the next candidate.  The buffer currently holds
		 * da_cands[da_idx] (length da_prefix_len); delete it and insert
		 * the next candidate. */
		da_idx = (da_idx + 1) % da_ncands;
	}

	dabbrev_replace(da_cands[da_idx], da_cand_len[da_idx]);
	return 1;
}

/* Indent the active region by `n` columns: insert n spaces at the start of
 * each line in the region.  Keeps the region active (Emacs' "keeping
 * region" behaviour) so repeated TAB indents further.  No-op if no region. */
void editor_indent_rigidly(int n)
{
	int cur_row = editor.rowoff + editor.cy;
	int cur_col = editor.coloff + editor.cx;
	int r0, r1, r, i;
	char spaces[16];
	int outdent = (n < 0);
	int absn = outdent ? -n : n;

	if (!editor.mark_set || !editor.mark_highlight) return;
	if (absn <= 0 || absn > (int)sizeof(spaces) - 1) return;
	if (editor_readonly_blocked()) return;

	for (i = 0; i < absn; i++) spaces[i] = ' ';
	spaces[absn] = '\0';

	if (editor.mark_row < cur_row ||
	    (editor.mark_row == cur_row && editor.mark_col < cur_col)) {
		r0 = editor.mark_row; r1 = cur_row;
		/* The region end (point) at column 0 means no text is selected
		 * on that line — Emacs treats the region as [mark, point), so
		 * a point sitting at BOL excludes its line.  Don't indent it. */
		if (cur_col == 0 && r1 > r0) r1--;
	} else {
		r0 = cur_row; r1 = editor.mark_row;
		if (editor.mark_col == 0 && r1 > r0) r1--;
	}
	if (r0 < 0) r0 = 0;
	if (r1 >= editor.numrows) r1 = editor.numrows - 1;

	for (r = r0; r <= r1; r++) {
		erow *row = &editor.row[r];
		editor_cursor_goto(r, 0);
		if (!outdent) {
			/* Indent: insert absn spaces at column 0. */
			editor_insert_text_raw(spaces, absn);
		} else {
			/* De-indent: remove up to absn leading spaces.  Only
			 * spaces count (tabs are left alone), matching Emacs
			 * indent-rigidly with a negative arg. */
			int avail = 0;
			while (avail < absn && avail < row->size &&
			       row->chars[avail] == ' ')
				avail++;
			if (avail > 0) {
				int j;
				for (j = 0; j < avail; j++)
					editor_row_del_char(row, 0);
				editor.dirty++;
			}
		}
	}
	/* Adjust the mark and point columns only if their row was actually
	 * touched — a point sitting at BOL on the excluded (last) line
	 * should stay put, since nothing was inserted/deleted there. */
	if (editor.mark_row >= r0 && editor.mark_row <= r1) {
		if (!outdent) editor.mark_col += absn;
		else {
			int lead = 0;
			erow *mrow = &editor.row[editor.mark_row];
			while (lead < absn && lead < mrow->size &&
			       mrow->chars[lead] == ' ') lead++;
			if (editor.mark_col > lead) editor.mark_col -= lead;
			else editor.mark_col = 0;
		}
	}
	if (cur_row >= r0 && cur_row <= r1) {
		if (!outdent) cur_col += absn;
		else {
			int lead = 0;
			erow *crow = &editor.row[cur_row];
			while (lead < absn && lead < crow->size &&
			       crow->chars[lead] == ' ') lead++;
			if (cur_col > lead) cur_col -= lead;
			else cur_col = 0;
		}
	}
	editor_cursor_goto(cur_row, cur_col);
	/* Keep the region highlighted for further TAB/Shift-Tab presses. */
	editor.mark_highlight = 1;
	/* Tell the post-command deactivation logic to leave the region
	 * alone: indent-rigidly intentionally keeps it for repeated TAB. */
	editor.keep_region = 1;
	editor_set_status_message(outdent ? "Region de-indented" : "Region indented");
}
