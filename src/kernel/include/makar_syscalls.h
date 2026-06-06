#ifndef _MAKAR_SYSCALLS_H
#define _MAKAR_SYSCALLS_H

/*
 * makar_syscalls.h -- the canonical syscall-number ABI, shared verbatim by the
 * kernel (<kernel/syscall.h>) and userspace (src/userspace/syscall.h) so the two
 * never drift.  Linux-uapi style: the kernel tree owns the contract, userspace
 * consumes it (the userspace build adds -I../kernel/include; this header is
 * staged to /usr/include for in-OS tcc + the kernel rebuild).  Numbers ONLY --
 * no types, so it includes nothing and works under -nostdinc.
 */
/*
 * Syscall numbers - Linux i386 ABI subset.
 *
 * Registers (int 0x80 calling convention):
 *   EAX = syscall number
 *   EBX = arg1, ECX = arg2, EDX = arg3
 *   Return value written back to EAX (negative errno on error).
 */
#define SYS_EXIT       1    /* void exit(int status)                            */
#define SYS_FORK       2    /* pid_t fork(void) -- COW clone of caller          */
#define SYS_READ       3    /* ssize_t read(int fd, void *buf, size_t len)      */
#define SYS_WRITE      4    /* ssize_t write(int fd, const void *buf, size_t)   */
#define SYS_OPEN       5    /* int open(const char *path, int flags)            */
#define SYS_CLOSE      6    /* int close(int fd)                                */
#define SYS_UNLINK     10   /* int unlink(const char *path)  -- alias of 208    */
#define SYS_EXECVE     11   /* int execve(const char *path, char *const argv[], char *const envp[]) */
#define SYS_CHDIR      12   /* int chdir(const char *path) -- set calling task's cwd */
#define SYS_LSEEK      19   /* off_t lseek(int fd, off_t offset, int whence)   */
#define SYS_GETPID     20   /* pid_t getpid(void)                               */
#define SYS_DUP        41   /* int dup(int oldfd) -- lowest free fd, Linux i386 */
#define SYS_PIPE       42   /* int pipe(int pipefd[2]) -- alloc reader+writer  */
#define SYS_DUP2       63   /* int dup2(int oldfd, int newfd) -- Linux i386 ABI*/
#define SYS_WAIT4      114  /* pid_t wait4(pid, int *status, int options, void *rusage) */
#define SYS_KILL       37   /* int kill(int pid, int signo)                     */
#define SYS_RENAME     38   /* int rename(const char *old, const char *new)     */
#define SYS_MKDIR      39   /* int mkdir(const char *path, mode_t mode)         */
#define SYS_RMDIR      40   /* int rmdir(const char *path)                      */
#define SYS_BRK        45   /* void *brk(void *addr)                            */
#define SYS_MUNMAP     91   /* int munmap(void *addr, size_t len)               */
#define SYS_MMAP2     192   /* void *mmap2(addr,len,prot,flags,fd,pgoff) -- anon only */
/* musl/Linux process-startup syscalls (hosted toolchain bring-up) */
#define SYS_IOCTL          54   /* int ioctl(fd, req, ...) -- stub (-ENOTTY)        */
#define SYS_WRITEV        146   /* ssize_t writev(fd, const struct iovec*, int)     */
#define SYS_RT_SIGPROCMASK 175  /* int rt_sigprocmask(how, set, oldset, sigsetsize) -- stub */
#define SYS_FUTEX         240   /* int futex(...) -- no-op (single-threaded) */
#define SYS_SET_THREAD_AREA 243 /* int set_thread_area(struct user_desc*) -- TLS    */
#define SYS_EXIT_GROUP    252   /* void exit_group(int status) -- == exit           */
#define SYS_SET_TID_ADDRESS 258 /* int set_tid_address(int *tidptr) -> tid          */
#define SYS_LOGOUT          260 /* int logout(void) - end root login session        */
#define SYS_GUI_CLOSE       261 /* int gui_close(void) - return to root text mode   */
#define SYS_LOGIN           266 /* int login(const char *user, const char *pass)    */
#define SYS_SIGNAL     48   /* sig_handler_t signal(int signo, sig_handler_t)   */
#define SYS_STAT      106   /* int stat(const char *path, struct stat *st)      */
#define SYS_FSTAT     108   /* int fstat(int fd, struct stat *st)               */
#define SYS_READDIR   141   /* int readdir(path, idx, struct dirent *)          */
#define SYS_SIGRETURN  119  /* void sigreturn(void) -- not for direct use      */
#define SYS_DEBUG      100  /* void debug(uint32_t cp)      [Makar ext]         */
#define SYS_YIELD      158  /* void sched_yield(void)                           */
/* Makar display/input extensions (200+) */
#define SYS_GETKEY     200  /* int getkey(void)  - raw single-char keyboard     */
#define SYS_PUTCH_AT   201  /* int putch_at(tty_cell_t*, uint32_t n)            */
#define SYS_SET_CURSOR 202  /* void set_cursor(uint32_t col, uint32_t row)      */
#define SYS_TTY_CLEAR  203  /* void tty_clear(uint8_t clr)                      */
#define SYS_TERM_SIZE  204  /* uint32_t term_size() → (cols<<16)|rows           */
#define SYS_WRITE_FILE 205  /* int write_file(path, buf, len)                   */
#define SYS_LS_DIR     206  /* int ls_dir(path, buf, bufsz) → bytes written     */
#define SYS_DISK_INFO    207  /* int disk_info(buf, bufsz) → bytes written        */
#define SYS_DELETE_FILE  208  /* int delete_file(path)                            */
#define SYS_RENAME_FILE  209  /* int rename_file(old_path, new_path)              */
#define SYS_DELETE_DIR   210  /* int delete_dir(path)                             */
#define SYS_WRITE_SERIAL 211  /* ssize_t write_serial(const void *buf, size_t len)*/
#define SYS_KEYBOARD_RAW 212  /* void keyboard_raw(int on)   - see keyboard.h     */
#define SYS_SHELL_CLEAR  213  /* void shell_clear(void)     - same as `clear`     */
#define SYS_UPTIME       214  /* uint32_t uptime_ticks(void) - 100 Hz timer ticks */
#define SYS_GETCWD       215  /* int getcwd(char *buf, size_t size) - copy task cwd */
#define SYS_FB_INFO      216  /* uint32_t fb_info(void) - VESA only; 0 if VGA-only.
                                * Returns (width << 16) | height when available. */
#define SYS_DRAW_LINE    217  /* int draw_line(uint32_t xy0, uint32_t xy1,
                                *               uint32_t rgb)
                                * Bresenham line in framebuffer pixels.  Coords
                                * packed (x << 16) | y.  Returns 0 on success,
                                * (uint32_t)-1 if pixel mode unavailable.
                                * Clipped to drawable area (status row excluded). */
#define SYS_CARET_STYLE  218  /* uint32_t caret_style(uint32_t style) - set the
                                * VESA caret style (0=line, 2=flashing block),
                                * returns the previous style.  No-op (returns 0)
                                * in VGA-text mode.  Used by vix.elf. */
#define SYS_MOUSE_READ   256  /* uint32_t mouse_read(void) - pop one PS/2 mouse
                                * event; 0 if none.  Packed: bit31=valid,
                                * bits0-2 buttons (L/R/M), bits8-15 dx int8,
                                * bits16-23 dy int8 (+y down). */
#define SYS_FB_PRESENT   257  /* int fb_present(const void *backbuf) - blit a
                                * width*height*32bpp (pitch=width*4) user back
                                * buffer full-frame to the framebuffer.  0 ok,
                                * -1 if no pixel FB / not focused. */
#define SYS_FB_PRESENT_RECT 269 /* int fb_present_rect(buf, (x<<16)|y, (w<<16)|h):
                                * blit only that sub-rect of the full-frame back
                                * buffer.  Lets the WM update the cursor without
                                * re-pushing the whole frame.  0 ok, -1 as above. */

/* Shared pixel surfaces (kernel/surface.h): the one shared-memory primitive.
 * A window manager creates a surface, a forked graphical child maps the same
 * physical frames by id, both draw into it; the WM composites.  See
 * docs/syscalls.md. */
#define SYS_SURFACE_CREATE  259 /* int surface_create(int w, int h) -> id / -1   */
#define SYS_SURFACE_MAP     262 /* void *surface_map(int id) -> addr / NULL       */
#define SYS_SURFACE_INFO    263 /* uint32_t surface_info(int id) -> (w<<16)|h /-1 */
#define SYS_SURFACE_DESTROY 264 /* int surface_destroy(int id) -> 0 / -1          */
#define SYS_SURFACE_UNMAP   268 /* int surface_unmap(int id) -> 0/-1 (resize realloc) */

/*
 * Admin syscalls (219..229).
 *
 * Each gates on task_is_admin() (always true today; future login model
 * tightens this).  Side-effect semantics + arg validation live in
 * kernel/admin.h helpers; the dispatcher is a thin marshaller.
 *
 * String args are read directly from the calling task's address space
 * (the caller's PD is loaded when int 0x80 runs, so a kernel-mode
 * dereference reaches user memory).
 *
 * Return:
 *   >= 0  success or current state (e.g. SYS_SCHED_QUANTUM returns ticks)
 *   -1    permission denied OR generic failure
 *   <-1   admin-helper-specific negative errno
 */
#define SYS_REBOOT         219  /* int reboot(void) - noreturn on success */
#define SYS_SHUTDOWN       220  /* int shutdown(void) - noreturn on success */
#define SYS_SETMODE        221  /* int setmode(const char *mode) */
#define SYS_FGCOL          222  /* int fgcol(const char *colour) */
#define SYS_BGCOL          223  /* int bgcol(const char *colour) */
#define SYS_EJECT          224  /* int eject(void) */
#define SYS_MOUNT          225  /* int mount(const char *dev, const char *mnt) */
#define SYS_UMOUNT         226  /* int umount(const char *target) -- NULL ok */
#define SYS_MKFS           227  /* int mkfs(const char *dev, const char *fstype) */
#define SYS_SCHED_QUANTUM  228  /* int sched_quantum(int new_value) -- <0 = query */
#define SYS_VERBOSE        229  /* int verbose(int onoff) -- 1/0/-1 */
#define SYS_SHELL_READY    231  /* void shell_ready_marker(void) - emit the
                                 * `[shell:ready vt=N]` sync marker on COM1
                                 * if g_serial_verbose; no-op otherwise.
                                 * Userspace shell calls this before each
                                 * prompt so the in-guest test drivers'
                                 * serial sync keeps working unchanged. */
#define SYS_CURSOR_POS     232  /* uint32_t cursor_pos(void) -> (col<<16)|row */
#define SYS_VT_ENTER       233  /* int vt_enter(int focus_new) - attach this task
                                 * as a makmux shell on the next VT slot. */
#define SYS_VT_CLOSE       234  /* int vt_close(pid_t pid) - close task's VT */
#define SYS_VT_OPEN_REQUEST 235 /* int vt_open_request(void) - consume Alt+T */
#define SYS_VT_STATE       236 /* uint32_t vt_state(void) -> active<<16 | mask */
#define SYS_VT_CLOCK_REQUEST 237 /* int vt_clock_request(void) - consume Alt+F5 */
#define SYS_PCI_INFO         239 /* int pci_info(char *buf, uint32_t bufsz) - render
                                  * pci_devices[] as text.  Returns bytes written. */
#define SYS_INSTALL          238 /* int install(void) - run the Limine installer TUI;
                                  * blocks the calling task until the installer
                                  * exits.  Returns 0 on success, -1 otherwise. */
#define SYS_GETHOSTNAME    230  /* int gethostname(char *buf, size_t size) - read
                                 * /etc/hostname (or fallback "makar"), copy up
                                 * to size-1 bytes, NUL-terminate.  Returns
                                 * strlen on success, -1 if buf invalid. */
#define SYS_WHOAMI         247  /* int whoami(char *buf, size_t size) - copy the
                                 * current session's username (auth_current_user,
                                 * "user" on live boots) up to size-1 bytes,
                                 * NUL-terminate.  Returns strlen, -1 if invalid. */
#define SYS_STATUSBAR      241  /* int statusbar(int cmd) - reserve/free the bottom
                                 * status row (mechanism for userspace statusbar.elf).
                                 * cmd: 1=enable (reserve), 0=disable (free), <0=query.
                                 * Returns the resulting enabled state (0/1). */
#define SYS_VT_OPEN_APP    242  /* int vt_open_app(const char *path) - open an app
                                 * in a named tab: switch to it if it exists, else
                                 * queue for makmux to spawn.  Returns 1/0. */
#define SYS_VT_TAKE_APP    248  /* int vt_take_app(char *buf, int cap) - makmux
                                 * drains one queued app path.  Returns 1/0. */
#define SYS_VT_SETNAME     244  /* int vt_setname(const char *name) - name the
                                 * calling task's VT tab (shown in the status bar). */
#define SYS_VT_GETNAME     245  /* int vt_getname(int slot, char *buf, int cap) -
                                 * copy slot's tab name (empty for unnamed VT
                                 * shells).  Returns strlen.  Used by statusbar. */
#define SYS_STATFS         246  /* int statfs(uint32_t *total_kb, uint32_t *free_kb)
                                 * rootfs usage in KiB (ext2 only).  Returns 0/-1. */

/* Microkernel IPC (MINIX-style synchronous message passing).  ebx = endpoint
 * pid (or IPC_ANY for recv), ecx = ipc_msg_t*.  See kernel/ipc.h. */
#define SYS_IPC_SEND       249  /* int ipc_send(int dst, const ipc_msg_t *)        */
#define SYS_IPC_RECV       250  /* int ipc_recv(int from, ipc_msg_t *)             */
#define SYS_IPC_SENDREC    251  /* int ipc_sendrec(int dst, ipc_msg_t *)           */
#define SYS_IPC_NBRECV     267  /* int ipc_nbrecv(int from, ipc_msg_t *) -> 0/-EAGAIN */
#define SYS_NET_INFO       253  /* int net_info(char *buf, uint32_t bufsz)         */
#define SYS_NET_CTL        254  /* int net_ctl(int cmd)                            */
#define SYS_WGET           255  /* int wget(const char *url, const char *outpath)
                                 * -> bytes saved, or negative on error           */

/* Back to Linux i386 ABI numbers for the next set. */
#define SYS_FCNTL      55   /* int fcntl(int fd, int cmd, int arg) - F_GETFL/F_SETFL */
#define SYS_GETPPID    64   /* pid_t getppid(void)                              */
#define SYS_GETTIMEOFDAY 78 /* int gettimeofday(struct timeval *, void *)       */
#define SYS_CLOCK_GETTIME 265 /* int clock_gettime(clockid_t, struct timespec *)*/

#endif /* _MAKAR_SYSCALLS_H */
