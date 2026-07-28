/* ====================== Vertical completion panel ======================
 *
 * Vertico-style candidate list shown in a bottom window while a minibuffer
 * picker (find-file, C-x b, M-x, C-x p f) is active.  The panel is a real
 * buffer slot drawn as an inactive window, so the minibuffer keeps focus:
 * the echo area still shows the prompt and the typed query, and the cursor
 * stays parked there.  Candidates are rendered one per row with the
 * selected row inverted (see draw_window_rows in display.c).
 *
 * The panel buffer rows are built directly in the slot (never via the live
 * editor globals) because the picker's underlying buffer must remain the
 * live one for the whole interaction.  On screens too small to split, the
 * pickers fall back to the horizontal echo-area picker.
 */

#include "def.h"

#define PICKER_BUF_NAME "*Completions*"

int picker_sel_row = -1;     /* file-row inverted in the panel; -1 = none */

static int picker_win = -1;  /* winlist index of the panel window */
static int picker_buf = -1;  /* buflist index of the *Completions* slot */

/* Build a plain-text erow (chars + render + hl all literal, no syntax)
 * directly into `b` at file-row `at`, without touching live editor state. */
static void picker_set_row(struct editor_buffer *b, int at, const char *s)
{
	erow *r;
	int len = (int)strlen(s);
	int j;

	if (at >= b->numrows) {
		b->row = realloc(b->row, sizeof(erow) * (at + 1));
		for (j = b->numrows; j <= at; j++)
			memset(&b->row[j], 0, sizeof(erow));
		b->numrows = at + 1;
	}
	r = &b->row[at];
	free(r->render);
	free(r->chars);
	free(r->hl);

	r->size  = len;
	r->chars = malloc(len + 1);
	memcpy(r->chars, s, len + 1);
	r->rsize  = len;
	r->render = malloc(len + 1);
	memcpy(r->render, s, len + 1);
	r->hl = malloc(len ? len : 1);
	memset(r->hl, HL_NORMAL, len);
	r->hl_oc = 0;
	r->idx   = at;
}

/* Drop all rows from the panel buffer slot. */
static void picker_clear_rows(struct editor_buffer *b)
{
	int i;
	for (i = 0; i < b->numrows; i++)
		editor_free_row(&b->row[i]);
	free(b->row);
	b->row = NULL;
	b->numrows = 0;
}

/* Find or create the background *Completions* buffer slot.  Returns the
 * buflist index, or -1 if no slot is free. */
static int picker_buf_slot(void)
{
	int i, free_slot = -1;

	for (i = 0; i < MAX_BUFFERS; i++) {
		if (buflist[i].active && buflist[i].filename &&
		    strcmp(buflist[i].filename, PICKER_BUF_NAME) == 0)
			return i;
		if (!buflist[i].active && free_slot < 0)
			free_slot = i;
	}
	if (free_slot < 0) return -1;

	memset(&buflist[free_slot], 0, sizeof(buflist[free_slot]));
	buflist[free_slot].filename = strdup(PICKER_BUF_NAME);
	buflist[free_slot].readonly = 1;
	buflist[free_slot].scratch  = 1;
	buflist[free_slot].active   = 1;
	return free_slot;
}

/* Open the completions panel: split a bottom window sized to the candidate
 * count (capped so the editing window stays usable) and point it at the
 * *Completions* buffer.  Returns the panel's winlist index, or -1 when the
 * screen can't spare the rows (caller falls back to horizontal display). */
int picker_panel_open(int nmatches)
{
	int panel_h, cur_buf;

	if (win_count >= MAX_WINDOWS) return -1;

	/* Height: one row per candidate, capped at ~40% of the screen and at
	 * least 3 rows; the editing window keeps the rest. */
	panel_h = nmatches > 0 ? nmatches : 1;
	if (panel_h > (win_total_rows * 2) / 5) panel_h = (win_total_rows * 2) / 5;
	if (panel_h < 3) panel_h = 3;

	picker_buf = picker_buf_slot();
	if (picker_buf < 0) return -1;

	cur_buf = buf_current;
	picker_win = win_split_bottom(panel_h);
	if (picker_win < 0) return -1;

	/* Point the new window at the completions buffer WITHOUT focusing it;
	 * minibuffer focus must stay in the original window.  win_split_bottom
	 * copied the current window (same bufidx), so reassign and reset its
	 * scroll/cursor to the top of the list. */
	winlist[picker_win].bufidx = picker_buf;
	winlist[picker_win].rowoff = 0;
	winlist[picker_win].coloff = 0;
	winlist[picker_win].cx     = 0;
	winlist[picker_win].cy     = 0;

	/* win_split_bottom called win_sync_view for the current window; the
	 * live buffer is unchanged (we never focused the panel). */
	buf_current = cur_buf;

	picker_sel_row = -1;
	return picker_win;
}

/* Rewrite the panel rows from `names` and invert `sel`.  Scrolls the panel
 * window so the selected row stays visible. */
void picker_panel_render(const char *const *names, int n, int sel)
{
	struct editor_buffer *b;
	struct editor_window *w;
	int i;

	if (picker_win < 0 || picker_buf < 0) return;
	if (!winlist[picker_win].active) return;

	b = &buflist[picker_buf];
	picker_clear_rows(b);

	if (n <= 0) {
		picker_set_row(b, 0, "[no match]");
		picker_sel_row = -1;
	} else {
		for (i = 0; i < n; i++)
			picker_set_row(b, i, names[i]);
		if (sel < 0) sel = 0;
		if (sel >= n) sel = n - 1;
		picker_sel_row = sel;
	}

	/* Keep the selected row visible in the panel's viewport. */
	w = &winlist[picker_win];
	if (picker_sel_row >= 0) {
		if (picker_sel_row < w->rowoff)
			w->rowoff = picker_sel_row;
		else if (picker_sel_row >= w->rowoff + w->h)
			w->rowoff = picker_sel_row - w->h + 1;
	} else {
		w->rowoff = 0;
	}
	if (w->rowoff < 0) w->rowoff = 0;
}

/* Tear down the panel window and free the completions rows.  The window the
 * picker was launched from keeps focus and re-expands to fill the screen. */
void picker_panel_close(void)
{
	if (picker_buf >= 0 && picker_buf < MAX_BUFFERS &&
	    buflist[picker_buf].active &&
	    buflist[picker_buf].filename &&
	    strcmp(buflist[picker_buf].filename, PICKER_BUF_NAME) == 0) {
		picker_clear_rows(&buflist[picker_buf]);
		free(buflist[picker_buf].filename);
		buflist[picker_buf].filename = NULL;
		buflist[picker_buf].active = 0;
	}
	picker_buf = -1;

	if (picker_win >= 0 && picker_win < MAX_WINDOWS &&
	    winlist[picker_win].active) {
		winlist[picker_win].active = 0;
		win_count--;
		/* Re-expand the focused window to reclaim the panel's rows. */
		win_fill_screen(&winlist[win_current]);
		win_sync_view();
	}
	picker_win = -1;
	picker_sel_row = -1;
}

/* Whether the panel window is currently up. */
int picker_panel_active(void)
{
	return picker_win >= 0 && picker_win < MAX_WINDOWS &&
	       winlist[picker_win].active;
}
