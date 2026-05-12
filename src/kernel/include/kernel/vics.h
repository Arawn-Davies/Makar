#ifndef _KERNEL_VICS_H
#define _KERNEL_VICS_H

#include <kernel/vesa_tty.h>

/*
 * vics.h - VICS interactive text editor for Makar.
 *
 * Key bindings:
 *   Arrow keys  - navigate
 *   Printable   - insert character at cursor
 *   Backspace   - delete character before cursor; join lines at column 0
 *   Enter       - split line at cursor
 *   Tab         - insert 4 spaces
 *   Ctrl+S      - save file
 *   Ctrl+Q      - quit (press twice to discard unsaved changes)
 *
 * Pass pane=NULL to use the default full-screen pane.
 */
void vics_edit(const char *path, vesa_pane_t *pane);

#endif /* _KERNEL_VICS_H */
