/* login.c -- login screen and session management (not yet implemented).
 *
 * Triggered when sh.elf --login receives `exit` or Ctrl-D.
 * Displays a full-screen login prompt (ported from Medli's Login.cs),
 * reads username + masked password, verifies via shadow.c, then either
 * re-enters mak.sh0 as the authenticated user or loops on failure.
 */
#include "auth.h"

void login_screen(void)
{
    /* TODO:
     *   1. vesa_tty_clear() + draw header (white on blue, "Makar login")
     *   2. prompt "Username: " → read via keyboard_getchar loop
     *   3. prompt "Password: " → masked read (echo '*' or nothing)
     *   4. shadow_verify(username, password)
     *   5. on success: set current_uid, update task name, return
     *   6. on failure: brief delay + re-prompt (max 3 attempts then halt)
     */
}
