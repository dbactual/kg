/* stubs_noyank.c — globals and stubs for test binaries that link yank.o.
 * Omits killring (defined in yank.c) and kill_ring_* stubs (ditto). */

#include <stdarg.h>
#include "../src/def.h"

/* Globals normally defined in main.c */
struct editor_config editor;
int running       = 1;
int suppress_undo = 0;

/* Globals normally defined in bufmgr.c */
struct editor_buffer buflist[MAX_BUFFERS];
int buf_current = 0;
int buf_count   = 0;
int global_auto_revert = 0;
int require_final_newline = 0;
int kg_bg_dark = -1;

/* Globals normally defined in winmgr.c */
struct editor_window winlist[MAX_WINDOWS];
int win_current    = 0;
int win_count      = 0;
int win_total_rows = 24;
int win_total_cols = 80;

/* No-op stub for display function not under test */
void editor_set_status_message(const char *fmt, ...) { (void)fmt; }

/* Weak stub for shell function referenced by yank.c copy/kill paths.
 * shell.o provides the real implementation when linked (test_shell). */
__attribute__((weak))
void copy_to_clipboard(const char *text, int len) { (void)text; (void)len; }

/* Stub for the minibuffer prompt used by rect.c editor_string_rect().
 * Tests drive string-rectangle's edit path with a pre-set string, so
 * the prompt just returns an empty string (the delete path). */
int editor_read_line(int fd, const char *prompt, char *buf, int bufsize)
{
	(void)fd; (void)prompt;
	if (bufsize > 0) buf[0] = '\0';
	return 0;
}
