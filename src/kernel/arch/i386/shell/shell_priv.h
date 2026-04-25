/*
 * shell_priv.h -- internal declarations shared between shell source files.
 *
 * Not part of the public kernel API; include only from shell*.c files.
 */

#ifndef SHELL_PRIV_H
#define SHELL_PRIV_H

#include <stddef.h>

#define SHELL_MAX_INPUT  256
#define SHELL_MAX_ARGS   8

/* Medli-compatible identity: username and hostname shown in the prompt. */
#define SHELL_USERNAME   "root"
#define SHELL_HOSTNAME   "makar"

/* Kernel version string (shared with cmd_version and the welcome banner). */
#define SHELL_VERSION    "0.1.0"

#define BUILD_DATE __DATE__
#define BUILD_TIME __TIME__
#define COPYRIGHT  "Copyright (C) 2026 Arawn Davies"

/*
 * Medli-compatible colour scheme: white text on blue background.
 *
 * SHELL_COLOR_VGA  – VGA attribute byte: fg=15 (WHITE) | bg=1 (BLUE) << 4
 * SHELL_FG_RGB     – 24-bit RGB for the VESA framebuffer (foreground)
 * SHELL_BG_RGB     – 24-bit RGB for the VESA framebuffer (background)
 * SHELL_ERROR_RGB_PAIR – VGA attr for error text: fg=12 (LIGHT_RED) | bg=1 (BLUE) << 4
 */
#define SHELL_COLOR_VGA      0x1F   /* make_color(COLOR_WHITE,      COLOR_BLUE) */
#define SHELL_ERROR_COLOR_VGA 0x1C  /* make_color(COLOR_LIGHT_RED,  COLOR_BLUE) */
#define SHELL_FG_RGB         0xFFFFFF
#define SHELL_BG_RGB         0x0000AA

/* shell.c – REPL core */
void shell_readline(char *buf, size_t max);

/* shell_help.c – help & version commands */
void cmd_help(void);
void cmd_version(void);

/* shell_cmds.c – all other built-in commands */
void cmd_clear(void);
void cmd_echo(int argc, char **argv);
void cmd_meminfo(void);
void cmd_uptime(void);
void cmd_tasks(void);
void cmd_lsdisks(void);
void cmd_lspart(int argc, char **argv);
void cmd_mkpart(int argc, char **argv);
void cmd_readsector(int argc, char **argv);
void cmd_setmode(int argc, char **argv);
void cmd_shutdown(void);
void cmd_reboot(void);

/* FAT32 commands */
void cmd_mount(int argc, char **argv);
void cmd_umount(void);
void cmd_ls(int argc, char **argv);
void cmd_cat(int argc, char **argv);
void cmd_cd(int argc, char **argv);
void cmd_mkdir(int argc, char **argv);
void cmd_mkfs(int argc, char **argv);

/* ISO9660 commands */
void cmd_isols(int argc, char **argv);

/* Installer */
void cmd_install(void);

/* VICS text editor */
void cmd_vics(int argc, char **argv);

/* Eject */
void cmd_eject(int argc, char **argv);

/* Ring-3 smoke test */
void cmd_ring3test(void);

#endif /* SHELL_PRIV_H */
