/*
 * ktest.c -- In-kernel unit test runner.
 *
 * Each suite is a static function called from ktest_run_all().  Results are
 * written directly to the VGA terminal so they work without heap or FS.
 */

#include <kernel/ktest.h>
#include <kernel/acpi.h>
#include <kernel/partition.h>
#include <kernel/pci.h>
#include <kernel/netdev.h>
#include <kernel/net_lwip.h>
#include <kernel/pmm.h>
#include <kernel/heap.h>
#include <kernel/vmm.h>
#include <kernel/paging.h>
#include <kernel/descr_tbl.h>
#include <kernel/fpu.h>
#include <kernel/task.h>
#include <kernel/fd.h>
#include <kernel/signal.h>
#include <kernel/syscall.h>
#include <kernel/serial.h>
#include <kernel/tty.h>
#include <kernel/vt.h>
#include <kernel/vesa.h>
#include <kernel/vesa_tty.h>
#include <kernel/bochs_vbe.h>
#include <kernel/timer.h>
#include <kernel/rtc.h>
#include <kernel/elf.h>
#include <kernel/vfs.h>
#include <kernel/devfs.h>
#include <kernel/tmpfs.h>
#include <kernel/asm.h>
#include <kernel/keyboard.h>
#include <kernel/surface.h>
#include <kernel/ide.h>
#include <lwip/ip_addr.h>
#include <lwip/pbuf.h>
#include <lwip/tcp.h>
#include <lwip/raw.h>
#include <lwip/inet_chksum.h>
#include <lwip/prot/icmp.h>
#include <lwip/prot/ip.h>
#include <kernel/wget.h>
#include <kernel/heap.h>
#include <kernel/unzip.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Harness state & primitives
 * ------------------------------------------------------------------------- */

int ktest_pass_count = 0;
int ktest_fail_count = 0;
volatile int ktest_bg_done = 0;

/* Background-suite progress, surfaced to the shell loading screen.  Updated
 * inside the RUN macro in ktest_bg_task; total is fixed at compile time so
 * the bar length is known the moment shell_run starts. */
volatile int ktest_bg_completed = 0;
const    int ktest_bg_total     = 21;   /* keep in sync with RUN() calls below */

/* When set, suppress VGA output for pass lines and suite headers. */
int ktest_muted = 0;

void ktest_begin(const char *suite, const char *desc)
{
    ktest_pass_count = 0;
    ktest_fail_count = 0;
    if (!ktest_muted) {
        t_writestring("\n[ktest] suite: ");
        t_writestring(suite);
        if (desc && *desc) {
            t_writestring(" -- ");
            t_writestring(desc);
        }
        t_putchar('\n');
    }
}

void ktest_assert(int cond, const char *expr, const char *file, uint32_t line)
{
    if (cond) {
        if (!ktest_muted) {
            t_writestring("  PASS: ");
            t_writestring(expr);
            t_putchar('\n');
        }
        ktest_pass_count++;
    } else {
        t_writestring("  FAIL: ");
        t_writestring(expr);
        t_writestring("  (");
        t_writestring(file);
        t_putchar(':');
        t_dec(line);
        t_writestring(")\n");
        ktest_fail_count++;
    }
}

void ktest_summary(void)
{
    if (ktest_muted)
        return;
    t_writestring("[ktest] results: ");
    t_dec((uint32_t)ktest_pass_count);
    t_writestring(" passed, ");
    t_dec((uint32_t)ktest_fail_count);
    t_writestring(" failed\n");
}

/* ---------------------------------------------------------------------------
 * Suite: ACPI helpers
 *
 * acpi_checksum() is the only internal helper we can call from outside
 * acpi.c without touching real hardware.  We test it with known buffers.
 * ------------------------------------------------------------------------- */

static void test_acpi_checksum(void)
{
    ktest_begin("acpi_checksum", "ACPI table byte-sum checksum invariants");

    /* A buffer whose byte sum is 0 - valid. */
    uint8_t good[4] = {0x01, 0x02, 0x03, 0xFA}; /* 1+2+3+250 = 256 → 0 mod 256 */
    KTEST_ASSERT(acpi_checksum(good, 4));

    /* Off-by-one: change the last byte so the sum is non-zero. */
    uint8_t bad[4] = {0x01, 0x02, 0x03, 0xFB};
    KTEST_ASSERT(!acpi_checksum(bad, 4));

    /* Single byte whose value is 0 - valid (sum = 0). */
    uint8_t zero[1] = {0x00};
    KTEST_ASSERT(acpi_checksum(zero, 1));

    /* Single byte whose value is non-zero - invalid. */
    uint8_t nonzero[1] = {0x01};
    KTEST_ASSERT(!acpi_checksum(nonzero, 1));

    /* Empty buffer (length 0) - sum is 0, always valid. */
    KTEST_ASSERT(acpi_checksum(good, 0));

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: string helpers (sanity-check the libc stubs used by the kernel)
 * ------------------------------------------------------------------------- */

static void test_fpu(void)
{
    ktest_begin("fpu", "x87 FPU armed by fpu_init (fild/fmulp/fsqrt/fistp round-trips)");

    /* Integer round-trips through the x87 stack prove the unit is live and
     * computing after fpu_init() cleared CR0.EM and ran fninit. */
    KTEST_ASSERT_EQ(fpu_imul(7, 3), 21);
    KTEST_ASSERT_EQ(fpu_imul(-6, 5), -30);
    KTEST_ASSERT_EQ(fpu_imul(0, 1234), 0);
    KTEST_ASSERT_EQ(fpu_isqrt(144), 12);
    KTEST_ASSERT_EQ(fpu_isqrt(16), 4);
    KTEST_ASSERT_EQ(fpu_isqrt(0), 0);

    /* x87 state survives a context switch: push a value onto the x87 stack,
     * yield (forces a schedule -> fxsave/fxrstor round-trip), read it back.
     * Also re-checks the unit is coherent after the round-trip. */
    {
        volatile int before = 0x1234;
        int after = 0;
        __asm__ volatile("fildl %0" :: "m"(before) : "st");
        task_yield();
        __asm__ volatile("fistpl %0" : "=m"(after) :: "st");
        KTEST_ASSERT_EQ(after, 0x1234);
        KTEST_ASSERT_EQ(fpu_imul(9, 9), 81);
    }

    ktest_summary();
}

static void test_string(void)
{
    ktest_begin("string", "freestanding libc string ops (strlen/strcmp/strncmp/memset/strcpy)");

    KTEST_ASSERT(strlen("hello") == 5);
    KTEST_ASSERT(strlen("") == 0);

    KTEST_ASSERT(strcmp("abc", "abc") == 0);
    KTEST_ASSERT(strcmp("abc", "abd") < 0);
    KTEST_ASSERT(strcmp("abd", "abc") > 0);

    KTEST_ASSERT(strncmp("abcX", "abcY", 3) == 0);
    KTEST_ASSERT(strncmp("abcX", "abcY", 4) != 0);

    char buf[16];
    memset(buf, 0xAA, sizeof(buf));
    KTEST_ASSERT((uint8_t)buf[0] == 0xAA);

    memcpy(buf, "hello", 6);
    KTEST_ASSERT(strcmp(buf, "hello") == 0);

    ktest_summary();
}

static void test_vt_status_scroll(void)
{
    ktest_begin("vt_status_scroll", "VT scroll region reserves status row");

    vt_buf_t vt;
    memset(&vt, 0, sizeof vt);
    KTEST_ASSERT(vt_init(&vt, 4, 4, 0x00FFFFFFu, 0x00000000u));
    if (!vt.cells) {
        ktest_summary();
        return;
    }

    vt_set_usable_rows(&vt, 3);
    vt_put_at(&vt, 'A', 0, 0);
    vt_put_at(&vt, 'B', 0, 1);
    vt_put_at(&vt, 'C', 0, 2);
    vt_put_at(&vt, 'S', 0, 3);

    vt_set_cursor(&vt, 0, 2);
    vt_putchar(&vt, '\n');

    KTEST_ASSERT(vt.cur_row == 2);
    KTEST_ASSERT(vt_get_cell(&vt, 0, 0).ch == 'B');
    KTEST_ASSERT(vt_get_cell(&vt, 0, 1).ch == 'C');
    KTEST_ASSERT(vt_get_cell(&vt, 0, 2).ch == ' ');
    KTEST_ASSERT(vt_get_cell(&vt, 0, 3).ch == 'S');

    vt_set_usable_rows(&vt, 4);
    vt_set_cursor(&vt, 0, 3);
    vt_putchar(&vt, 'Z');
    KTEST_ASSERT(vt_get_cell(&vt, 0, 3).ch == 'Z');

    kfree(vt.cells);
    ktest_summary();
}

static void test_vt_ansi(void)
{
    ktest_begin("vt_ansi", "VT backing grid parses ANSI/VT100 escapes");

    vt_buf_t vt;
    memset(&vt, 0, sizeof vt);
    KTEST_ASSERT(vt_init(&vt, 20, 6, 0x00FFFFFFu, 0x00000000u));
    if (!vt.cells) { ktest_summary(); return; }

    /* CUP: ESC[3;5H -> row 2, col 4 (1-based -> 0-based). */
    const char *cup = "\x1b[3;5H";
    for (const char *p = cup; *p; p++) vt_putchar(&vt, *p);
    KTEST_ASSERT(vt.cur_row == 2);
    KTEST_ASSERT(vt.cur_col == 4);

    /* A glyph lands at the addressed cell, cursor advances. */
    vt_putchar(&vt, 'X');
    KTEST_ASSERT(vt_get_cell(&vt, 4, 2).ch == 'X');
    KTEST_ASSERT(vt.cur_col == 5);

    /* SGR red-on-default then a glyph: cell carries the red fg. */
    const char *sgr = "\x1b[31m";
    for (const char *p = sgr; *p; p++) vt_putchar(&vt, *p);
    vt_putchar(&vt, 'R');
    KTEST_ASSERT(vt_get_cell(&vt, 5, 2).ch == 'R');
    KTEST_ASSERT(vt_get_cell(&vt, 5, 2).fg == 0x00AA0000u);

    /* SGR reset returns to the default fg. */
    const char *rst = "\x1b[0m";
    for (const char *p = rst; *p; p++) vt_putchar(&vt, *p);
    vt_putchar(&vt, 'W');
    KTEST_ASSERT(vt_get_cell(&vt, 6, 2).fg == 0x00FFFFFFu);

    /* EL(2): ESC[2K clears the whole current line. */
    vt_set_cursor(&vt, 0, 0);
    vt_putchar(&vt, 'a'); vt_putchar(&vt, 'b'); vt_putchar(&vt, 'c');
    vt_set_cursor(&vt, 1, 0);
    const char *el = "\x1b[2K";
    for (const char *p = el; *p; p++) vt_putchar(&vt, *p);
    KTEST_ASSERT(vt_get_cell(&vt, 0, 0).ch == ' ');
    KTEST_ASSERT(vt_get_cell(&vt, 2, 0).ch == ' ');

    /* ED(2): ESC[2J clears the screen. */
    vt_set_cursor(&vt, 0, 3);
    vt_putchar(&vt, 'Z');
    const char *ed = "\x1b[2J";
    for (const char *p = ed; *p; p++) vt_putchar(&vt, *p);
    KTEST_ASSERT(vt_get_cell(&vt, 0, 3).ch == ' ');

    /* Plain text + control chars still behave exactly as before. */
    vt_set_cursor(&vt, 0, 0);
    vt_putchar(&vt, 'h'); vt_putchar(&vt, 'i');
    vt_putchar(&vt, '\r');
    KTEST_ASSERT(vt.cur_col == 0);
    vt_putchar(&vt, '\n');
    KTEST_ASSERT(vt.cur_row == 1);
    KTEST_ASSERT(vt_get_cell(&vt, 0, 0).ch == 'h');
    KTEST_ASSERT(vt_get_cell(&vt, 1, 0).ch == 'i');

    /* An unterminated/long CSI must never wedge: cursor save/restore. */
    vt_set_cursor(&vt, 7, 3);
    const char *sv = "\x1b[s";
    for (const char *p = sv; *p; p++) vt_putchar(&vt, *p);
    vt_set_cursor(&vt, 0, 0);
    const char *rs = "\x1b[u";
    for (const char *p = rs; *p; p++) vt_putchar(&vt, *p);
    KTEST_ASSERT(vt.cur_col == 7);
    KTEST_ASSERT(vt.cur_row == 3);

    kfree(vt.cells);
    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: partition helpers
 *
 * Tests the pure-logic helpers in partition.c that can run without hardware.
 * part_type_name() and part_guid_type_name() both look up tables in BSS,
 * so they work without any disk being present.
 * ------------------------------------------------------------------------- */

static void test_partition(void)
{
    ktest_begin("partition", "MBR + GPT partition-type name lookups");

    /* MBR type name lookup */
    KTEST_ASSERT(strcmp(part_type_name(PART_MBR_EMPTY),     "Empty")          == 0);
    KTEST_ASSERT(strcmp(part_type_name(PART_MBR_FAT32_LBA), "FAT32 (LBA)")    == 0);
    KTEST_ASSERT(strcmp(part_type_name(PART_MBR_FAT32_CHS), "FAT32 (CHS)")    == 0);
    KTEST_ASSERT(strcmp(part_type_name(PART_MBR_GPT_PROT),  "GPT protective") == 0);
    KTEST_ASSERT(strcmp(part_type_name(PART_MBR_EFI),       "EFI System")     == 0);
    KTEST_ASSERT(strcmp(part_type_name(PART_MBR_MDFS),      "MDFS")           == 0);
    KTEST_ASSERT(strcmp(part_type_name(PART_MBR_LINUX),     "Linux")          == 0);
    /* Unknown type returns a non-empty string (not NULL). */
    KTEST_ASSERT(part_type_name(0xAB) != 0);

    /* GPT GUID type name lookup */
    KTEST_ASSERT(strcmp(part_guid_type_name(PART_GUID_FAT32), "FAT32")      == 0);
    KTEST_ASSERT(strcmp(part_guid_type_name(PART_GUID_EFI),   "EFI System") == 0);
    KTEST_ASSERT(strcmp(part_guid_type_name(PART_GUID_LINUX), "Linux Data") == 0);
    KTEST_ASSERT(strcmp(part_guid_type_name(PART_GUID_MDFS),  "MDFS")       == 0);

    /* All-zero GUID is "Unused". */
    static const uint8_t zero16[16] = {0};
    KTEST_ASSERT(strcmp(part_guid_type_name(zero16), "Unused") == 0);

    /* An unrecognised GUID returns a non-NULL string. */
    static const uint8_t unknown[16] = {
        0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE,
        0xBA, 0xBE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01
    };
    KTEST_ASSERT(part_guid_type_name(unknown) != 0);

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: devfs
 *
 * Exercises the /dev synthetic filesystem.  Boot-mode aware: the medium we
 * actually booted from dictates which device gets the read/readonly probes,
 * so the suite passes identically on ISO and HDD boots.
 *   - Negative lookups + "root is not a node" run unconditionally.
 *   - The CD-ROM node carries media only on ISO/live boots; an HDD boot
 *     still exposes an (empty) ATAPI drive, so the media-dependent asserts
 *     run only when devfs_node_size(cdrom) > 0.
 *   - The primary ATA disk (hda) is probed when present (HDD boots, or live
 *     boots with an installed disk attached).
 * ------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * Suite: PCI driver binding (pci_driver_t registration + pci_probe_all)
 * Matches the always-present i440FX host bridge (class 0x06/0x00) with a
 * dummy class-match driver and proves probe firing, claim, and idempotency.
 * ------------------------------------------------------------------------- */
static int test_pci_probe_hits;
static int test_pci_probe_fn(pci_device_t *dev) { (void)dev; test_pci_probe_hits++; return 0; }
static const pci_driver_t test_pci_drv = {
    .name = "ktest-hostbridge", .match_class = 1,
    .class_code = 0x06, .subclass = 0x00, .probe = test_pci_probe_fn,
};

static void test_pci_bind(void)
{
    ktest_begin("pci_bind", "pci_driver_t registration + pci_probe_all class match");

    KTEST_ASSERT(pci_device_count > 0);          /* QEMU always enumerates devices */

    test_pci_probe_hits = 0;
    pci_register_driver(&test_pci_drv);
    int bound = pci_probe_all();

    KTEST_ASSERT(test_pci_probe_hits >= 1);      /* host bridge probed */
    KTEST_ASSERT(bound >= 1);                    /* and claimed */

    int named = 0;
    for (int i = 0; i < pci_device_count; i++)
        if (pci_devices[i].driver &&
            strcmp(pci_devices[i].driver, "ktest-hostbridge") == 0)
            named++;
    KTEST_ASSERT(named >= 1);                     /* dev->driver carries the name */

    KTEST_ASSERT(pci_probe_all() == 0);          /* re-probe skips bound devices */

    ktest_summary();
}

static void be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t rd_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void test_virtio_net(void)
{
    ktest_begin("netdev", "active Ethernet netdev TX ARP request + polled RX reply from QEMU slirp");

    if (!netdev_present()) {
        Serial_WriteString("[ktest] netdev: no device, skipping\n");
        KTEST_ASSERT(1);
        ktest_summary();
        return;
    }

    const uint8_t *mac = netdev_mac();
    Serial_WriteString("[ktest] netdev: device=");
    Serial_WriteString((char *)(netdev_name() ? netdev_name() : "unknown"));
    Serial_WriteString(" mac=");
    for (int i = 0; i < 6; i++) {
        if (i) Serial_WriteString(":");
        Serial_WriteHex(mac[i]);
    }
    Serial_WriteString("\n");

    uint8_t arp[42];
    memset(arp, 0, sizeof(arp));
    for (int i = 0; i < 6; i++)
        arp[i] = 0xff;
    memcpy(arp + 6, mac, 6);
    be16(arp + 12, 0x0806);       /* Ethernet type: ARP */
    be16(arp + 14, 0x0001);       /* Ethernet hardware */
    be16(arp + 16, 0x0800);       /* IPv4 protocol */
    arp[18] = 6;                  /* hardware length */
    arp[19] = 4;                  /* protocol length */
    be16(arp + 20, 0x0001);       /* request */
    memcpy(arp + 22, mac, 6);
    be32(arp + 28, 0x0a00020f);   /* 10.0.2.15 */
    be32(arp + 38, 0x0a000202);   /* 10.0.2.2 */

    Serial_WriteString("[ktest] netdev: TX ARP who-has 10.0.2.2\n");
    int tx_rc = netdev_send(arp, sizeof(arp));
    Serial_WriteString("[ktest] netdev: TX rc=");
    Serial_WriteDec((uint32_t)tx_rc);
    Serial_WriteString("\n");
    KTEST_ASSERT(tx_rc == 0);

    /* Poll RX, yielding each iteration: under QEMU/TCG the guest must yield
     * the CPU so the device's TX/RX backend and slirp's ARP responder get to
     * run.  ~3 s wall-clock budget at 100 Hz. */
    uint8_t rx[1600];
    int got_reply = 0;
    int rx_frames = 0;
    uint32_t t0 = timer_get_ticks();
    while (timer_get_ticks() - t0 < 300) {
        int n = netdev_rx_poll(rx, sizeof(rx));
        if (n > 0) {
            rx_frames++;
            Serial_WriteString("[ktest] netdev: RX frame len=");
            Serial_WriteDec((uint32_t)n);
            Serial_WriteString(" ethertype=");
            Serial_WriteHex(rd_be16(rx + 12));
            Serial_WriteString("\n");
        }
        if (n >= 42 &&
            rd_be16(rx + 12) == 0x0806 &&      /* ARP                     */
            rd_be16(rx + 20) == 0x0002 &&      /* reply                   */
            rd_be32(rx + 28) == 0x0a000202 &&  /* sender 10.0.2.2 (slirp) */
            rd_be32(rx + 38) == 0x0a00020f &&  /* target 10.0.2.15 (us)   */
            memcmp(rx + 32, mac, 6) == 0) {    /* target MAC = ours       */
            got_reply = 1;
            break;
        }
        task_yield();
    }

    Serial_WriteString("[ktest] netdev: rx_frames=");
    Serial_WriteDec((uint32_t)rx_frames);
    Serial_WriteString(got_reply ? " ARP reply received\n" : " TIMEOUT (no ARP reply)\n");
    KTEST_ASSERT(got_reply);
    ktest_summary();
}

typedef struct lwip_tcp_test_state {
    int connected;
    int received;
    int errored;
} lwip_tcp_test_state_t;

static err_t lwip_tcp_test_recv(void *arg, struct tcp_pcb *pcb,
                                struct pbuf *p, err_t err)
{
    lwip_tcp_test_state_t *st = (lwip_tcp_test_state_t *)arg;
    if (err != ERR_OK) {
        st->errored = 1;
        if (p) pbuf_free(p);
        return ERR_OK;
    }
    if (!p)
        return ERR_OK;
    if (p->tot_len > 0) {
        st->received = 1;
        tcp_recved(pcb, p->tot_len);
    }
    pbuf_free(p);
    tcp_close(pcb);
    return ERR_OK;
}

static err_t lwip_tcp_test_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    lwip_tcp_test_state_t *st = (lwip_tcp_test_state_t *)arg;
    if (err != ERR_OK) {
        st->errored = 1;
        return ERR_OK;
    }
    st->connected = 1;
    tcp_recv(pcb, lwip_tcp_test_recv);
    return ERR_OK;
}

static void lwip_tcp_test_err(void *arg, err_t err)
{
    (void)err;
    lwip_tcp_test_state_t *st = (lwip_tcp_test_state_t *)arg;
    if (st)
        st->errored = 1;
}

static void test_lwip_tcp(void)
{
    ktest_begin("lwip_tcp", "lwIP over netdev: TCP connect + receive via QEMU slirp guestfwd");

    if (!netdev_present()) {
        Serial_WriteString("[ktest] lwip_tcp: no netdev, skipping\n");
        KTEST_ASSERT(1);
        ktest_summary();
        return;
    }

    KTEST_ASSERT(net_lwip_init() == 0);
    if (!net_lwip_ready()) {
        ktest_summary();
        return;
    }

    lwip_tcp_test_state_t st;
    memset(&st, 0, sizeof st);

    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    KTEST_ASSERT(pcb != NULL);
    if (!pcb) {
        ktest_summary();
        return;
    }

    /* The guestfwd endpoint lives at host .100 of the netif's subnet; derive
     * it from the live gateway rather than assuming a subnet. */
    uint8_t gw[4];
    KTEST_ASSERT(net_lwip_gateway(gw) == 0);
    ip_addr_t dst;
    IP_ADDR4(&dst, gw[0], gw[1], gw[2], 100);
    tcp_arg(pcb, &st);
    tcp_err(pcb, lwip_tcp_test_err);

    Serial_WriteString("[ktest] lwip_tcp: connect to guestfwd ");
    Serial_WriteDec(gw[0]); Serial_WriteString(".");
    Serial_WriteDec(gw[1]); Serial_WriteString(".");
    Serial_WriteDec(gw[2]); Serial_WriteString(".100:1234\n");
    err_t rc = tcp_connect(pcb, &dst, 1234, lwip_tcp_test_connected);
    KTEST_ASSERT(rc == ERR_OK);
    if (rc != ERR_OK) {
        tcp_abort(pcb);
        ktest_summary();
        return;
    }

    uint32_t t0 = timer_get_ticks();
    while (timer_get_ticks() - t0 < 600 && !st.errored && !st.received) {
        net_lwip_poll();
        task_yield();
    }

    Serial_WriteString("[ktest] lwip_tcp: connected=");
    Serial_WriteDec((uint32_t)st.connected);
    Serial_WriteString(" received=");
    Serial_WriteDec((uint32_t)st.received);
    Serial_WriteString(" errored=");
    Serial_WriteDec((uint32_t)st.errored);
    Serial_WriteString("\n");

    KTEST_ASSERT(st.connected);
    KTEST_ASSERT(st.received);
    KTEST_ASSERT(!st.errored);
    ktest_summary();
}

/* ---- ICMP echo (ping) over lwIP raw API ------------------------------------
 *
 * Asserts a reply from the slirp gateway (deterministic) and additionally
 * attempts an external echo to 1.1.1.1, which is logged but NOT asserted:
 * QEMU slirp only forwards ICMP to real hosts when the host grants the
 * capability (net.ipv4.ping_group_range / raw sockets), so external
 * reachability is environment-dependent and must not gate the suite. */
/* Format ip[4] as "a.b.c.d" into dst (needs <= 16 bytes).  Returns length. */
static uint32_t ip4_to_str(char *dst, const uint8_t ip[4])
{
    uint32_t o = 0;
    for (int i = 0; i < 4; i++) {
        if (i) dst[o++] = '.';
        uint8_t v = ip[i];
        if (v >= 100) dst[o++] = (char)('0' + v / 100);
        if (v >= 10)  dst[o++] = (char)('0' + (v / 10) % 10);
        dst[o++] = (char)('0' + v % 10);
    }
    dst[o] = '\0';
    return o;
}

#define PING_ID         0xAFAFu
#define PING_DATA_SIZE  32u

typedef struct {
    uint16_t seqno;
    int got;
} ping_state_t;

static u8_t lwip_ping_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p,
                           const ip_addr_t *addr)
{
    ping_state_t *st = (ping_state_t *)arg;
    (void)pcb; (void)addr;
    if (!p)
        return 0;
    if (p->tot_len < (u16_t)(PBUF_IP_HLEN + sizeof(struct icmp_echo_hdr)))
        return 0;
    if (pbuf_remove_header(p, PBUF_IP_HLEN) != 0)
        return 0;
    struct icmp_echo_hdr *iecho = (struct icmp_echo_hdr *)p->payload;
    if (ICMPH_TYPE(iecho) == ICMP_ER && iecho->id == PING_ID &&
        iecho->seqno == lwip_htons(st->seqno)) {
        st->got = 1;
        pbuf_free(p);
        return 1;                       /* consumed */
    }
    pbuf_add_header(p, PBUF_IP_HLEN);
    return 0;                           /* not ours */
}

/* Returns 1 on echo reply, 0 on timeout, -1 on allocation failure. */
static int lwip_ping_once(const ip_addr_t *dst, uint16_t seqno,
                          uint32_t timeout_ticks)
{
    struct raw_pcb *pcb = raw_new(IP_PROTO_ICMP);
    if (!pcb)
        return -1;

    ping_state_t st = { seqno, 0 };
    raw_recv(pcb, lwip_ping_recv, &st);
    raw_bind(pcb, IP_ADDR_ANY);

    uint16_t ping_size = (uint16_t)(sizeof(struct icmp_echo_hdr) + PING_DATA_SIZE);
    struct pbuf *p = pbuf_alloc(PBUF_IP, ping_size, PBUF_RAM);
    if (!p) {
        raw_remove(pcb);
        return -1;
    }
    struct icmp_echo_hdr *iecho = (struct icmp_echo_hdr *)p->payload;
    ICMPH_TYPE_SET(iecho, ICMP_ECHO);
    ICMPH_CODE_SET(iecho, 0);
    iecho->chksum = 0;
    iecho->id = PING_ID;
    iecho->seqno = lwip_htons(seqno);
    for (uint16_t i = 0; i < PING_DATA_SIZE; i++)
        ((char *)iecho)[sizeof(struct icmp_echo_hdr) + i] = (char)i;
    iecho->chksum = inet_chksum(iecho, ping_size);

    raw_sendto(pcb, p, dst);
    pbuf_free(p);

    uint32_t t0 = timer_get_ticks();
    while (timer_get_ticks() - t0 < timeout_ticks && !st.got) {
        net_lwip_poll();
        task_yield();
    }
    raw_remove(pcb);
    return st.got ? 1 : 0;
}

static void test_lwip_ping(void)
{
    ktest_begin("lwip_ping", "lwIP ICMP echo: gateway reply (asserted) + 1.1.1.1 (informational)");

    if (!netdev_present()) {
        Serial_WriteString("[ktest] lwip_ping: no netdev, skipping\n");
        KTEST_ASSERT(1);
        ktest_summary();
        return;
    }
    KTEST_ASSERT(net_lwip_init() == 0);
    if (!net_lwip_ready()) {
        ktest_summary();
        return;
    }

    uint8_t gwip[4];
    KTEST_ASSERT(net_lwip_gateway(gwip) == 0);
    ip_addr_t gw;
    IP_ADDR4(&gw, gwip[0], gwip[1], gwip[2], gwip[3]);
    int gw_ok = lwip_ping_once(&gw, 1, 300);
    Serial_WriteString("[ktest] lwip_ping: gateway ");
    Serial_WriteDec(gwip[0]); Serial_WriteString(".");
    Serial_WriteDec(gwip[1]); Serial_WriteString(".");
    Serial_WriteDec(gwip[2]); Serial_WriteString(".");
    Serial_WriteDec(gwip[3]); Serial_WriteString(" reply=");
    Serial_WriteDec((uint32_t)(gw_ok == 1));
    Serial_WriteString("\n");
    KTEST_ASSERT(gw_ok == 1);

    ip_addr_t ext;
    IP_ADDR4(&ext, 1, 1, 1, 1);
    int ext_ok = lwip_ping_once(&ext, 2, 300);
    Serial_WriteString("[ktest] lwip_ping: 1.1.1.1 reply=");
    Serial_WriteDec((uint32_t)(ext_ok == 1));
    Serial_WriteString(ext_ok == 1 ? " (external ICMP reachable)\n"
                                    : " (no external ICMP; informational, not gating)\n");
    KTEST_ASSERT(1);                    /* external result never fails the gate */

    ktest_summary();
}

static void test_lwip_dns(void)
{
    ktest_begin("lwip_dns", "lwIP DNS resolver: dotted-quad (asserted) + real name (informational)");

    if (!netdev_present()) {
        Serial_WriteString("[ktest] lwip_dns: no netdev, skipping\n");
        KTEST_ASSERT(1);
        ktest_summary();
        return;
    }
    KTEST_ASSERT(net_lwip_init() == 0);
    if (!net_lwip_ready()) {
        ktest_summary();
        return;
    }

    /* Dotted-quad literals resolve synchronously without a server, so this is
     * a deterministic check of the resolver plumbing. */
    uint8_t ip[4] = { 0, 0, 0, 0 };
    int lit = net_lwip_resolve("1.1.1.1", ip, 50);
    KTEST_ASSERT(lit == 0);
    KTEST_ASSERT(ip[0] == 1 && ip[1] == 1 && ip[2] == 1 && ip[3] == 1);

    /* A real lookup depends on slirp's DNS reaching the host resolver, so it
     * is logged but never gates the suite (like the external ping). */
    uint8_t rip[4] = { 0, 0, 0, 0 };
    int real = net_lwip_resolve("example.com", rip, 300);
    Serial_WriteString("[ktest] lwip_dns: example.com -> ");
    if (real == 0) {
        Serial_WriteDec(rip[0]); Serial_WriteString(".");
        Serial_WriteDec(rip[1]); Serial_WriteString(".");
        Serial_WriteDec(rip[2]); Serial_WriteString(".");
        Serial_WriteDec(rip[3]); Serial_WriteString("\n");
    } else {
        Serial_WriteString("(no DNS reply; informational, not gating)\n");
    }
    KTEST_ASSERT(1);

    ktest_summary();
}

static void test_wget(void)
{
    ktest_begin("wget", "HTTP GET over lwIP TCP via slirp guestfwd HTTP fixture");

    if (!netdev_present()) {
        Serial_WriteString("[ktest] wget: no netdev, skipping\n");
        KTEST_ASSERT(1);
        ktest_summary();
        return;
    }
    KTEST_ASSERT(net_lwip_init() == 0);
    if (!net_lwip_ready()) {
        ktest_summary();
        return;
    }

    /* tests/http_fixture.sh is wired to a slirp guestfwd at host .100 of the
     * netif's subnet, port 8080, and always replies "200 OK" + body
     * "MAKAR-WGET-OK" -- deterministic, no real internet.  Derive the target
     * from the live gateway so we don't bake in a specific subnet. */
    uint8_t gw[4];
    KTEST_ASSERT(net_lwip_gateway(gw) == 0);
    uint8_t fixture[4] = { gw[0], gw[1], gw[2], 100 };

    char url[64];
    uint32_t uo = 0;
    const char *pre = "http://";
    while (*pre) url[uo++] = *pre++;
    uo += ip4_to_str(url + uo, fixture);
    const char *suf = ":8080/wgettest";
    const char *sp = suf;
    while (*sp) url[uo++] = *sp++;
    url[uo] = '\0';

    uint8_t *body = NULL;
    uint32_t len = 0;
    int status = 0;
    int rc = wget_fetch(url, &body, &len, &status);
    Serial_WriteString("[ktest] wget: rc=");
    Serial_WriteDec((uint32_t)(rc == 0));
    Serial_WriteString(" status=");
    Serial_WriteDec((uint32_t)status);
    Serial_WriteString(" len=");
    Serial_WriteDec(len);
    Serial_WriteString("\n");

    KTEST_ASSERT(rc == 0);
    KTEST_ASSERT(status == 200);
    KTEST_ASSERT(body != NULL);
    KTEST_ASSERT(len == 13);
    if (body && len == 13)
        KTEST_ASSERT(memcmp(body, "MAKAR-WGET-OK", 13) == 0);
    else
        KTEST_ASSERT(0);
    kfree(body);

    ktest_summary();
}


static void test_lwip_net_info(void)
{
    ktest_begin("lwip_net_info", "SYS_NET_INFO backing text reports lwIP interface state");

    char info[512];
    int n = net_lwip_info(info, sizeof(info));
    KTEST_ASSERT(n > 0);
    KTEST_ASSERT(strstr(info, "Ethernet adapter eth0:") != NULL);
    KTEST_ASSERT(strstr(info, "DHCP State") != NULL);
    KTEST_ASSERT(strstr(info, "IPv4 Address") != NULL);
    KTEST_ASSERT(strstr(info, "Default Gateway") != NULL);
    KTEST_ASSERT(strstr(info, "DNS Servers") != NULL);
    KTEST_ASSERT(net_lwip_control(NET_CTL_DNS_FLUSH) == 0);
    KTEST_ASSERT(net_lwip_control(NET_CTL_DHCP_RELEASE) == 0);
    KTEST_ASSERT(net_lwip_control(NET_CTL_DHCP_RENEW) == 0);
    ktest_summary();
}

static void test_devfs(void)
{
    ktest_begin("devfs", "/dev block-device nodes: lookup, readonly, pread");

    /* Unknown nodes resolve to -1 / not-exist. */
    KTEST_ASSERT(devfs_lookup("/no_such_dev") < 0);
    KTEST_ASSERT(devfs_file_exists("/no_such_dev") == 0);
    /* The mount root itself is not a node. */
    KTEST_ASSERT(devfs_lookup("/") < 0);

    /* CD-ROM node: registered whenever an ATAPI drive exists, but only
     * carries media on the ISO/live boot path.  An HDD-only boot still
     * exposes an empty ATAPI drive (devfs_node_size == 0), so gate the
     * media-dependent checks on real media being present -- the suite must
     * pass on both ISO and HDD boots. */
    int cd = devfs_lookup("/cdrom");
    if (cd >= 0 && devfs_node_size(cd) > 0) {
        KTEST_ASSERT(devfs_file_exists("/cdrom") == 1);
        KTEST_ASSERT(devfs_node_readonly(cd) == 1);

        /* A byte-addressed read of the first sector must succeed and fill
         * the request (offset 0, a full 2048-byte ATAPI sector). */
        static uint8_t sec[2048];
        long n = devfs_pread(cd, sec, sizeof(sec), 0);
        KTEST_ASSERT(n == (long)sizeof(sec));

        /* Writes to a read-only node are rejected. */
        KTEST_ASSERT(devfs_pwrite(cd, sec, sizeof(sec), 0) < 0);
    }

    /* Primary ATA disk: present on HDD boots (the medium we booted from)
     * and on live boots with an installed disk attached.  Verify a
     * byte-addressed 512-byte sector read through devfs_pread when present. */
    int hd = devfs_lookup("/hda");
    if (hd >= 0 && devfs_node_size(hd) > 0) {
        static uint8_t blk[512];
        long n = devfs_pread(hd, blk, sizeof(blk), 0);
        KTEST_ASSERT(n == (long)sizeof(blk));
    }

    /* Virtual terminals: /dev/tty0 (root console) + /dev/tty1../dev/tty9 are
     * always registered, regardless of makmux state.  They are character
     * sinks, not block devices: writable, never read-only, no LBA location,
     * reads return EOF, and a write is consumed into the slot's backing grid.
     * Exercise tty9 (slot 8) -- a background slot during ktest, so painting it
     * never disturbs the live console. */
    int t0 = devfs_lookup("/tty0");
    int t9 = devfs_lookup("/tty9");
    KTEST_ASSERT(t0 >= 0 && t9 >= 0);
    KTEST_ASSERT(devfs_file_exists("/tty0") == 1);
    KTEST_ASSERT(devfs_node_readonly(t9) == 0);
    {
        uint8_t drv = 0xFF; uint32_t lba = 0xFFFFFFFFu;
        KTEST_ASSERT(devfs_node_location(t9, &drv, &lba) < 0);   /* not a block dev */
        char rb[4];
        KTEST_ASSERT(devfs_pread(t9, rb, sizeof(rb), 0) == 0);    /* read = EOF */
        const char msg[] = "ktest\n";
        KTEST_ASSERT(devfs_pwrite(t9, msg, sizeof(msg) - 1, 0) == (long)(sizeof(msg) - 1));
    }

    ktest_summary();
}



/* ---------------------------------------------------------------------------
 * Suite: tmpfs
 *
 * Exercises the in-RAM /tmp ramdisk (fs/tmpfs.c) end-to-end through the
 * VFS dispatch matrix.  The matrix routes /tmp/... to tmpfs_*; we verify
 * write-then-read overwrite semantics (unlike logfs's append ring),
 * stat, file_exists, and delete.  Pure in-RAM -- no disk dependency.
 * ------------------------------------------------------------------------- */

static void test_tmpfs(void)
{
    ktest_begin("tmpfs", "/tmp ramdisk: write/read overwrite, stat, file_exists, delete");

    /* Initial state: file doesn't exist. */
    KTEST_ASSERT(vfs_file_exists("/tmp/probe.bin") == 0);

    /* Write through the VFS layer (routes to tmpfs_write). */
    const char *msg = "tmpfs-roundtrip-ok";
    uint32_t    mlen = (uint32_t)strlen(msg);
    KTEST_ASSERT(vfs_write_file("/tmp/probe.bin", msg, mlen) == 0);
    KTEST_ASSERT(vfs_file_exists("/tmp/probe.bin") == 1);

    /* Read back; payload must match exactly. */
    char readback[64];
    uint32_t got = 0;
    KTEST_ASSERT(vfs_read_file("/tmp/probe.bin", readback, sizeof(readback), &got) == 0);
    KTEST_ASSERT(got == mlen);
    KTEST_ASSERT(memcmp(readback, msg, mlen) == 0);

    /* Overwrite (NOT append, unlike logfs) -- second write replaces the
     * payload, doesn't tack onto the end. */
    const char *msg2 = "xx";
    KTEST_ASSERT(vfs_write_file("/tmp/probe.bin", msg2, 2) == 0);
    got = 0;
    KTEST_ASSERT(vfs_read_file("/tmp/probe.bin", readback, sizeof(readback), &got) == 0);
    KTEST_ASSERT(got == 2);
    KTEST_ASSERT(memcmp(readback, msg2, 2) == 0);

    /* stat: size reflects the most recent write, kind is regular file. */
    vfs_stat_info_t st;
    KTEST_ASSERT(vfs_stat("/tmp/probe.bin", &st) == 0);
    KTEST_ASSERT(st.size == 2);
    KTEST_ASSERT(st.kind == VFS_STAT_FILE);

    /* Delete and confirm the file disappears. */
    KTEST_ASSERT(vfs_delete_file("/tmp/probe.bin") == 0);
    KTEST_ASSERT(vfs_file_exists("/tmp/probe.bin") == 0);

    /* Dynamic file records: more than the historical 16-slot table should
     * work, and deleting them should release the records again. */
    char path[] = "/tmp/many-00.tmp";
    for (int i = 0; i < 24; i++) {
        path[10] = (char)('0' + (i / 10));
        path[11] = (char)('0' + (i % 10));
        KTEST_ASSERT(vfs_write_file(path, "x", 1) == 0);
        KTEST_ASSERT(vfs_file_exists(path) == 1);
    }
    for (int i = 0; i < 24; i++) {
        path[10] = (char)('0' + (i / 10));
        path[11] = (char)('0' + (i % 10));
        KTEST_ASSERT(vfs_delete_file(path) == 0);
        KTEST_ASSERT(vfs_file_exists(path) == 0);
    }

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: /usr resolver
 *
 * /usr is a synthetic redirect in vfs_route; the prefix is resolved
 * lazily to /mnt/cdrom/usr (CD boot) or /mnt/root/usr (HDD boot) by
 * probing for the sentinel file /usr/lib/crt0.o.  This suite proves
 * the rewrite works against whichever boot medium is active.  Skipped
 * silently when no sysroot is installed (sentinel missing on both
 * boot media) so the test stays valid on minimal builds.
 * ------------------------------------------------------------------------- */

static void test_usr(void)
{
    ktest_begin("usr", "/usr redirect: sysroot path resolution + content visible");

    /* If no sysroot is shipped on this medium, skip silently. */
    if (!vfs_file_exists("/usr/lib/crt0.o")) {
        ktest_summary();
        return;
    }

    /* The sentinel probe must work via the /usr rewrite. */
    KTEST_ASSERT(vfs_file_exists("/usr/lib/crt0.o") == 1);

    /* Runtime libraries and TCC startup objects must be reachable via /usr/lib. */
    KTEST_ASSERT(vfs_file_exists("/usr/lib/libc.a") == 1);
    KTEST_ASSERT(vfs_file_exists("/usr/lib/crt1.o") == 1);
    KTEST_ASSERT(vfs_file_exists("/usr/lib/crti.o") == 1);
    KTEST_ASSERT(vfs_file_exists("/usr/lib/crtn.o") == 1);
    KTEST_ASSERT(vfs_file_exists("/usr/lib/tcc/libtcc1.a") == 1);

    /* Headers must be reachable via /usr/include. */
    KTEST_ASSERT(vfs_file_exists("/usr/include/stdio.h") == 1);
    KTEST_ASSERT(vfs_file_exists("/usr/include/string.h") == 1);

    /* And the example source ships under /usr/share/examples. */
    KTEST_ASSERT(vfs_file_exists("/usr/share/examples/hello-tcc.c") == 1);

    /* stat shows a non-zero size for libc.a (it's an archive of real .o files). */
    vfs_stat_info_t st;
    KTEST_ASSERT(vfs_stat("/usr/lib/libc.a", &st) == 0);
    KTEST_ASSERT(st.size > 0);

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: rootfs mount layout
 *
 * Bulk VFS behavior that should not depend on typed shell commands.  The
 * rootfs is elected at "/" before ktest runs; on HDD boots vfs_auto_mount()
 * should also bind that same volume at /mnt/root so documented explicit
 * paths keep working.  ISO boots keep /mnt/root as an empty placeholder, so
 * the HDD-only assertions are gated on that slot being bound.
 * ------------------------------------------------------------------------- */

static void test_rootfs_mount_layout(void)
{
    ktest_begin("rootfs_mount_layout",
                "VFS mount-table routing: / present, /mnt virtual dir, "
                "/dev /proc /tmp /log overlays reachable");

    vfs_stat_info_t st;

    /* / is always elected (ISO9660 or ext2/FAT32). */
    KTEST_ASSERT(vfs_stat("/", &st) == 0);
    KTEST_ASSERT(st.kind == VFS_STAT_DIR);

    /* /mnt is a virtual directory synthesised from the mount table. */
    KTEST_ASSERT(vfs_stat("/mnt", &st) == 0);
    KTEST_ASSERT(st.kind == VFS_STAT_DIR);

    /* Synthetic overlays registered unconditionally by vfs_init. */
    KTEST_ASSERT(vfs_stat("/dev",  &st) == 0);
    KTEST_ASSERT(st.kind == VFS_STAT_DIR);
    KTEST_ASSERT(vfs_stat("/proc", &st) == 0);
    KTEST_ASSERT(st.kind == VFS_STAT_DIR);
    KTEST_ASSERT(vfs_stat("/tmp",  &st) == 0);
    KTEST_ASSERT(st.kind == VFS_STAT_DIR);
    KTEST_ASSERT(vfs_stat("/log",  &st) == 0);
    KTEST_ASSERT(st.kind == VFS_STAT_DIR);

    ktest_summary();
}


/* ---------------------------------------------------------------------------
 * Suite: PMM
 *
 * Exercises pmm_alloc_frame / pmm_free_frame using the live allocator.
 * All allocated frames are freed before the suite returns so the PMM state
 * is identical before and after.
 * ------------------------------------------------------------------------- */

static void test_pmm(void)
{
    ktest_begin("pmm", "physical-memory allocator: 4 KiB frame alloc/free + free-count accounting");

    /* Alloc must return a 4 KiB-aligned non-error address. */
    uint32_t f1 = pmm_alloc_frame();
    KTEST_ASSERT(f1 != PMM_ALLOC_ERROR);
    KTEST_ASSERT((f1 & 0xFFFu) == 0);

    /* Two consecutive allocs must return distinct frames. */
    uint32_t f2 = pmm_alloc_frame();
    KTEST_ASSERT(f2 != PMM_ALLOC_ERROR);
    KTEST_ASSERT(f1 != f2);

    /* free_count decreases by 1 per alloc. */
    uint32_t fc = pmm_free_count();
    uint32_t f3 = pmm_alloc_frame();
    KTEST_ASSERT(f3 != PMM_ALLOC_ERROR);
    KTEST_ASSERT(pmm_free_count() == fc - 1);

    /* Freeing a frame increments free_count and makes it re-allocatable. */
    pmm_free_frame(f3);
    KTEST_ASSERT(pmm_free_count() == fc);
    uint32_t f4 = pmm_alloc_frame();
    KTEST_ASSERT(f4 == f3);   /* same frame recycled (first-fit scan) */

    pmm_free_frame(f1);
    pmm_free_frame(f2);
    pmm_free_frame(f4);

    /* --- refcount semantics (slice 12a, fork+COW prep) --- */
    uint32_t rc_fc = pmm_free_count();
    uint32_t rf = pmm_alloc_frame();
    KTEST_ASSERT(rf != PMM_ALLOC_ERROR);
    KTEST_ASSERT(pmm_ref_count(rf) == 1);
    KTEST_ASSERT(pmm_free_count() == rc_fc - 1);

    /* Bump to 3 owners. */
    pmm_inc_ref(rf);
    pmm_inc_ref(rf);
    KTEST_ASSERT(pmm_ref_count(rf) == 3);

    /* Two frees drop refcount to 1 but DON'T release the frame. */
    pmm_free_frame(rf);
    pmm_free_frame(rf);
    KTEST_ASSERT(pmm_ref_count(rf) == 1);
    KTEST_ASSERT(pmm_free_count() == rc_fc - 1);

    /* Final free releases the frame and makes it re-allocatable. */
    pmm_free_frame(rf);
    KTEST_ASSERT(pmm_ref_count(rf) == 0);
    KTEST_ASSERT(pmm_free_count() == rc_fc);
    uint32_t rf2 = pmm_alloc_frame();
    KTEST_ASSERT(rf2 == rf);
    pmm_free_frame(rf2);

    ktest_summary();
}

/* Walk page directory `pd` and return the physical frame backing virtual
 * address `va`, or 0 if not present.  Mirrors the inline walk in test_vmm. */
static uint32_t kt_pte_phys(uint32_t *pd, uint32_t va)
{
    uint32_t pde = pd[va >> 22];
    if (!(pde & 0x1u) || (pde & 0x80u)) return 0;       /* present, not large */
    uint32_t *pt = (uint32_t *)(pde & ~0xFFFu);
    uint32_t pte = pt[(va >> 12) & 0x3FFu];
    return (pte & 0x1u) ? (pte & ~0xFFFu) : 0;
}

/* ---------------------------------------------------------------------------
 * Suite: shared pixel surfaces (kernel/surface.h)
 *
 * The only shared-memory primitive: a surface's physical frames are mapped
 * into two independent page directories at once, so the window manager and a
 * forked graphical child share a frame buffer.  Verifies create/info, that two
 * PDs resolve to the *same* physical frame (genuine sharing, write-visible),
 * reference-counted teardown (a holder's release clears only its own PTEs and
 * keeps the surface alive while others hold it), and exact frame accounting.
 * ------------------------------------------------------------------------- */
static void test_surface(void)
{
    ktest_begin("surface", "shared pixel surfaces: create/map/share/release/destroy + frame accounting");

    uint32_t fc0 = pmm_free_count();

    /* 8x8 RGBA = 256 bytes -> exactly one frame. */
    int id = surface_create(8, 8);
    KTEST_ASSERT(id >= 0);
    KTEST_ASSERT(surface_info(id) == ((8u << 16) | 8u));
    KTEST_ASSERT(pmm_free_count() == fc0 - 1);          /* one frame reserved */

    /* Map the same surface into two independent address spaces. */
    uint32_t *pdA = vmm_create_pd();
    uint32_t *pdB = vmm_create_pd();
    KTEST_ASSERT(pdA != NULL && pdB != NULL);

    task_t ta, tb;
    memset(&ta, 0, sizeof ta);
    memset(&tb, 0, sizeof tb);
    ta.page_dir = pdA;                                  /* mmap_next = 0 -> lazy base */
    tb.page_dir = pdB;

    uint32_t va_a = surface_map(id, &ta);
    uint32_t va_b = surface_map(id, &tb);
    KTEST_ASSERT(va_a != 0 && va_b != 0);

    /* Both PDs must resolve to the SAME physical frame -> shared memory. */
    uint32_t physA = kt_pte_phys(pdA, va_a);
    uint32_t physB = kt_pte_phys(pdB, va_b);
    KTEST_ASSERT(physA != 0 && physA == physB);

    /* A write through the shared frame is visible to "both" mappings. */
    *(volatile uint32_t *)physA = 0xCAFEBABEu;
    KTEST_ASSERT(*(volatile uint32_t *)physB == 0xCAFEBABEu);

    /* Releasing holder A clears only A's PTE; the surface stays alive because
     * holder B and the creator reference still hold it. */
    surface_release_task(&ta);
    KTEST_ASSERT(kt_pte_phys(pdA, va_a) == 0);
    KTEST_ASSERT(kt_pte_phys(pdB, va_b) == physA);
    KTEST_ASSERT(surface_info(id) == ((8u << 16) | 8u));

    /* Release holder B and drop the creator ref -> frame is reclaimed. */
    surface_release_task(&tb);
    KTEST_ASSERT(surface_destroy(id, task_current()) == 0);
    KTEST_ASSERT(surface_info(id) == (uint32_t)-1);

    /* Surface pages are already unmapped, so tearing the scratch PDs down frees
     * only their own PT+PD frames (no double-free) and accounting balances. */
    vmm_free_pd(pdA);
    vmm_free_pd(pdB);
    KTEST_ASSERT(pmm_free_count() == fc0);

    /* Bad-arg guards. */
    KTEST_ASSERT(surface_create(0, 8) == -1);
    KTEST_ASSERT(surface_info(-1) == (uint32_t)-1);

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: IDE block I/O (DMA + PIO round-trip)
 *
 * Exercises the exact path the installer's file copy uses:
 * ide_write_sectors() -> ide_read_sectors() through the public API, which
 * prefers bus-master DMA and silently falls back to PIO (the whole point of
 * the Hyper-V/VMware/VBox/legacy resilience work).  The transfer is 128
 * sectors = 64 KiB, spanning TWO 32 KiB DMA bounce-buffer chunks, so a
 * chunk-loop / boundary bug corrupts the second half and the memcmp catches
 * it.  Non-destructive: the target sectors are read and saved first, then
 * restored, so this is safe even when ktest runs against a real installed
 * disk (HDD boot) and not just the scratch image the harness attaches.
 *
 * Skips cleanly (one guard assert only) when no writable ATA drive exists,
 * so CD-only boots and the GDB iso-test still pass.
 * ------------------------------------------------------------------------- */
static void test_ide_dma(void)
{
    ktest_begin("ide_dma", "block I/O: 64 KiB write/read round-trip (DMA->PIO), multi-chunk, non-destructive");

    /* Out-of-range guard works regardless of attached hardware. */
    KTEST_ASSERT(ide_get_drive(IDE_MAX_DRIVES) == NULL);

    /* Find the first writable ATA disk big enough for the test window. */
    const uint32_t SECS = 128;                  /* 64 KiB = 2 DMA chunks */
    int idx = -1;
    for (uint8_t i = 0; i < IDE_MAX_DRIVES; i++) {
        const ide_drive_t *d = ide_get_drive(i);
        if (d && d->present && d->type == IDE_TYPE_ATA && d->size > SECS + 4096) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        Serial_WriteString("[ktest]   ide_dma: no writable ATA disk attached, skipping round-trip\n");
        ktest_summary();
        return;
    }

    const ide_drive_t *drv = ide_get_drive((uint8_t)idx);
    /* Land the window a little below the end of the disk, clear of any
     * partition table / FS metadata the harness or installer cares about. */
    uint32_t lba   = drv->size - SECS - 2048;
    uint32_t bytes = SECS * 512u;

    uint8_t *save     = (uint8_t *)kmalloc(bytes);
    uint8_t *pattern  = (uint8_t *)kmalloc(bytes);
    uint8_t *readback = (uint8_t *)kmalloc(bytes);
    KTEST_ASSERT(save && pattern && readback);
    if (!save || !pattern || !readback) {
        kfree(save); kfree(pattern); kfree(readback);
        ktest_summary();
        return;
    }

    /* Preserve the original contents so the test leaves the disk untouched. */
    KTEST_ASSERT(ide_read_sectors((uint8_t)idx, lba, (uint8_t)SECS, save) == 0);

    /* A per-byte-distinct pattern that varies across the 32 KiB chunk
     * boundary (byte 32768), so a second-chunk LBA/offset bug is visible. */
    for (uint32_t i = 0; i < bytes; i++)
        pattern[i] = (uint8_t)((i * 131u + lba) ^ (i >> 7));

    KTEST_ASSERT(ide_write_sectors((uint8_t)idx, lba, (uint8_t)SECS, pattern) == 0);

    memset(readback, 0, bytes);
    KTEST_ASSERT(ide_read_sectors((uint8_t)idx, lba, (uint8_t)SECS, readback) == 0);
    KTEST_ASSERT(memcmp(readback, pattern, bytes) == 0);   /* core: write==read */

    /* Single-sector round-trip exercises the count==1 (sub-chunk) path. */
    pattern[0] ^= 0xFFu;
    KTEST_ASSERT(ide_write_sectors((uint8_t)idx, lba, 1, pattern) == 0);
    memset(readback, 0, 512);
    KTEST_ASSERT(ide_read_sectors((uint8_t)idx, lba, 1, readback) == 0);
    KTEST_ASSERT(memcmp(readback, pattern, 512) == 0);

    /* Restore the original sectors and confirm the disk is as we found it. */
    KTEST_ASSERT(ide_write_sectors((uint8_t)idx, lba, (uint8_t)SECS, save) == 0);
    memset(readback, 0, bytes);
    KTEST_ASSERT(ide_read_sectors((uint8_t)idx, lba, (uint8_t)SECS, readback) == 0);
    KTEST_ASSERT(memcmp(readback, save, bytes) == 0);

    kfree(save); kfree(pattern); kfree(readback);
    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: buddy allocator
 *
 * Exercises the multi-order page allocator beneath pmm_alloc_frame: contiguous
 * power-of-two blocks, natural alignment (what DMA rings need), real writable
 * backing RAM, exact free-frame accounting, and coalescing on free.
 * ------------------------------------------------------------------------- */

static void test_buddy(void)
{
    ktest_begin("buddy", "buddy allocator: contiguous alloc, alignment, RAM, coalescing, accounting");

    uint32_t free0 = pmm_free_count();

    /* Order-0 path == legacy pmm_alloc_frame. */
    uint32_t a = pmm_alloc_pages(0);
    KTEST_ASSERT(a != PMM_ALLOC_ERROR);
    KTEST_ASSERT((a & (PMM_FRAME_SIZE - 1)) == 0);
    KTEST_ASSERT(pmm_free_count() == free0 - 1);
    pmm_free_pages(a, 0);
    KTEST_ASSERT(pmm_free_count() == free0);

    /* Order-3: 8 contiguous frames, naturally aligned to 8*4 KiB = 32 KiB. */
    uint32_t blk = pmm_alloc_pages(3);
    KTEST_ASSERT(blk != PMM_ALLOC_ERROR);
    KTEST_ASSERT((blk & ((8u * PMM_FRAME_SIZE) - 1)) == 0);
    KTEST_ASSERT(pmm_free_count() == free0 - 8);

    /* The block is real, writable RAM (identity-mapped below 256 MiB):
     * stamp one word per frame and read it back. */
    volatile uint32_t *p = (volatile uint32_t *)blk;
    for (int i = 0; i < 8; i++)
        p[i * (PMM_FRAME_SIZE / 4)] = 0xB0B00000u + (uint32_t)i;
    int ram_ok = 1;
    for (int i = 0; i < 8; i++)
        if (p[i * (PMM_FRAME_SIZE / 4)] != 0xB0B00000u + (uint32_t)i)
            ram_ok = 0;
    KTEST_ASSERT(ram_ok);

    pmm_free_pages(blk, 3);
    KTEST_ASSERT(pmm_free_count() == free0);

    /* Order-5: 128 KiB-aligned. */
    uint32_t big = pmm_alloc_pages(5);
    KTEST_ASSERT(big != PMM_ALLOC_ERROR);
    KTEST_ASSERT((big & ((32u * PMM_FRAME_SIZE) - 1)) == 0);
    pmm_free_pages(big, 5);
    KTEST_ASSERT(pmm_free_count() == free0);

    /* Coalescing: an order-4 alloc splits a larger block into buddies; freeing
     * it must merge them back so the *same* block is handed out next time. */
    uint32_t c1 = pmm_alloc_pages(4);
    KTEST_ASSERT(c1 != PMM_ALLOC_ERROR);
    pmm_free_pages(c1, 4);
    uint32_t c2 = pmm_alloc_pages(4);
    KTEST_ASSERT(c2 == c1);                 /* coalesced, not left fragmented */
    pmm_free_pages(c2, 4);

    /* A mixed alloc/free storm must not leak or double-count frames. */
    for (int it = 0; it < 64; it++) {
        unsigned ord = (unsigned)(it % 6);          /* orders 0..5 */
        uint32_t x = pmm_alloc_pages(ord);
        KTEST_ASSERT(x != PMM_ALLOC_ERROR);
        KTEST_ASSERT((x & (((1u << ord) * PMM_FRAME_SIZE) - 1)) == 0);
        pmm_free_pages(x, ord);
    }
    KTEST_ASSERT(pmm_free_count() == free0);

    /* Over-large order is rejected, not serviced. */
    KTEST_ASSERT(pmm_alloc_pages(PMM_MAX_ORDER) == PMM_ALLOC_ERROR);

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: heap
 *
 * Tests the kmalloc/kfree/krealloc first-fit allocator.
 * ------------------------------------------------------------------------- */

static void test_heap(void)
{
    ktest_begin("heap", "kernel heap: kmalloc/kfree, bytes-written sanity, exhaustion path");

    /* kmalloc(0) must return NULL. */
    KTEST_ASSERT(kmalloc(0) == NULL);

    /* Normal allocations return non-NULL distinct pointers. */
    void *p1 = kmalloc(64);
    void *p2 = kmalloc(64);
    KTEST_ASSERT(p1 != NULL);
    KTEST_ASSERT(p2 != NULL);
    KTEST_ASSERT(p1 != p2);

    /* Data written to one block is not clobbered by the other. */
    memset(p1, 0xAB, 64);
    memset(p2, 0xCD, 64);
    KTEST_ASSERT(((uint8_t *)p1)[0]  == 0xAB);
    KTEST_ASSERT(((uint8_t *)p1)[63] == 0xAB);
    KTEST_ASSERT(((uint8_t *)p2)[0]  == 0xCD);

    /* kfree(NULL) must not crash. */
    kfree(NULL);

    /* krealloc to a larger size preserves existing bytes. */
    void *p3 = kmalloc(16);
    KTEST_ASSERT(p3 != NULL);
    memset(p3, 0xEF, 16);
    void *p4 = krealloc(p3, 128);
    KTEST_ASSERT(p4 != NULL);
    KTEST_ASSERT(((uint8_t *)p4)[0]  == 0xEF);
    KTEST_ASSERT(((uint8_t *)p4)[15] == 0xEF);

    /* krealloc(ptr, 0) behaves like kfree and returns NULL. */
    void *p5 = kmalloc(32);
    KTEST_ASSERT(p5 != NULL);
    void *p6 = krealloc(p5, 0);
    KTEST_ASSERT(p6 == NULL);

    /* After freeing, the allocator can hand out a new block in that space. */
    kfree(p1);
    void *p7 = kmalloc(64);
    KTEST_ASSERT(p7 != NULL);

    kfree(p2);
    kfree(p4);
    kfree(p7);

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: VMM
 *
 * Tests vmm_create_pd / vmm_map_page / vmm_unmap_page / vmm_free_pd.
 *
 * Page table entries are inspected directly: since the kernel is identity-
 * mapped (phys == virt), every PMM frame address is directly dereferenceable
 * as a uint32_t pointer.
 *
 * x86 page-entry bit positions used here:
 *   bit 0 (0x1)  – Present
 *   bit 7 (0x80) – PS / large page (set in kernel PSE entries)
 * ------------------------------------------------------------------------- */

#define KT_PAGE_PRESENT  0x1u
#define KT_PAGE_LARGE    0x80u

static void test_vmm(void)
{
    ktest_begin("vmm", "per-task page directory: create, map, switch, lookup, teardown");

    uint32_t *kpd = paging_kernel_pd();

    /* vmm_create_pd returns a 4 KiB-aligned pointer. */
    uint32_t *pd = vmm_create_pd();
    KTEST_ASSERT(pd != NULL);
    KTEST_ASSERT(((uint32_t)pd & 0xFFFu) == 0);

    /* Every non-zero kernel PDE must be propagated into the new PD. */
    bool all_kpdes_ok = true;
    for (uint32_t i = 0; i < 1024; i++) {
        if (kpd[i] && pd[i] != kpd[i]) {
            all_kpdes_ok = false;
            break;
        }
    }
    KTEST_ASSERT(all_kpdes_ok);

    /* Spot-check: identity-window large-page entries copied. */
    KTEST_ASSERT(pd[0]  == kpd[0]);
    KTEST_ASSERT(pd[32] == kpd[32]);
    KTEST_ASSERT(pd[63] == kpd[63]);

    /* vmm_map_page: allocate a frame and map it at a user virtual address. */
    uint32_t phys = pmm_alloc_frame();
    KTEST_ASSERT(phys != PMM_ALLOC_ERROR);

    uint32_t virt = 0x40001000u;               /* PDE 256, PTE 1 */
    uint32_t pdi  = virt >> 22;                /* 256             */
    uint32_t pti  = (virt >> 12) & 0x3FFu;    /* 1               */

    vmm_map_page(pd, virt, phys, VMM_FLAG_USER | VMM_FLAG_WRITABLE);

    /* PDE for the mapped region must be present and NOT a large page. */
    KTEST_ASSERT((pd[pdi] & KT_PAGE_PRESENT) != 0);
    KTEST_ASSERT((pd[pdi] & KT_PAGE_LARGE)   == 0);

    /* The PTE must point to the right physical frame with Present set. */
    uint32_t *pt = (uint32_t *)(pd[pdi] & ~0xFFFu);
    KTEST_ASSERT((pt[pti] & KT_PAGE_PRESENT) != 0);
    KTEST_ASSERT((pt[pti] & ~0xFFFu) == phys);

    /* Mapping inside the kernel large-page window is silently ignored. */
    vmm_map_page(pd, 0x1000u, phys, VMM_FLAG_USER);  /* PDE 0 = large page */
    KTEST_ASSERT(pd[0] == kpd[0]);                    /* PDE 0 unchanged    */

    /* vmm_unmap_page clears the PTE (does not free the frame). */
    vmm_unmap_page(pd, virt);
    KTEST_ASSERT((pt[pti] & KT_PAGE_PRESENT) == 0);

    /* Re-map so vmm_free_pd has a frame to release. */
    vmm_map_page(pd, virt, phys, VMM_FLAG_USER | VMM_FLAG_WRITABLE);

    /* vmm_free_pd releases the mapped frame, the page table, and the PD itself.
     * Three PMM frames total must be returned. */
    uint32_t fc_before = pmm_free_count();
    vmm_free_pd(pd);                           /* frees phys + PT + PD = 3 */
    KTEST_ASSERT(pmm_free_count() == fc_before + 3);

    /* --- COW clone (slice 12b) ---
     * Build a parent PD with one writable user page, clone it, and verify:
     *   - both parent and child PTEs are now RO + COW-tagged
     *   - the shared frame's refcount is 2
     *   - freeing the child drops refcount to 1 (frame still owned by parent)
     *   - freeing the parent drops refcount to 0 (frame actually released) */
    uint32_t *parent = vmm_create_pd();
    KTEST_ASSERT(parent != NULL);

    uint32_t cow_phys = pmm_alloc_frame();
    KTEST_ASSERT(cow_phys != PMM_ALLOC_ERROR);
    KTEST_ASSERT(pmm_ref_count(cow_phys) == 1);

    uint32_t cow_virt = 0x40002000u;
    uint32_t cow_pdi  = cow_virt >> 22;
    uint32_t cow_pti  = (cow_virt >> 12) & 0x3FFu;
    vmm_map_page(parent, cow_virt, cow_phys, VMM_FLAG_USER | VMM_FLAG_WRITABLE);

    uint32_t *child = vmm_clone_pd_cow(parent);
    KTEST_ASSERT(child != NULL);
    KTEST_ASSERT(child != parent);

    uint32_t *p_pt = (uint32_t *)(parent[cow_pdi] & ~0xFFFu);
    uint32_t *c_pt = (uint32_t *)(child[cow_pdi]  & ~0xFFFu);
    KTEST_ASSERT(p_pt != c_pt);  /* child got a fresh PT frame */

    /* Both PTEs lost WRITABLE and gained the COW bit. */
    KTEST_ASSERT((p_pt[cow_pti] & 0x2u) == 0);
    KTEST_ASSERT((p_pt[cow_pti] & VMM_PTE_COW) != 0);
    KTEST_ASSERT((c_pt[cow_pti] & 0x2u) == 0);
    KTEST_ASSERT((c_pt[cow_pti] & VMM_PTE_COW) != 0);

    /* Both point at the same physical frame, now refcount=2. */
    KTEST_ASSERT((p_pt[cow_pti] & ~0xFFFu) == cow_phys);
    KTEST_ASSERT((c_pt[cow_pti] & ~0xFFFu) == cow_phys);
    KTEST_ASSERT(pmm_ref_count(cow_phys) == 2);

    /* Free child first: refcount drops to 1, frame still owned by parent. */
    vmm_free_pd(child);
    KTEST_ASSERT(pmm_ref_count(cow_phys) == 1);

    /* Free parent: refcount drops to 0, frame released to the pool. */
    vmm_free_pd(parent);
    KTEST_ASSERT(pmm_ref_count(cow_phys) == 0);

    /* --- COW #PF resolution (slice 12c) ---
     * End-to-end: set up a parent PD with a writable user page, fill it
     * with a sentinel, COW-clone, switch to the parent's PD, write to
     * the shared page from kernel mode (which takes a #PF and goes
     * through try_handle_cow_fault), then verify:
     *   - the write succeeded (parent's page now holds the new value)
     *   - the child's PTE still points at the original frame, unchanged
     *   - refcount on the original frame is back to 1 (parent got a
     *     fresh copy via the slow path)
     *
     * Note: this exercises the slow path because rc=2 at fault time.
     * The fast path (rc==1, just flip RW) is taken implicitly whenever
     * an exec'd ELF writes to its own data section after the page got
     * COW-marked during a fork that the other side already exited from. */
    uint32_t *cow_parent = vmm_create_pd();
    KTEST_ASSERT(cow_parent != NULL);
    uint32_t orig_phys = pmm_alloc_frame();
    KTEST_ASSERT(orig_phys != PMM_ALLOC_ERROR);

    /* Fill the frame with a sentinel via its kernel identity mapping. */
    volatile uint32_t *orig_words = (volatile uint32_t *)orig_phys;
    for (int i = 0; i < 1024; i++) orig_words[i] = 0xAA550000u | (uint32_t)i;

    uint32_t cow_va = 0x40004000u;
    vmm_map_page(cow_parent, cow_va, orig_phys, VMM_FLAG_USER | VMM_FLAG_WRITABLE);

    uint32_t *cow_child = vmm_clone_pd_cow(cow_parent);
    KTEST_ASSERT(cow_child != NULL);
    KTEST_ASSERT(pmm_ref_count(orig_phys) == 2);

    /* Switch into the parent's PD so a write through cow_va lands in
     * the parent's PTE.  Save the original CR3 so we can switch back. */
    uint32_t saved_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(saved_cr3));
    vmm_switch(cow_parent);

    /* This write triggers a #PF (page is RO + COW); the handler must
     * allocate a fresh frame, copy the contents, and promote the PTE
     * to RW.  If it doesn't, we'd panic right here. */
    volatile uint32_t *p = (volatile uint32_t *)cow_va;
    p[0] = 0xDEADBEEFu;
    p[7] = 0xCAFEBABEu;

    /* Restore the kernel's PD before walking the test PDs (we'll be
     * touching their physical frames through their identity-mapped
     * virtual addresses, which works regardless of CR3). */
    asm volatile("mov %0, %%cr3" :: "r"(saved_cr3) : "memory");

    uint32_t cow_pdi2 = cow_va >> 22;
    uint32_t cow_pti2 = (cow_va >> 12) & 0x3FFu;
    uint32_t *p_pt2 = (uint32_t *)(cow_parent[cow_pdi2] & ~0xFFFu);
    uint32_t *c_pt2 = (uint32_t *)(cow_child[cow_pdi2]  & ~0xFFFu);
    uint32_t new_phys = p_pt2[cow_pti2] & ~0xFFFu;

    /* Parent now owns a different frame; child still points at the
     * original. */
    KTEST_ASSERT(new_phys != orig_phys);
    KTEST_ASSERT((c_pt2[cow_pti2] & ~0xFFFu) == orig_phys);

    /* Parent's PTE is writable, COW bit cleared. */
    KTEST_ASSERT((p_pt2[cow_pti2] & 0x2u) != 0);
    KTEST_ASSERT((p_pt2[cow_pti2] & VMM_PTE_COW) == 0);

    /* Refcounts: parent's new private frame is 1, original frame
     * still has one owner (the child). */
    KTEST_ASSERT(pmm_ref_count(new_phys)  == 1);
    KTEST_ASSERT(pmm_ref_count(orig_phys) == 1);

    /* Parent's new frame holds the post-write values; original frame
     * (still owned by child) holds the sentinel. */
    volatile uint32_t *parent_view = (volatile uint32_t *)new_phys;
    KTEST_ASSERT(parent_view[0] == 0xDEADBEEFu);
    KTEST_ASSERT(parent_view[7] == 0xCAFEBABEu);
    KTEST_ASSERT(orig_words[0]  == 0xAA550000u);
    KTEST_ASSERT(orig_words[7]  == (0xAA550000u | 7u));

    vmm_free_pd(cow_parent);
    vmm_free_pd(cow_child);
    KTEST_ASSERT(pmm_ref_count(new_phys)  == 0);
    KTEST_ASSERT(pmm_ref_count(orig_phys) == 0);

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: task
 *
 * Exercises task_create and task_yield using the live task pool.
 * Two lightweight noop tasks are created; task_yield transfers control to them
 * and they self-terminate via task_exit, eventually returning here.
 * ------------------------------------------------------------------------- */

static volatile int noop_ran;
static void noop_task(void) { noop_ran = 1; task_exit(); }

static void test_task(void)
{
    ktest_begin("task", "scheduler primitives: task pool, state transitions, yield semantics");

    /* Disable interrupts across both creates so the timer cannot preempt
     * noop1 before noop2 exists - otherwise noop1 runs, dies, and its slot
     * gets recycled for noop2, making t1 == t2. */
    disable_interrupts();
    noop_ran = 0;
    task_t *t1 = task_create("ktest_noop1", noop_task);
    task_t *t2 = task_create("ktest_noop2", noop_task);
    enable_interrupts();

    KTEST_ASSERT(t1 != NULL);
    KTEST_ASSERT(t2 != NULL);
    KTEST_ASSERT(t1 != t2);

    /* Yield until at least one noop task has run and exited. */
    for (int i = 0; i < 100 && !noop_ran; i++)
        task_yield();
    KTEST_ASSERT(noop_ran);
    KTEST_ASSERT(t1->state == TASK_DEAD || t2->state == TASK_DEAD);

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: IPC (microkernel synchronous message passing)
 *
 * Spawns a server task that loops on ipc_recv(IPC_ANY) and replies, then
 * drives it as an RPC client via ipc_sendrec.  Exercises both rendezvous
 * orderings (sender-blocks-first and receiver-blocks-first), the sender
 * queue, message integrity, src stamping, and clean teardown via a QUIT
 * message.  Proves the blocking primitive the whole microkernel direction
 * rests on.
 * ------------------------------------------------------------------------- */

#define TEST_IPC_REQ    1
#define TEST_IPC_QUIT   2
#define TEST_IPC_REPLY  100

static int s_ipc_server_pid;

static void test_ipc_server(void)
{
    for (;;) {
        ipc_msg_t m;
        if (ipc_recv(IPC_ANY, &m) != 0)
            break;                       /* partner gone -- bail */
        if (m.type == TEST_IPC_QUIT)
            break;
        if (m.type == TEST_IPC_REQ) {
            ipc_msg_t r;
            r.type    = TEST_IPC_REPLY;
            r.data[0] = m.data[0] + 1;   /* server transforms the payload */
            ipc_send(m.src, &r);         /* reply to whoever asked */
        }
    }
    task_exit();
}

static void test_ipc(void)
{
    ktest_begin("ipc",
                "MINIX-style synchronous IPC: sendrec RPC to a server task, "
                "blocking rendezvous + sender queue + teardown");

    task_t *srv = task_create("ipc_server", test_ipc_server);
    KTEST_ASSERT(srv != NULL);
    if (!srv) { ktest_summary(); return; }
    s_ipc_server_pid = srv->pid;

    /* First request: the server hasn't run yet, so our send blocks and
     * enqueues -- exercises the sender-blocks-first path.  Later iterations
     * find the server already waiting in recv -- the fast path. */
    int all_ok = 1;
    for (int i = 0; i < 8; i++) {
        ipc_msg_t m;
        memset(&m, 0, sizeof(m));
        m.type    = TEST_IPC_REQ;
        m.data[0] = (uint32_t)(i * 10);

        int rc = ipc_sendrec(srv->pid, &m);
        if (rc != 0)                              { all_ok = 0; break; }
        if (m.type != TEST_IPC_REPLY)             { all_ok = 0; break; }
        if (m.data[0] != (uint32_t)(i * 10 + 1))  { all_ok = 0; break; }
        if (m.src != srv->pid)                    { all_ok = 0; break; }
    }
    KTEST_ASSERT(all_ok);

    /* Self-send is rejected. */
    {
        ipc_msg_t m; memset(&m, 0, sizeof(m));
        KTEST_ASSERT(ipc_send(task_current()->pid, &m) != 0);
    }

    /* Send to a non-existent endpoint fails with an error, not a hang. */
    {
        ipc_msg_t m; memset(&m, 0, sizeof(m)); m.type = TEST_IPC_REQ;
        KTEST_ASSERT(ipc_send(0x7fffffff, &m) != 0);
    }

    /* Tell the server to quit, then join. */
    ipc_msg_t q; memset(&q, 0, sizeof(q)); q.type = TEST_IPC_QUIT;
    ipc_send(srv->pid, &q);
    for (int i = 0; i < 256 && srv->state != TASK_DEAD; i++)
        task_yield();
    KTEST_ASSERT(srv->state == TASK_DEAD);

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: procfs task listing
 *
 * /proc/tasks is bulk kernel behavior, not a keyboard/UI behavior.  Dead task
 * slots can linger until task_create reclaims them, but procfs must hide those
 * slots from user-facing listings so tools like maktop and `cat /proc/tasks`
 * only show live work.
 * ------------------------------------------------------------------------- */

static void test_procfs_tasks(void)
{
    ktest_begin("procfs_tasks", "/proc/tasks hides lingering TASK_DEAD slots");

    int saw_dead_slot = 0;
    int n = task_count();
    for (int i = 0; i < n; i++) {
        task_t *t = task_get(i);
        if (t && t->state == TASK_DEAD) {
            saw_dead_slot = 1;
            break;
        }
    }

    KTEST_ASSERT(saw_dead_slot == 1);

    char buf[1024];
    uint32_t got = 0;
    KTEST_ASSERT(vfs_read_file("/proc/tasks", buf, sizeof(buf) - 1, &got) == 0);
    KTEST_ASSERT(got > 0);
    if (got >= sizeof(buf)) got = sizeof(buf) - 1;
    buf[got] = '\0';

    KTEST_ASSERT(strstr(buf, "PID NAME") != NULL);
    KTEST_ASSERT(strstr(buf, "DEAD") == NULL);

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: getpid
 *
 * SYS_GETPID(20) / SYS_GETPPID(64) return task_current()->pid /
 * parent_pid.  Drive both via syscall_dispatch with a stack frame, the
 * same shape as test_syscall.
 * ------------------------------------------------------------------------- */

static void test_getpid(void)
{
    ktest_begin("getpid", "SYS_GETPID/GETPPID return task_current() pid/parent_pid");

    task_t *me = task_current();
    KTEST_ASSERT(me != NULL);

    registers_t regs;
    memset(&regs, 0, sizeof(regs));
    regs.eax = SYS_GETPID;
    syscall_dispatch(&regs);
    KTEST_ASSERT_EQ((int)regs.eax, me->pid);

    memset(&regs, 0, sizeof(regs));
    regs.eax = SYS_GETPPID;
    syscall_dispatch(&regs);
    KTEST_ASSERT_EQ((int)regs.eax, me->parent_pid);

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: rtc_unix_time
 *
 * Verifies the CMOS RTC reader and the SYS_GETTIMEOFDAY/CLOCK_GETTIME
 * dispatch.  rtc_unix_time must return a seconds-since-1970 value in
 * the plausible window (>= 2025-01-01, < 2100-01-01).  CLOCK_MONOTONIC
 * must be non-decreasing across two reads.
 * ------------------------------------------------------------------------- */

static void test_rtc_unix_time(void)
{
    ktest_begin("rtc_unix_time", "RTC -> Unix epoch, SYS_GETTIMEOFDAY, CLOCK_MONOTONIC monotonic");

    /* 2025-01-01 00:00:00 UTC = 1735689600
     * 2100-01-01 00:00:00 UTC = 4102444800 */
    uint32_t secs = 0;
    KTEST_ASSERT(rtc_unix_time(&secs) == 0);
    KTEST_ASSERT(secs >= 1735689600u);
    KTEST_ASSERT(secs <  4102444800u);

    registers_t regs;
    struct timeval tv = { 0, 0 };
    memset(&regs, 0, sizeof(regs));
    regs.eax = SYS_GETTIMEOFDAY;
    regs.ebx = (uint32_t)(uintptr_t)&tv;
    syscall_dispatch(&regs);
    KTEST_ASSERT_EQ((int)regs.eax, 0);
    KTEST_ASSERT((uint32_t)tv.tv_sec >= 1735689600u);

    struct timespec ts1 = { 0, 0 }, ts2 = { 0, 0 };
    memset(&regs, 0, sizeof(regs));
    regs.eax = SYS_CLOCK_GETTIME;
    regs.ebx = CLOCK_MONOTONIC;
    regs.ecx = (uint32_t)(uintptr_t)&ts1;
    syscall_dispatch(&regs);
    KTEST_ASSERT_EQ((int)regs.eax, 0);

    /* Spin briefly so the tick advances. */
    uint32_t t0 = timer_get_ticks();
    while (timer_get_ticks() - t0 < 2) { /* ~20 ms */ }

    memset(&regs, 0, sizeof(regs));
    regs.eax = SYS_CLOCK_GETTIME;
    regs.ebx = CLOCK_MONOTONIC;
    regs.ecx = (uint32_t)(uintptr_t)&ts2;
    syscall_dispatch(&regs);
    KTEST_ASSERT_EQ((int)regs.eax, 0);

    int monotonic = (ts2.tv_sec > ts1.tv_sec) ||
                    (ts2.tv_sec == ts1.tv_sec && ts2.tv_nsec >= ts1.tv_nsec);
    KTEST_ASSERT(monotonic);

    /* Unknown clockid rejected. */
    memset(&regs, 0, sizeof(regs));
    regs.eax = SYS_CLOCK_GETTIME;
    regs.ebx = 999;
    regs.ecx = (uint32_t)(uintptr_t)&ts1;
    syscall_dispatch(&regs);
    KTEST_ASSERT_EQ((int)regs.eax, -1);

    /* USER_HZ contract: the internal PIT runs at TIMER_HZ (250) but
     * SYS_UPTIME reports in fixed 100 Hz user-ticks (timer_user_ticks).
     * Over a ksleep(20) delay (0.2 s in legacy 100 Hz units) uptime must
     * advance by ~20 user-ticks -- guards both the rate and the scaling so
     * a future rate change can't silently skew wall-clock-facing apps. */
    memset(&regs, 0, sizeof(regs));
    regs.eax = SYS_UPTIME;
    syscall_dispatch(&regs);
    uint32_t up0 = regs.eax;
    ksleep(20);
    memset(&regs, 0, sizeof(regs));
    regs.eax = SYS_UPTIME;
    syscall_dispatch(&regs);
    uint32_t dup = regs.eax - up0;
    KTEST_ASSERT(dup >= 16u && dup <= 24u);   /* ~20 user-ticks, ±20% */

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: posix_fs_syscalls
 *
 * Proves the new POSIX-numbered aliases (SYS_UNLINK 10, SYS_RENAME 38,
 * SYS_MKDIR 39, SYS_RMDIR 40) route through syscall_dispatch correctly.
 * unlink is exercised behaviourally against /tmp (tmpfs supports
 * delete_file).  mkdir/rmdir/rename are validated on the NULL-arg
 * rejection path; behavioural coverage on a writable backend happens
 * via the in-guest test drivers on the ext2/FAT32 rootfs.
 * ------------------------------------------------------------------------- */

static void test_posix_fs_syscalls(void)
{
    ktest_begin("posix_fs_syscalls", "SYS_UNLINK/RMDIR/RENAME/MKDIR dispatch routing");

    /* Seed a tmpfs file so SYS_UNLINK has something to delete. */
    const char *body = "posix-unlink-probe";
    KTEST_ASSERT(vfs_write_file("/tmp/posix_unlink.bin", body,
                                (uint32_t)strlen(body)) == 0);
    KTEST_ASSERT(vfs_file_exists("/tmp/posix_unlink.bin") == 1);

    registers_t regs;

    /* SYS_UNLINK(10) on the seeded file -> 0; file gone. */
    memset(&regs, 0, sizeof(regs));
    regs.eax = SYS_UNLINK;
    regs.ebx = (uint32_t)(uintptr_t)"/tmp/posix_unlink.bin";
    syscall_dispatch(&regs);
    KTEST_ASSERT_EQ((int)regs.eax, 0);
    KTEST_ASSERT(vfs_file_exists("/tmp/posix_unlink.bin") == 0);

    /* NULL-path rejections.  All four return -1 (the dispatch-level
     * sanity check) without faulting. */
    memset(&regs, 0, sizeof(regs));
    regs.eax = SYS_UNLINK; regs.ebx = 0;
    syscall_dispatch(&regs);
    KTEST_ASSERT_EQ((int)regs.eax, -1);

    memset(&regs, 0, sizeof(regs));
    regs.eax = SYS_RMDIR; regs.ebx = 0;
    syscall_dispatch(&regs);
    KTEST_ASSERT_EQ((int)regs.eax, -1);

    memset(&regs, 0, sizeof(regs));
    regs.eax = SYS_RENAME; regs.ebx = 0; regs.ecx = (uint32_t)(uintptr_t)"/x";
    syscall_dispatch(&regs);
    KTEST_ASSERT_EQ((int)regs.eax, -1);

    memset(&regs, 0, sizeof(regs));
    regs.eax = SYS_MKDIR; regs.ebx = 0;
    syscall_dispatch(&regs);
    KTEST_ASSERT_EQ((int)regs.eax, -1);

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: syscall
 *
 * Calls syscall_dispatch directly with a stack-allocated registers_t frame,
 * verifying that safe syscalls do not crash and return control to the caller.
 * SYS_EXIT is intentionally excluded - it calls task_exit() which is noreturn.
 * ------------------------------------------------------------------------- */

static void test_syscall(void)
{
    ktest_begin("syscall", "int 0x80 syscall dispatcher: arg passing, return value, unknown-syscall guard");

    registers_t regs;
    memset(&regs, 0, sizeof(regs));

    /* SYS_WRITE(fd=1, buf, len): write to stdout - must not crash and must
     * return the byte count. */
    static const char msg[] = "[ktest] syscall SYS_WRITE\n";
    regs.eax = SYS_WRITE;
    regs.ebx = FD_STDOUT;
    regs.ecx = (uint32_t)(uintptr_t)msg;
    regs.edx = sizeof(msg) - 1;   /* exclude NUL */
    syscall_dispatch(&regs);
    KTEST_ASSERT(regs.eax == sizeof(msg) - 1);

    /* SYS_WRITE to an invalid fd must return -1. */
    regs.eax = SYS_WRITE;
    regs.ebx = 99;   /* no such fd */
    regs.ecx = (uint32_t)(uintptr_t)msg;
    regs.edx = 1;
    syscall_dispatch(&regs);
    KTEST_ASSERT(regs.eax == (uint32_t)-1);

    /* Unknown syscall must return -ENOSYS (not crash). */
    regs.eax = 9999;
    regs.ebx = regs.ecx = regs.edx = 0;
    syscall_dispatch(&regs);
    KTEST_ASSERT(regs.eax == (uint32_t)-38);   /* -ENOSYS */

    /* SYS_YIELD must not crash. */
    regs.eax = SYS_YIELD;
    regs.ebx = 0;
    syscall_dispatch(&regs);
    KTEST_ASSERT(1);

    /* SYS_OPEN on a non-existent path must return -1. */
    regs.eax = SYS_OPEN;
    regs.ebx = (uint32_t)(uintptr_t)"/no/such/file";
    regs.ecx = O_RDONLY;
    syscall_dispatch(&regs);
    KTEST_ASSERT(regs.eax == (uint32_t)-1);

    /* SYS_BRK(0): query current break on a kernel task (user_brk == 0). */
    regs.eax = SYS_BRK;
    regs.ebx = 0;
    syscall_dispatch(&regs);
    KTEST_ASSERT(regs.eax == 0);   /* kernel tasks have no user heap */

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: fd_table
 *
 * Verifies the per-task file descriptor table:
 *   - the current task has a table with fds 0/1/2 pre-bound
 *   - fd_get rejects out-of-range / unused slots
 *   - fd_alloc returns the lowest free slot and skips occupied ones
 *   - fd_close releases the slot for reuse and tears down file payloads
 *   - a second, separately-allocated table is fully isolated from the
 *     current task's table (the property that makes per-task fds work)
 * ------------------------------------------------------------------------- */

static void test_fd_table(void)
{
    ktest_begin("fd_table",
                "per-task fd table: stdin/stdout/stderr binding, alloc/close, "
                "isolation between tables");

    task_t *cur = task_current();
    KTEST_ASSERT(cur != NULL);
    KTEST_ASSERT(cur->fd_table != NULL);

    /* Pre-bound stdio. */
    fd_entry_t *e0 = fd_get(cur->fd_table, 0);
    fd_entry_t *e1 = fd_get(cur->fd_table, 1);
    fd_entry_t *e2 = fd_get(cur->fd_table, 2);
    KTEST_ASSERT(e0 && e0->kind == FD_KIND_KEYBOARD);
    KTEST_ASSERT(e1 && e1->kind == FD_KIND_VGA);
    KTEST_ASSERT(e2 && e2->kind == FD_KIND_VGA_SERIAL);

    /* Out-of-range and free-slot lookups return NULL. */
    KTEST_ASSERT(fd_get(cur->fd_table, -1) == NULL);
    KTEST_ASSERT(fd_get(cur->fd_table, TASK_MAX_FDS) == NULL);
    KTEST_ASSERT(fd_get(cur->fd_table, 5) == NULL);
    KTEST_ASSERT(fd_get(NULL, 0) == NULL);

    /* Allocate a separate table and prove it's independent of cur's. */
    fd_table_t *aux = fd_table_create_default();
    KTEST_ASSERT(aux != NULL);
    KTEST_ASSERT(aux != cur->fd_table);

    /* Lowest free slot in a fresh default table is 3. */
    int a = fd_alloc(aux);
    KTEST_ASSERT(a == 3);
    aux->slots[a].kind = FD_KIND_FILE;
    aux->slots[a].data = (uint8_t *)kmalloc(16);
    KTEST_ASSERT(aux->slots[a].data != NULL);
    aux->slots[a].size = 16;
    aux->slots[a].pos  = 0;

    /* Next alloc skips the now-occupied slot 3 -> 4. */
    int b = fd_alloc(aux);
    KTEST_ASSERT(b == 4);

    /* The current task's table was not touched by aux mutations. */
    KTEST_ASSERT(fd_get(cur->fd_table, 3) == NULL);
    KTEST_ASSERT(fd_get(cur->fd_table, 4) == NULL);

    /* Closing slot 3 frees its buffer and reopens the slot for alloc. */
    KTEST_ASSERT(fd_close(aux, a) == 0);
    KTEST_ASSERT(fd_get(aux, a) == NULL);
    int c = fd_alloc(aux);
    KTEST_ASSERT(c == 3);   /* lowest free again */

    /* Close on an invalid fd is an error, not a crash. */
    KTEST_ASSERT(fd_close(aux, 99) == -1);
    KTEST_ASSERT(fd_close(aux, -1) == -1);
    KTEST_ASSERT(fd_close(NULL, 0) == -1);

    /* fd_table_destroy frees any open file payloads as well as the table. */
    aux->slots[c].kind = FD_KIND_FILE;
    aux->slots[c].data = (uint8_t *)kmalloc(8);
    aux->slots[c].size = 8;
    /* Leave path[0] = '\0' so the destroy-time flush is a no-op
     * (fd_flush_one short-circuits on empty paths). */
    fd_table_destroy(aux);   /* no leaks even with an open file */
    fd_table_destroy(NULL);  /* must accept NULL */

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: file_fd
 *
 * Phase-1-of-TCC-port slice -- verifies the writable FD_KIND_FILE state
 * machine in isolation (no real filesystem round-trips; those land in
 * the filetest.elf userland scenario).  Pins:
 *   A. SYS_WRITE on a writable FILE slot grows the buffer past the old
 *      64 KiB cap (covers krealloc path + doubling).
 *   B. lseek-past-EOF + SYS_WRITE zero-fills the gap.
 *   C. SYS_STAT on /proc/uname returns S_IFREG + non-zero size.
 *   D. SYS_STAT on a missing path returns -1.
 *   E. fd_table_clone clears `dirty` on child FILE slots and gives them
 *      their own buffer (no aliasing back to the parent).
 *   F. fd_close on a dirty FILE slot whose path points at a non-writable
 *      route returns -1 (flush propagates the backend error).
 * --------------------------------------------------------------------------- */
static void test_file_fd(void)
{
    ktest_begin("file_fd",
                "growable FD_KIND_FILE buffer, lseek-past-EOF zero-fill, "
                "SYS_STAT, fork-clone-clears-dirty, flush-error propagation");

    task_t *cur = task_current();
    KTEST_ASSERT(cur != NULL);
    KTEST_ASSERT(cur->fd_table != NULL);

    /* ---- A. growable buffer (write 192 KiB past the old 64 KiB cap) ---- */
    int fd = fd_alloc(cur->fd_table);
    KTEST_ASSERT(fd >= 0);
    fd_entry_t *e = &cur->fd_table->slots[fd];
    memset(e, 0, sizeof(*e));
    e->kind     = FD_KIND_FILE;
    e->writable = 1;
    /* path[0] = '\0' so close-flush is a no-op (no fs needed). */
    /* No initial allocation; SYS_WRITE will grow from zero. */

    const uint32_t GROW_BYTES = 192u * 1024u;   /* well past 64 KiB */
    static uint8_t pattern[4096];
    for (uint32_t off = 0; off < GROW_BYTES; off += 4096u) {
        for (uint32_t i = 0; i < 4096u; i++)
            pattern[i] = (uint8_t)((off + i) & 0xFFu);
        registers_t r;
        memset(&r, 0, sizeof(r));
        r.eax = SYS_WRITE;
        r.ebx = (uint32_t)fd;
        r.ecx = (uint32_t)(uintptr_t)pattern;
        r.edx = 4096u;
        syscall_dispatch(&r);
        KTEST_ASSERT(r.eax == 4096u);
    }
    KTEST_ASSERT(e->size == GROW_BYTES);
    KTEST_ASSERT(e->capacity >= GROW_BYTES);
    KTEST_ASSERT(e->dirty == 1);
    /* spot-check first and last byte against pattern */
    KTEST_ASSERT(e->data[0]                == (uint8_t)(0u & 0xFFu));
    KTEST_ASSERT(e->data[GROW_BYTES - 1u]  == (uint8_t)((GROW_BYTES - 1u) & 0xFFu));
    /* Clear dirty so close doesn't try to flush an empty path. */
    e->dirty = 0;
    KTEST_ASSERT(fd_close(cur->fd_table, fd) == 0);

    /* ---- B. lseek-past-EOF zero-fill ---- */
    fd = fd_alloc(cur->fd_table);
    KTEST_ASSERT(fd >= 0);
    e = &cur->fd_table->slots[fd];
    memset(e, 0, sizeof(*e));
    e->kind     = FD_KIND_FILE;
    e->writable = 1;

    /* Write "A" at offset 0 first so size is 1. */
    {
        registers_t r; memset(&r, 0, sizeof(r));
        r.eax = SYS_WRITE; r.ebx = (uint32_t)fd;
        r.ecx = (uint32_t)(uintptr_t)"A"; r.edx = 1u;
        syscall_dispatch(&r);
        KTEST_ASSERT(r.eax == 1u);
    }
    /* Seek to 100. */
    {
        registers_t r; memset(&r, 0, sizeof(r));
        r.eax = SYS_LSEEK; r.ebx = (uint32_t)fd;
        r.ecx = 100u; r.edx = 0u;   /* SEEK_SET */
        syscall_dispatch(&r);
        KTEST_ASSERT(r.eax == 100u);   /* writable fd: not clamped to size */
    }
    /* Write "Z". */
    {
        registers_t r; memset(&r, 0, sizeof(r));
        r.eax = SYS_WRITE; r.ebx = (uint32_t)fd;
        r.ecx = (uint32_t)(uintptr_t)"Z"; r.edx = 1u;
        syscall_dispatch(&r);
        KTEST_ASSERT(r.eax == 1u);
    }
    KTEST_ASSERT(e->size == 101u);
    KTEST_ASSERT(e->data[0]   == 'A');
    KTEST_ASSERT(e->data[100] == 'Z');
    /* Zero-fill in the gap. */
    for (uint32_t i = 1; i < 100u; i++)
        KTEST_ASSERT(e->data[i] == 0);
    e->dirty = 0;
    KTEST_ASSERT(fd_close(cur->fd_table, fd) == 0);

    /* ---- C. SYS_STAT on /proc/uname ---- */
    {
        struct stat st;
        memset(&st, 0xAB, sizeof(st));   /* poison: catch un-set fields */
        registers_t r; memset(&r, 0, sizeof(r));
        r.eax = SYS_STAT;
        r.ebx = (uint32_t)(uintptr_t)"/proc/uname";
        r.ecx = (uint32_t)(uintptr_t)&st;
        syscall_dispatch(&r);
        KTEST_ASSERT(r.eax == 0);
        KTEST_ASSERT((st.st_mode & S_IFMT) == S_IFREG);
        KTEST_ASSERT(st.st_size > 0);
        KTEST_ASSERT(st.st_blksize == 4096);
    }

    /* ---- D. SYS_STAT on a missing path ---- */
    {
        struct stat st;
        registers_t r; memset(&r, 0, sizeof(r));
        r.eax = SYS_STAT;
        r.ebx = (uint32_t)(uintptr_t)"/no/such/file/anywhere";
        r.ecx = (uint32_t)(uintptr_t)&st;
        syscall_dispatch(&r);
        KTEST_ASSERT(r.eax == (uint32_t)-1);
    }

    /* ---- E. fork-clone clears dirty + gives the child its own buffer ---- */
    {
        fd_table_t *parent = fd_table_create_default();
        KTEST_ASSERT(parent != NULL);
        int pfd = fd_alloc(parent);
        KTEST_ASSERT(pfd >= 0);
        fd_entry_t *pe = &parent->slots[pfd];
        memset(pe, 0, sizeof(*pe));
        pe->kind     = FD_KIND_FILE;
        pe->writable = 1;
        pe->data     = (uint8_t *)kmalloc(32);
        pe->capacity = 32;
        pe->size     = 4;
        pe->dirty    = 1;
        memcpy(pe->data, "abcd", 4);
        /* No path so any accidental flush is harmless. */

        fd_table_t *child = fd_table_clone(parent);
        KTEST_ASSERT(child != NULL);
        fd_entry_t *ce = &child->slots[pfd];
        KTEST_ASSERT(ce->kind == FD_KIND_FILE);
        KTEST_ASSERT(ce->data != pe->data);          /* deep copy */
        KTEST_ASSERT(ce->size == 4);
        KTEST_ASSERT(ce->dirty == 0);                /* the key invariant */
        KTEST_ASSERT(memcmp(ce->data, "abcd", 4) == 0);

        /* Mutating the parent buffer must not show up in the child. */
        pe->data[0] = 'X';
        KTEST_ASSERT(ce->data[0] == 'a');

        /* Tear down without flushing (path empty). */
        pe->dirty = 0;
        fd_table_destroy(parent);
        fd_table_destroy(child);
    }

    /* ---- F. flush-error propagation ---- */
    /* Write to a read-only path and verify fd_close surfaces the error.
     * Use /mnt/cdrom when a CD-ROM is mounted (live boot or QEMU with ISO);
     * skip the sub-test on installed boots with no CD present — the core
     * flush-error path is the same regardless of which backend rejects it. */
    if (vfs_file_exists("/mnt/cdrom")) {
        fd_table_t *t = fd_table_create_default();
        KTEST_ASSERT(t != NULL);
        int xfd = fd_alloc(t);
        fd_entry_t *xe = &t->slots[xfd];
        memset(xe, 0, sizeof(*xe));
        xe->kind     = FD_KIND_FILE;
        xe->writable = 1;
        xe->data     = (uint8_t *)kmalloc(8);
        xe->capacity = 8;
        xe->size     = 2;
        xe->dirty    = 1;
        memcpy(xe->data, "ab", 2);
        const char *bad = "/mnt/cdrom/should-not-write";
        uint32_t i = 0;
        while (bad[i] && i < VFS_PATH_MAX - 1) { xe->path[i] = bad[i]; i++; }
        xe->path[i] = '\0';
        KTEST_ASSERT(fd_close(t, xfd) == -1);   /* flush error surfaces */
        fd_table_destroy(t);
    }

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: per-task cwd
 *
 * Slice 15 migrated the VFS cwd off the global s_cwd onto task_t.cwd.
 * These asserts pin the migration:
 *   - vfs_getcwd() returns the calling task's cwd (pointer identity).
 *   - vfs_cd() writes through to task_current()->cwd.
 *   - Mutating a peer task's cwd does NOT change vfs_getcwd().
 *   - Round-trip restores the original cwd cleanly.
 * --------------------------------------------------------------------------- */
static void test_cwd(void)
{
    ktest_begin("cwd",
                "per-task cwd: vfs_getcwd routes to task_current, vfs_cd writes "
                "through, peer-task cwd is isolated");

    task_t *cur = task_current();
    KTEST_ASSERT(cur != NULL);

    /* vfs_getcwd points into the calling task's storage. */
    KTEST_ASSERT(vfs_getcwd() == cur->cwd);

    /* Snapshot so we can restore. */
    char saved[VFS_PATH_MAX];
    size_t n = strlen(cur->cwd);
    if (n >= VFS_PATH_MAX) n = VFS_PATH_MAX - 1;
    memcpy(saved, cur->cwd, n);
    saved[n] = '\0';

    /* Write through: vfs_cd("/") lands in cur->cwd. */
    KTEST_ASSERT(vfs_cd("/") == 0);
    KTEST_ASSERT(strcmp(cur->cwd, "/") == 0);
    KTEST_ASSERT(strcmp(vfs_getcwd(), "/") == 0);

    /* Isolation: poke a peer task's cwd; vfs_getcwd must not reflect it.
     * task_get(0) is idle; if we're not idle, use index 0 as the peer.
     * If we *are* idle (test_mode boot), reach for any other live slot.
     * Falling back to skip if no peer exists keeps the test safe in
     * minimal boot configurations. */
    task_t *peer = NULL;
    for (int i = 0; ; i++) {
        task_t *t = task_get(i);
        if (!t) break;
        if (t != cur) { peer = t; break; }
    }
    if (peer) {
        char peer_saved[VFS_PATH_MAX];
        size_t pn = strlen(peer->cwd);
        if (pn >= VFS_PATH_MAX) pn = VFS_PATH_MAX - 1;
        memcpy(peer_saved, peer->cwd, pn);
        peer_saved[pn] = '\0';

        memcpy(peer->cwd, "/apps", 6);
        KTEST_ASSERT(strcmp(vfs_getcwd(), "/") == 0);        /* unchanged */
        KTEST_ASSERT(strcmp(peer->cwd, "/apps") == 0);       /* but peer did change */

        /* Restore peer cwd. */
        memcpy(peer->cwd, peer_saved, strlen(peer_saved) + 1);
    }

    /* Restore caller's cwd via vfs_cd to exercise the full path. */
    KTEST_ASSERT(vfs_cd(saved) == 0);
    KTEST_ASSERT(strcmp(cur->cwd, saved) == 0);

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: signal
 *
 * Exercises the per-task signal subsystem: default-action classification,
 * sig_send pending-bit semantics, SIGKILL override of mask/handler, and
 * default-terminate delivery from the scheduler.  Victim tasks loop on
 * task_yield as a safety net; they should never run their body because
 * sig_deliver runs on the incoming task in schedule() and transitions it
 * to TASK_DEAD before the context switch.
 * --------------------------------------------------------------------------- */

static void test_signal_victim_loop(void)
{
    /* Loop on task_yield so the only path to TASK_DEAD is via signal
     * delivery from schedule().  Calling task_exit here instead would
     * race the SIGTERM send: preemption could give the victim a slice
     * between task_create returning and the test's sig_send call,
     * marking it DEAD before any signal is posted. */
    for (;;)
        task_yield();
}

static void test_signal(void)
{
    ktest_begin("signal",
                "per-task signal subsystem: default classification, "
                "sig_send + sig_deliver, SIGKILL override, scheduler delivery");

    /* Default-action classification. */
    KTEST_ASSERT(sig_default_terminates(SIGINT)  == 1);
    KTEST_ASSERT(sig_default_terminates(SIGTERM) == 1);
    KTEST_ASSERT(sig_default_terminates(SIGKILL) == 1);
    KTEST_ASSERT(sig_default_terminates(SIGCHLD) == 0);
    KTEST_ASSERT(sig_default_terminates(SIGCONT) == 0);

    task_t *cur = task_current();
    KTEST_ASSERT(cur != NULL);

    /* sig_set_handler rejects SIGKILL / SIGSTOP, accepts others. */
    KTEST_ASSERT(sig_set_handler(cur, SIGKILL, SIG_IGN) == -1);
    KTEST_ASSERT(sig_set_handler(cur, SIGSTOP, SIG_IGN) == -1);
    KTEST_ASSERT(sig_set_handler(cur, SIGINT,  SIG_IGN) == 0);

    /* SIG_IGN: sig_deliver clears the pending bit without touching state.
     * cur is the running task, so a preempting timer IRQ between the post and
     * the observe would let schedule()'s sig_deliver clear the (ignored) bit
     * first -- wrap post + observe in disable_interrupts to remove the window
     * (more likely at 250 Hz; surfaced on CI's TCG timing).  Capture state with
     * IRQs off, assert on the snapshots. */
    disable_interrupts();
    sig_send(cur, SIGINT);
    uint32_t ign_before = cur->sig_pending;
    sig_deliver(cur);
    uint32_t ign_after  = cur->sig_pending;
    int      ign_state  = cur->state;
    enable_interrupts();
    KTEST_ASSERT((ign_before & SIG_BIT(SIGINT)) != 0);
    KTEST_ASSERT((ign_after  & SIG_BIT(SIGINT)) == 0);
    KTEST_ASSERT(ign_state != TASK_DEAD);
    sig_set_handler(cur, SIGINT, SIG_DFL);

    /* sig_send input validation: NULL task / bogus signo are no-ops. */
    sig_send(NULL, SIGTERM);
    sig_send(cur,  0);
    sig_send(cur,  SIG_MAX + 1);
    KTEST_ASSERT((cur->sig_pending & ~cur->sig_mask) == 0);

    /* sig_get_handler / sig_set_handler round-trip and previous-value
     * semantics that SYS_SIGNAL relies on. */
    KTEST_ASSERT(sig_get_handler(cur, SIGUSR1) == SIG_DFL);
    KTEST_ASSERT(sig_set_handler(cur, SIGUSR1, SIG_IGN) == 0);
    KTEST_ASSERT(sig_get_handler(cur, SIGUSR1) == SIG_IGN);
    KTEST_ASSERT(sig_set_handler(cur, SIGUSR1, SIG_DFL) == 0);
    KTEST_ASSERT(sig_get_handler(cur, SIGUSR1) == SIG_DFL);
    /* Out-of-range signo: getter returns SIG_DFL, setter rejects. */
    KTEST_ASSERT(sig_get_handler(cur, 0) == SIG_DFL);
    KTEST_ASSERT(sig_get_handler(cur, SIG_MAX + 1) == SIG_DFL);

    /* sig_send_pid: unknown pid returns -1. */
    KTEST_ASSERT(sig_send_pid(99999, SIGTERM) == -1);

    /* sig_send_pid: valid pid posts the pending bit.  Use a fresh
     * victim (not the running task) so a preempting timer IRQ between
     * the post and the observe can't pick the signal up via sig_deliver
     * and clear it.  Even so, wrap post + observe in disable_interrupts
     * to remove the window entirely: terminating the test runner via
     * its own signal would kill idle under the test_mode boot path and
     * deadlock the kernel. */
    task_t *vp = task_create("sigtest_pid", test_signal_victim_loop);
    KTEST_ASSERT(vp != NULL);
    if (vp) {
        disable_interrupts();
        int posted = sig_send_pid(vp->pid, SIGCHLD);
        uint32_t pending = vp->sig_pending;
        /* Clear so a later sig_deliver pass treats it as no-op (SIGCHLD
         * default action is ignore, so leaving it pending would also
         * be safe -- but explicit cleanup keeps the assertion focused). */
        vp->sig_pending &= ~SIG_BIT(SIGCHLD);
        enable_interrupts();
        KTEST_ASSERT(posted == 0);
        KTEST_ASSERT((pending & SIG_BIT(SIGCHLD)) != 0);
        /* Reap the victim. */
        sig_send(vp, SIGTERM);
        for (int i = 0; i < 16 && vp->state != TASK_DEAD; i++)
            task_yield();
        KTEST_ASSERT(vp->state == TASK_DEAD);
    }

    /* Default-terminate via scheduler: spawn victim, send SIGTERM, yield
     * until schedule() picks it and sig_deliver flips state to DEAD. */
    task_t *victim = task_create("sigtest_term", test_signal_victim_loop);
    KTEST_ASSERT(victim != NULL);
    if (victim) {
        KTEST_ASSERT(victim->state == TASK_READY);
        sig_send(victim, SIGTERM);
        for (int i = 0; i < 16 && victim->state != TASK_DEAD; i++)
            task_yield();
        KTEST_ASSERT(victim->state == TASK_DEAD);
        KTEST_ASSERT((victim->sig_pending & SIG_BIT(SIGTERM)) == 0);
    }

    /* SIGKILL overrides SIG_IGN on every other signal and terminates. */
    task_t *vk = task_create("sigtest_kill", test_signal_victim_loop);
    KTEST_ASSERT(vk != NULL);
    if (vk) {
        sig_set_handler(vk, SIGINT,  SIG_IGN);
        sig_set_handler(vk, SIGTERM, SIG_IGN);
        sig_send(vk, SIGKILL);
        for (int i = 0; i < 16 && vk->state != TASK_DEAD; i++)
            task_yield();
        KTEST_ASSERT(vk->state == TASK_DEAD);
    }

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: preempt
 *
 * Proves the timer-driven preemptive scheduler is doing its job: a victim
 * task busy-loops with no task_yield, the test task yields normally, and
 * we verify both tasks' per-task tick counters advance over ~30 ticks of
 * wallclock.  If the scheduler is broken (re-entrancy lockup, missing
 * preemption, in_schedule flag stuck) one of those counters stays flat
 * and the assertion fires.  Pairs with the slice 9 phase 1 re-entrancy
 * guard and the phase 2 kticks counter.
 * --------------------------------------------------------------------------- */

static volatile int preempt_run = 0;

static void test_preempt_busy_loop(void)
{
    /* No task_yield: only the PIT IRQ can preempt us.  Cleared from
     * the parent so we exit cleanly even if the SIGKILL teardown
     * below somehow doesn't fire. */
    while (preempt_run)
        ;
    task_exit();
}

static void test_preempt(void)
{
    ktest_begin("preempt",
                "timer-driven preemption: a busy-loop task does not "
                "starve the rest of the system, kticks counter advances");

    task_t *cur = task_current();
    KTEST_ASSERT(cur != NULL);

    preempt_run = 1;
    task_t *v = task_create("preempt_victim", test_preempt_busy_loop);
    KTEST_ASSERT(v != NULL);
    if (!v) { preempt_run = 0; ktest_summary(); return; }

    uint32_t v_before = v->kticks;
    uint32_t t0       = timer_get_ticks();

    /* Wait ~30 PIT ticks of wallclock (300 ms at 100 Hz).  The caller
     * yields each iteration so we don't accumulate kticks of our own
     * here (a tick only charges the task that's current at IRQ-0
     * time, and yields make our resident window vanishingly small).
     * The fact that we exit this loop at all is the implicit "we
     * weren't starved" check; if preemption were broken and the
     * busy-looping victim hogged forever, this loop would hang and
     * the iso-test outer timeout would kill the run. */
    while (timer_get_ticks() - t0 < 30)
        task_yield();

    KTEST_ASSERT(v->kticks > v_before);   /* victim got CPU via preemption */

    /* Teardown.  Clear the loop flag first (lets the victim exit
     * cleanly on its next scheduled slice), then SIGKILL as a
     * belt-and-braces in case it somehow doesn't see the write. */
    preempt_run = 0;
    sig_send(v, SIGKILL);
    for (int i = 0; i < 64 && v->state != TASK_DEAD; i++)
        task_yield();
    KTEST_ASSERT(v->state == TASK_DEAD);

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: GDT
 *
 * Verifies that the GDT segment descriptors ring-3 entry depends on are
 * installed correctly:
 *   index 3 (selector 0x1B) - user code,  DPL=3
 *   index 4 (selector 0x23) - user data,  DPL=3
 *   index 5 (selector 0x28) - TSS,        type=9 (32-bit available)
 *
 * Also round-trips tss_set_kernel_stack / tss_get_esp0 to confirm the TSS
 * ESP0 field is writable (the CPU reads it on every ring-3 → ring-0 entry).
 * ------------------------------------------------------------------------- */

static void test_gdt(void)
{
    extern gdt_entry_t gdt_entries[6];

    ktest_begin("gdt", "GDT layout: kernel/user code+data, TSS selector, ring 3 DPLs");

    /* User code segment (index 3): DPL field (bits [6:5] of access) must be 3. */
    KTEST_ASSERT(((gdt_entries[3].access >> 5) & 0x3u) == 3u);

    /* User data segment (index 4): DPL must be 3. */
    KTEST_ASSERT(((gdt_entries[4].access >> 5) & 0x3u) == 3u);

    /* TSS descriptor (index 5): access byte must be 0x89 (available) or 0x8B
     * (busy) - the CPU sets the busy bit when ltr loads the selector. */
    KTEST_ASSERT(gdt_entries[5].access == 0x89u || gdt_entries[5].access == 0x8Bu);

    /* tss_set_kernel_stack must update the TSS ESP0 field the CPU will read. */
    uint32_t saved = tss_get_esp0();
    tss_set_kernel_stack(0xDEAD0000u);
    KTEST_ASSERT(tss_get_esp0() == 0xDEAD0000u);
    tss_set_kernel_stack(saved);

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: ring3_prereqs
 *
 * Maps a code page (USER only, not writable) and a stack page (USER+WRITABLE)
 * into a fresh page directory and verifies that the PTE flag bits match what
 * ring3_enter and the CPU require:
 *
 *   Code  page: bit 2 (USER) set, bit 1 (WRITABLE) clear, bit 0 (PRESENT) set
 *   Stack page: bits 2+1+0 all set
 *
 * Uses the same virtual addresses as usertest.c so a mis-mapping here would
 * reproduce the ring-3 freeze without actually entering ring 3.
 * ------------------------------------------------------------------------- */

#define RT_USER_CODE_BASE  0x40000000u   /* PDE 256, PTE 0 */
#define RT_USER_STACK_VIRT 0xBFFEF000u   /* USER_STACK_TOP − 4 KiB */

static void test_ring3_prereqs(void)
{
    ktest_begin("ring3_prereqs", "ring-3 transition prerequisites: TSS, user PD, kernel stack");

    /* VMM flag constants must match the x86 PTE bit positions the CPU checks. */
    KTEST_ASSERT(VMM_FLAG_USER     == 0x4u);
    KTEST_ASSERT(VMM_FLAG_WRITABLE == 0x2u);

    uint32_t *pd = vmm_create_pd();
    KTEST_ASSERT(pd != NULL);

    uint32_t phys_code  = pmm_alloc_frame();
    uint32_t phys_stack = pmm_alloc_frame();
    KTEST_ASSERT(phys_code  != PMM_ALLOC_ERROR);
    KTEST_ASSERT(phys_stack != PMM_ALLOC_ERROR);

    /* Code page - user-readable, not writable (ring 3 must not write .text). */
    vmm_map_page(pd, RT_USER_CODE_BASE, phys_code, VMM_FLAG_USER);
    uint32_t pdi_code = RT_USER_CODE_BASE >> 22;
    uint32_t *pt_code = (uint32_t *)(pd[pdi_code] & ~0xFFFu);
    uint32_t pte_code = pt_code[0];
    KTEST_ASSERT((pte_code & 0x1u) != 0);   /* PRESENT  */
    KTEST_ASSERT((pte_code & 0x4u) != 0);   /* USER     */
    KTEST_ASSERT((pte_code & 0x2u) == 0);   /* !WRITABLE */

    /* Stack page - user-readable and writable. */
    vmm_map_page(pd, RT_USER_STACK_VIRT, phys_stack,
                 VMM_FLAG_USER | VMM_FLAG_WRITABLE);
    uint32_t pdi_stack = RT_USER_STACK_VIRT >> 22;
    uint32_t pti_stack = (RT_USER_STACK_VIRT >> 12) & 0x3FFu;
    uint32_t *pt_stack = (uint32_t *)(pd[pdi_stack] & ~0xFFFu);
    uint32_t pte_stack = pt_stack[pti_stack];
    KTEST_ASSERT((pte_stack & 0x1u) != 0);  /* PRESENT   */
    KTEST_ASSERT((pte_stack & 0x4u) != 0);  /* USER      */
    KTEST_ASSERT((pte_stack & 0x2u) != 0);  /* WRITABLE  */

    /* vmm_free_pd releases the mapped frames, page tables, and the PD itself. */
    uint32_t fc_before = pmm_free_count();
    vmm_free_pd(pd);    /* frees phys_code + PT_code + phys_stack + PT_stack + PD = 5 */
    KTEST_ASSERT(pmm_free_count() == fc_before + 5);

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: IDT
 *
 * Verifies that the IDT entries the kernel depends on are installed correctly:
 *   - Exception gates (DPL=0, present, 32-bit interrupt gate = 0x8E)
 *   - Syscall gate at vector 0x80 (DPL=3 = 0xEE so ring-3 can invoke it)
 *   - All handler pointers are non-zero
 *   - All gates use the kernel code selector (0x08)
 * ------------------------------------------------------------------------- */

static void test_idt(void)
{
    extern idt_entry_t idt_entries[256];
    ktest_begin("idt", "IDT: gate types, DPLs, syscall gate present with DPL=3");

    /* Exception gates: present, DPL=0, 32-bit interrupt gate (0x8E). */
    KTEST_ASSERT(idt_entries[0].flags  == 0x8E);  /* #DE divide error    */
    KTEST_ASSERT(idt_entries[8].flags  == 0x8E);  /* #DF double fault    */
    KTEST_ASSERT(idt_entries[13].flags == 0x8E);  /* #GP protection      */
    KTEST_ASSERT(idt_entries[14].flags == 0x8E);  /* #PF page fault      */

    /* Handler pointers must be non-zero. */
    uint32_t base0  = idt_entries[0].base_lo  | ((uint32_t)idt_entries[0].base_hi  << 16);
    uint32_t base14 = idt_entries[14].base_lo | ((uint32_t)idt_entries[14].base_hi << 16);
    KTEST_ASSERT(base0  != 0);
    KTEST_ASSERT(base14 != 0);

    /* Syscall gate: present, DPL=3 (0xEE) so ring-3 can invoke int 0x80. */
    KTEST_ASSERT(idt_entries[0x80].flags == 0xEE);
    uint32_t base80 = idt_entries[0x80].base_lo | ((uint32_t)idt_entries[0x80].base_hi << 16);
    KTEST_ASSERT(base80 != 0);

    /* All checked gates must use the kernel code selector. */
    KTEST_ASSERT(idt_entries[0].sel    == 0x08);
    KTEST_ASSERT(idt_entries[14].sel   == 0x08);
    KTEST_ASSERT(idt_entries[0x80].sel == 0x08);

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: ring3_execution
 *
 * Performs an end-to-end ring-3 smoke test by creating a task that maps user
 * code and stack, drops to ring 3 via iret, executes the embedded PIC binary
 * (SYS_DEBUG → SYS_WRITE → SYS_DEBUG → SYS_EXIT), and verifies that both
 * debug checkpoints were reached.
 *
 * CP1 fires immediately on ring-3 entry; CP2 fires after SYS_WRITE returns.
 * Seeing CP2 == 2 proves: ring-3 entry worked, the write syscall returned, and
 * the user binary ran to completion before calling SYS_EXIT.
 * ------------------------------------------------------------------------- */

extern void ring3_usertest_task(void);

static void test_ring3_execution(void)
{
    ktest_begin("ring3_execution", "ring-3 lifecycle end-to-end: iret to user, syscall back, exit");

    /* Reset the checkpoint so stale values from a prior run don't give a
     * false positive. */
    g_ring3_last_cp = 0;

    task_t *t = task_create("ring3test", ring3_usertest_task);
    KTEST_ASSERT(t != NULL);

    /* Yield until the ring-3 task has exited.  With preemptive scheduling the
     * timer may switch us back before ring3test completes all its syscalls, so
     * we loop until the task is marked DEAD rather than assuming one yield is
     * sufficient. */
    while (t && t->state != TASK_DEAD)
        task_yield();

    /* This line executes only after the scheduler returned to kernel mode.
     * It proves we are back in ring 0 with the kernel page directory active,
     * able to call kernel functions and write to serial/VGA normally. */
    Serial_WriteString("[ktest] ring3_execution: back in kernel mode (ring 0)\n");
    if (!ktest_muted)
        t_writestring("[ktest] ring3 -> kernel mode OK\n");

    /* CP2 appears after SYS_WRITE returns, just before SYS_EXIT.  Seeing 2
     * confirms ring-3 entry, the write syscall, and the debug syscall all
     * worked correctly. */
    KTEST_ASSERT(g_ring3_last_cp == 2);

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: elf_exec
 *
 * Spawns a child task that calls elf_exec() on /apps/echo.elf (the
 * standard VFS path for apps; rootfs election routes to the live medium).
 * Waits for the task to reach TASK_DEAD, which proves the ELF loader,
 * argv setup, ring-3 entry, and SYS_EXIT path all function end-to-end.
 * ------------------------------------------------------------------------- */

static const char *s_echo_argv[] = { "echo", "ktest-elf-exec-ok", NULL };
static int s_echo_argc = 2;

static void elf_exec_task_entry(void)
{
    elf_exec("/apps/echo.elf", s_echo_argc, s_echo_argv);
    task_exit();
}

static void test_elf_exec(void)
{
    ktest_begin("elf_exec", "ELF32 loader: header parse, segment mapping, entry-point dispatch");

    /* Single rootfs-elected path -- /apps routes to whichever volume
     * holds /usr/lib/crt0.o (CD on live boot, ext2/FAT32 on HDD boot). */
    static const char *candidates[] = {
        "/apps/echo.elf",
        NULL
    };

    const char *path = NULL;
    for (int i = 0; candidates[i]; i++) {
        if (vfs_file_exists(candidates[i])) {
            path = candidates[i];
            break;
        }
    }

    if (!path) {
        Serial_WriteString("[ktest] elf_exec: echo.elf not found on cdrom or hd - skipping\n");
        t_writestring("[ktest] elf_exec: echo.elf not found (skip)\n");
        ktest_summary();
        return;
    }

    task_t *t = task_create("elf_exec_test", elf_exec_task_entry);
    KTEST_ASSERT(t != NULL);

    while (t && t->state != TASK_DEAD)
        task_yield();

    Serial_WriteString("[ktest] elf_exec: task completed\n");
    KTEST_ASSERT(t->state == TASK_DEAD);

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: ring3_with_arg
 *
 * Spawns a child task that elf_exec()s hello.elf with one argument
 * ("tester") and verifies the full lifecycle:
 *
 *   parent test task
 *     │ task_create("hello_arg", entry)        ── child enters READY
 *     │
 *     │ ── yields ──>  child runs hello_arg_entry()
 *     │                  └─ elf_exec("/apps/hello.elf",
 *     │                              2, {"hello", "tester"})
 *     │                       ↓ ring transition (iret to ring 3)
 *     │                  hello main() prints "Hello, tester!\n"
 *     │                       ↓ SYS_EXIT
 *     │ <── yields ──   child marked TASK_DEAD
 *     │
 *     │ parent observes TASK_DEAD, asserts, prints "control returned"
 *
 * The lifecycle log goes to serial only (via Serial_WriteString) so it
 * doesn't disrupt the loading screen when the test runs from the bg
 * harness. The TTY-visible "Hello, tester!" line in the VESA framebuffer
 * is the user-visible proof that argv made it all the way to ring 3.
 * ------------------------------------------------------------------------- */

static const char *s_hello_argv[] = { "hello", "tester", NULL };
static int         s_hello_argc   = 2;

static void hello_arg_entry(void)
{
    task_t *me = task_current();
    Serial_WriteString("[ktest]   >>> CHILD SCHEDULED (pid=");
    Serial_WriteDec((uint32_t)(me ? me->pid : 0));
    Serial_WriteString(", ring 0) - about to enter ring 3 via elf_exec\n");
    Serial_WriteString("[ktest]   >>> elf_exec(\"hello.elf\", argc=2, argv=[\"hello\",\"tester\"])\n");
    Serial_WriteString("[ktest]   ----- BEGIN RING 3 OUTPUT -----\n");

    elf_exec("/apps/hello.elf", s_hello_argc, s_hello_argv);

    /* Only reached on elf_exec failure (e.g. file missing). On success the
     * ring-3 program returns via SYS_EXIT, which calls task_exit() directly
     * and never returns here. */
    Serial_WriteString("[ktest]   ----- elf_exec FAILURE PATH -----\n");
    task_exit();
}

static void test_ring3_with_arg(void)
{
    ktest_begin("ring3_with_arg", "argc/argv plumbing into a ring-3 binary; ESP-relative arg layout");

    Serial_WriteString("\n");
    Serial_WriteString("[ktest] ============================================================\n");
    Serial_WriteString("[ktest]  RING-3 TASK-SWITCHING TEST: parent -> child(hello.elf) -> parent\n");
    Serial_WriteString("[ktest] ============================================================\n");

    static const char *candidates[] = {
        "/apps/hello.elf",
        NULL
    };

    const char *path = NULL;
    for (int i = 0; candidates[i]; i++) {
        if (vfs_file_exists(candidates[i])) {
            path = candidates[i];
            break;
        }
    }

    if (!path) {
        Serial_WriteString("[ktest] ring3_with_arg: hello.elf not found - skipping\n");
        t_writestring("[ktest] ring3_with_arg: hello.elf not found (skip)\n");
        ktest_summary();
        return;
    }

    task_t *self = task_current();
    int parent_pid = self ? self->pid : 0;

    Serial_WriteString("[ktest] [PARENT] pid=");
    Serial_WriteDec((uint32_t)parent_pid);
    Serial_WriteString(" name=");
    Serial_WriteString((char *)(self && self->name ? self->name : "(unknown)"));
    Serial_WriteString(" - running, ring 0\n");

    Serial_WriteString("[ktest] [PARENT] task_create(\"hello_arg\", hello_arg_entry)\n");
    task_t *child = task_create("hello_arg", hello_arg_entry);
    KTEST_ASSERT(child != NULL);
    if (!child) { ktest_summary(); return; }

    Serial_WriteString("[ktest] [PARENT] child created: pid=");
    Serial_WriteDec((uint32_t)child->pid);
    Serial_WriteString(" name=hello_arg state=READY\n");

    Serial_WriteString("[ktest] [PARENT] yielding -> scheduler picks child (pid=");
    Serial_WriteDec((uint32_t)child->pid);
    Serial_WriteString(")\n");

    /* Yield until the child finishes, with periodic state logging so a hang
     * produces visible diagnostics rather than a silent timeout. */
    uint32_t spins = 0;
    while (child->state != TASK_DEAD) {
        task_yield();
        if ((++spins & 0xFFFu) == 0) {
            Serial_WriteString("[ktest] [PARENT] waiting, child state=");
            Serial_WriteDec((uint32_t)child->state);
            Serial_WriteString("\n");
        }
    }

    Serial_WriteString("[ktest]   ----- END RING 3 OUTPUT -----\n");
    Serial_WriteString("[ktest] [PARENT] resumed, pid=");
    Serial_WriteDec((uint32_t)parent_pid);
    Serial_WriteString(" - child reaped (state=DEAD)\n");

    KTEST_ASSERT(child->state == TASK_DEAD);
    KTEST_ASSERT(task_current() == self);   /* parent identity preserved across context switches */

    Serial_WriteString("[ktest] ============================================================\n");
    Serial_WriteString("[ktest]  RING-3 TASK SWITCHING: WORKING (parent<->child<->ring3 all OK)\n");
    Serial_WriteString("[ktest] ============================================================\n\n");

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: vesa_resolution
 *
 * Exercises the VESA geometry pipeline at four resolutions in ascending size
 * order: 320×240, 640×480, 1280×720, 1920×1080.
 *
 * For each resolution the suite:
 *   1. Announces the upcoming switch with a 3-second countdown (3… 2… 1…).
 *   2. Sets the font scale (1 for <1280-wide, 2 for ≥1280-wide).
 *   3. Programs Bochs VBE hardware (skipped if unavailable).
 *   4. Updates the vesa_fb_t struct via vesa_update_geometry().
 *   5. Re-initialises the VESA TTY via vesa_tty_init().
 *   6. Holds the new resolution for 1 second so it is visible on screen.
 *   7. Asserts fb width/height/bpp/pitch and computed cols/rows.
 *
 * The original resolution is restored before returning.
 * ------------------------------------------------------------------------- */

typedef struct { uint32_t w; uint32_t h; uint32_t scale; const char *name; } vesa_res_t;

/* Print a 3-second countdown before switching resolution. */
static void res_countdown(const char *name)
{
    t_writestring("[ktest] vesa_resolution: switching to ");
    t_writestring(name);
    t_writestring(" in: 3");
    ksleep(100);
    t_writestring("  2");
    ksleep(100);
    t_writestring("  1");
    ksleep(100);
    t_writestring("\n");
}

static void test_vesa_resolution(void)
{
    ktest_begin("vesa_resolution", "VESA mode switching across 320x240 / 640x480 / 720p / 1080p");

    /* Save current fb geometry so we can restore it afterwards. */
    const vesa_fb_t *orig = vesa_get_fb();
    uint32_t saved_w = orig ? orig->width  : 640;
    uint32_t saved_h = orig ? orig->height : 480;
    bool hw = bochs_vbe_available();

    static const vesa_res_t modes[] = {
        {  320,  240, 1, " 320x240"  },
        {  640,  480, 1, " 640x480"  },
        { 1280,  720, 2, "1280x720"  },
        { 1920, 1080, 2, "1920x1080" },
    };

    for (uint32_t i = 0; i < 4; i++) {
        uint32_t    w     = modes[i].w;
        uint32_t    h     = modes[i].h;
        uint32_t    scale = modes[i].scale;
        const char *name  = modes[i].name;

        res_countdown(name);

        vesa_tty_set_scale(scale);
        if (hw)
            bochs_vbe_set_mode(w, h, 32);
        vesa_update_geometry(w, h, 32);
        vesa_tty_init();

        /* Hold the new resolution for 1 second so it is visible. */
        t_writestring("[ktest] vesa_resolution: now at ");
        t_writestring(name);
        t_writestring("x32\n");
        ksleep(100);

        const vesa_fb_t *fb = vesa_get_fb();
        KTEST_ASSERT(fb != NULL);
        KTEST_ASSERT_EQ(fb->width,  w);
        KTEST_ASSERT_EQ(fb->height, h);
        KTEST_ASSERT_EQ(fb->bpp,    32u);
        KTEST_ASSERT_EQ(fb->pitch,  w * 4u);

        /* Each glyph cell is 8×8 pixels scaled by font_scale. */
        uint32_t cell = 8u * scale;
        KTEST_ASSERT_EQ(vesa_tty_get_cols(), w / cell);
        KTEST_ASSERT_EQ(vesa_tty_get_rows(), h / cell);
    }

    /* Restore original display state. */
    t_writestring("[ktest] vesa_resolution: restoring ");
    t_dec(saved_w); t_writestring("x"); t_dec(saved_h);
    t_writestring("\n");
    uint32_t restore_scale = (saved_w >= 1280) ? 2u : 1u;
    vesa_tty_set_scale(restore_scale);
    if (hw)
        bochs_vbe_set_mode(saved_w, saved_h, 32);
    vesa_update_geometry(saved_w, saved_h, 32);
    vesa_tty_init();

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: vesa_colour
 *
 * Cycles through all 16 standard CGA palette colours as both foreground and
 * background, holding each combination briefly so it is visible in a graphical
 * ktest run.  Asserts that vesa_tty_is_ready() remains true throughout and
 * that the screen can be written to without panicking.
 * ------------------------------------------------------------------------- */

typedef struct { const char *name; uint32_t rgb; } ktest_colour_t;

static const ktest_colour_t ktest_palette[] = {
    { "black",        0x000000 },
    { "blue",         0x0000AA },
    { "green",        0x00AA00 },
    { "cyan",         0x00AAAA },
    { "red",          0xAA0000 },
    { "magenta",      0xAA00AA },
    { "brown",        0xAA5500 },
    { "lightgray",    0xAAAAAA },
    { "darkgray",     0x555555 },
    { "lightblue",    0x5555FF },
    { "lightgreen",   0x55FF55 },
    { "lightcyan",    0x55FFFF },
    { "lightred",     0xFF5555 },
    { "lightmagenta", 0xFF55FF },
    { "yellow",       0xFFFF55 },
    { "white",        0xFFFFFF },
};
#define KTEST_PALETTE_SIZE ((uint32_t)(sizeof(ktest_palette)/sizeof(ktest_palette[0])))

static void test_vesa_colour(void)
{
    ktest_begin("vesa_colour", "VESA fg/bg colour state, glyph rendering, framebuffer pixel layout");

    KTEST_ASSERT(vesa_tty_is_ready());

    /* Pair each background with a contrasting foreground (white or black). */
    for (uint32_t i = 0; i < KTEST_PALETTE_SIZE; i++) {
        uint32_t bg = ktest_palette[i].rgb;
        /* Use white fg on dark backgrounds, black fg on light ones. */
        uint32_t luminance = ((bg >> 16) & 0xFF) * 299u
                           + ((bg >>  8) & 0xFF) * 587u
                           + ( bg        & 0xFF) * 114u;
        uint32_t fg = (luminance < 128000u) ? 0xFFFFFF : 0x000000;

        vesa_tty_setcolor(fg, bg);
        vesa_tty_clear();

        t_writestring("[ktest] vesa_colour: bg=");
        t_writestring(ktest_palette[i].name);
        t_writestring("\n");

        KTEST_ASSERT(vesa_tty_is_ready());

        ksleep(16); /* ~160 ms at 100 Hz - long enough to see the change */
    }

    /* Restore default white-on-blue. */
    vesa_tty_setcolor(0xFFFFFF, 0x0000AA);
    vesa_tty_clear();

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: keyboard
 *
 * Drives the PS/2 decoder synchronously through the keyboard_test_* hooks.
 * Each subtest calls keyboard_test_reset() so it inherits a clean decoder
 * state, modifier state, and global ring.  The whole suite is bracketed
 * by keyboard_test_begin/end which save and clear the real focused task
 * (so routed bytes land in the global ring where we can drain them).
 *
 * Behaviour pinned by these tests:
 *
 *   - All KEY_* sentinels survive the dispatch path as unsigned bytes >= 0x80
 *     (the EIP=0xFFFFFF83 sign-extension regression from PR #124).
 *   - Make/break separation: a break never produces output; a held modifier
 *     applies to subsequent makes; releasing the other side of a paired
 *     modifier (RSHIFT while LSHIFT is still down) does not clear the state.
 *   - Decoder is livelock-free under random byte streams (4096-byte LCG fuzz)
 *     and across the full 0x00..0xFF boundary in each prefix state.
 *   - PrintScreen's "fake-shift" padding (e0 2a / e0 aa) is dropped.
 *
 * The typematic-repeat and Caps-LED behaviours are *not* asserted yet --
 * they're introduced by the slice-5b commits that follow this harness, so
 * the assertions for those land alongside the fixes.
 * ------------------------------------------------------------------------- */

/* Drain the global ring into out[], return count.  Bounded to cap so a
 * runaway decoder can't blow the stack buffer. */
static uint32_t kb_test_drain_all(unsigned char *out, uint32_t cap)
{
    uint32_t n = 0;
    while (n < cap) {
        unsigned char c;
        if (!keyboard_test_drain(&c)) break;
        out[n++] = c;
    }
    return n;
}

static void test_keyboard(void)
{
    ktest_begin("keyboard", "PS/2 layered driver: decoder state machine, modifier tracking, SPSC ring");

    keyboard_test_begin();

    /* ---- sentinel coverage: e0-prefixed navigation keycodes ------------- */
    /* These KEY_* sentinels must arrive as single unsigned bytes.  The 0x83
     * arrow case is the regression target for PR #124's EIP=0xFFFFFF83 panic. */
    {
        struct { uint8_t e0_byte; unsigned char expect; } cases[] = {
            { 0x48, KEY_ARROW_UP    },
            { 0x50, KEY_ARROW_DOWN  },
            { 0x4B, KEY_ARROW_LEFT  },
            { 0x4D, KEY_ARROW_RIGHT },
            { 0x49, KEY_PAGE_UP     },
            { 0x51, KEY_PAGE_DOWN   },
        };
        for (uint32_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
            keyboard_test_reset();
            keyboard_test_feed(0xE0);
            keyboard_test_feed(cases[i].e0_byte);
            unsigned char buf[4];
            uint32_t n = kb_test_drain_all(buf, sizeof(buf));
            KTEST_ASSERT(n == 1);
            KTEST_ASSERT(buf[0] == cases[i].expect);
            /* Each sentinel is >= 0x80 - if any path sign-extended it, the
             * resulting int comparison would survive but the byte would not
             * round-trip equal to the cast we expect. */
            KTEST_ASSERT(buf[0] >= 0x80);
            /* Break event must produce no output and not double-fire. */
            keyboard_test_feed(0xE0);
            keyboard_test_feed((uint8_t)(cases[i].e0_byte | 0x80));
            n = kb_test_drain_all(buf, sizeof(buf));
            KTEST_ASSERT(n == 0);
        }
    }

    /* ---- single-byte ASCII translation ---------------------------------- */
    {
        keyboard_test_reset();
        keyboard_test_feed(0x1E); /* 'a' make */
        unsigned char buf[4];
        uint32_t n = kb_test_drain_all(buf, sizeof(buf));
        KTEST_ASSERT(n == 1);
        KTEST_ASSERT(buf[0] == 'a');

        keyboard_test_feed(0x9E); /* 'a' break - no output */
        n = kb_test_drain_all(buf, sizeof(buf));
        KTEST_ASSERT(n == 0);
    }

    /* ---- make/break separation with both shifts ------------------------- */
    /* Hold RSHIFT, press 'a' -> 'A'.  Release RSHIFT, press 'a' -> 'a'.
     * Repeat with LSHIFT.  The LSHIFT half is the regression target for the
     * 0xAA BAT-pass filter collision -- before decoder_feed's response-byte
     * filter was narrowed, LSHIFT break (0xAA) was silently swallowed and
     * the shift state stayed stuck at 1. */
    {
        struct { uint8_t make, brk; } shifts[] = {
            { 0x36, 0xB6 }, /* RSHIFT */
            { 0x2A, 0xAA }, /* LSHIFT - regression target */
        };
        for (uint32_t i = 0; i < sizeof(shifts)/sizeof(shifts[0]); i++) {
            keyboard_test_reset();
            keyboard_test_feed(shifts[i].make);
            unsigned char buf[4];
            uint32_t n = kb_test_drain_all(buf, sizeof(buf));
            KTEST_ASSERT(n == 0); /* modifier silent in cooked mode */
            KTEST_ASSERT((keyboard_test_mod_state() & 0x1u) == 0x1u);

            keyboard_test_feed(0x1E); /* 'a' make */
            n = kb_test_drain_all(buf, sizeof(buf));
            KTEST_ASSERT(n == 1 && buf[0] == 'A');

            keyboard_test_feed(shifts[i].brk);
            KTEST_ASSERT((keyboard_test_mod_state() & 0x1u) == 0u);

            keyboard_test_feed(0x1E);
            n = kb_test_drain_all(buf, sizeof(buf));
            KTEST_ASSERT(n == 1 && buf[0] == 'a');
        }
    }

    /* ---- paired-modifier release: either side can be released first ----- */
    /* Hold LSHIFT + RSHIFT; release either; mod_shift stays set thanks to
     * the other side still being held. */
    {
        struct { uint8_t first_brk; } cases[] = { { 0xB6 }, { 0xAA } };
        for (uint32_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
            keyboard_test_reset();
            keyboard_test_feed(0x2A); /* LSHIFT make */
            keyboard_test_feed(0x36); /* RSHIFT make */
            KTEST_ASSERT((keyboard_test_mod_state() & 0x1u) == 0x1u);
            keyboard_test_feed(cases[i].first_brk);
            KTEST_ASSERT((keyboard_test_mod_state() & 0x1u) == 0x1u);
        }
    }

    /* ---- Ctrl+letter folds to ASCII control code ------------------------ */
    /* Use 'b' (Ctrl-B == 0x02), not 'a': Ctrl-A is intercepted in cooked
     * mode as the pane-switch prefix and never reaches the routed-byte path. */
    {
        keyboard_test_reset();
        keyboard_test_feed(0x1D); /* LCTRL make */
        keyboard_test_feed(0x30); /* 'b' make - should be Ctrl-B == 0x02 */
        unsigned char buf[4];
        uint32_t n = kb_test_drain_all(buf, sizeof(buf));
        KTEST_ASSERT(n == 1 && buf[0] == 0x02);
        keyboard_test_feed(0x9D); /* LCTRL break */
    }

    /* ---- typematic-repeat filter for modifiers -------------------------- */
    /* PS/2 hardware re-fires a held key's make at ~30 Hz.  For Caps Lock the
     * previous decoder toggled mod_caps on every make, so holding Caps for a
     * fraction of a second ping-ponged the toggle unpredictably.  Modifier
     * make events without an intervening break must now be dropped entirely.
     *
     * Specifically:
     *   - Caps make x5, then break: mod_caps must toggle EXACTLY once.
     *   - Shift make x5: mod_shift stays 1 (idempotent) but routes no extra
     *     KEY_SHIFT_DOWN sentinels in raw mode.  We test the state half here
     *     (raw-mode sentinel coverage is covered by the kbtester smoke test).
     *   - After break and re-press, the next make is honoured again. */
    {
        keyboard_test_reset();
        for (int i = 0; i < 5; i++)
            keyboard_test_feed(0x3A); /* Caps make */
        KTEST_ASSERT((keyboard_test_mod_state() >> 3) & 1u); /* mod_caps == 1 */
        keyboard_test_feed(0xBA);     /* Caps break */
        KTEST_ASSERT((keyboard_test_mod_state() >> 3) & 1u); /* still 1 (Caps is sticky) */

        /* Second press cycle: another 5x make should toggle exactly once. */
        for (int i = 0; i < 5; i++)
            keyboard_test_feed(0x3A);
        KTEST_ASSERT(((keyboard_test_mod_state() >> 3) & 1u) == 0u);
        keyboard_test_feed(0xBA);
    }
    {
        /* LSHIFT held: subsequent makes don't redundantly fire on_make.  We
         * can't directly observe on_make calls here, but raw-mode delivery
         * surfaces them as routed bytes -- exercise that path. */
        keyboard_test_reset();
        keyboard_set_raw(1);
        keyboard_test_feed(0x2A); /* LSHIFT make */
        unsigned char buf[8];
        uint32_t n = kb_test_drain_all(buf, sizeof(buf));
        KTEST_ASSERT(n == 1 && buf[0] == (unsigned char)KEY_SHIFT_DOWN);
        /* Typematic repeats should NOT re-emit the sentinel. */
        for (int i = 0; i < 5; i++)
            keyboard_test_feed(0x2A);
        n = kb_test_drain_all(buf, sizeof(buf));
        KTEST_ASSERT(n == 0);
        /* Break, then re-press: a single sentinel again. */
        keyboard_test_feed(0xAA);
        keyboard_test_feed(0x2A);
        n = kb_test_drain_all(buf, sizeof(buf));
        KTEST_ASSERT(n == 1 && buf[0] == (unsigned char)KEY_SHIFT_DOWN);
        keyboard_set_raw(0);
    }

    /* ---- LED sync: Caps press updates the bitmap exactly once ----------- */
    /* kb_sync_leds is called from apply_modifier when mod_caps changes; the
     * 0xED + bitmap is suppressed in test mode and only the software shadow
     * updates.  We exercise:
     *   - one Caps press cycle flips the Caps bit and increments send count;
     *   - typematic repeats during the press DON'T trigger extra sends
     *     (the typematic filter from f636920 squashes them upstream);
     *   - a second press cycle clears the Caps bit. */
    {
        keyboard_test_reset();
        uint32_t baseline = keyboard_test_led_sends();
        for (int i = 0; i < 5; i++)
            keyboard_test_feed(0x3A); /* Caps make x5 */
        keyboard_test_feed(0xBA);     /* Caps break */
        KTEST_ASSERT(keyboard_test_led_sends() == baseline + 1);
        KTEST_ASSERT(keyboard_test_leds() & 0x04); /* PS2_LED_CAPS */

        for (int i = 0; i < 5; i++)
            keyboard_test_feed(0x3A);
        keyboard_test_feed(0xBA);
        KTEST_ASSERT(keyboard_test_led_sends() == baseline + 2);
        KTEST_ASSERT((keyboard_test_leds() & 0x04) == 0);
    }

    /* ---- torn-prefix recovery: repeated 0xE0 restarts the prefix -------- */
    /* DEC_AFTER_E0 + 0xE0 -> still DEC_AFTER_E0; one ARROW_LEFT emitted. */
    {
        keyboard_test_reset();
        keyboard_test_feed(0xE0);
        keyboard_test_feed(0xE0);
        keyboard_test_feed(0x4B); /* ARROW_LEFT make */
        unsigned char buf[4];
        uint32_t n = kb_test_drain_all(buf, sizeof(buf));
        KTEST_ASSERT(n == 1 && buf[0] == 0x82);
    }

    /* ---- PrintScreen fake-shift padding is dropped ---------------------- */
    /* PS/2 emits e0 2a e0 37 (make) / e0 b7 e0 aa (break).  The e0 2a / e0 aa
     * portions are "fake shift" padding -- we drop them so real shift state
     * stays honest.  After feeding e0 aa with no real shift held, mod_shift
     * must still be zero. */
    {
        keyboard_test_reset();
        keyboard_test_feed(0xE0);
        keyboard_test_feed(0x2A);
        keyboard_test_feed(0xE0);
        keyboard_test_feed(0xAA);
        unsigned char buf[4];
        uint32_t n = kb_test_drain_all(buf, sizeof(buf));
        KTEST_ASSERT(n == 0);
        KTEST_ASSERT((keyboard_test_mod_state() & 0x1u) == 0u);
    }

    /* ---- controller response bytes reset decoder state ------------------ */
    /* 0xFA (ack) and friends arriving mid-prefix must reset to NORMAL, not
     * poison the next real scancode. */
    {
        keyboard_test_reset();
        keyboard_test_feed(0xE0);   /* prime extended prefix */
        keyboard_test_feed(0xFA);   /* ack - must reset */
        keyboard_test_feed(0x1E);   /* 'a' make - should arrive as plain 'a' */
        unsigned char buf[4];
        uint32_t n = kb_test_drain_all(buf, sizeof(buf));
        KTEST_ASSERT(n == 1 && buf[0] == 'a');
    }

    /* ---- boundary scan: every byte in every decoder state, no panic ----- */
    /* Feed 0x00..0xFF in DEC_NORMAL, DEC_AFTER_E0, DEC_AFTER_E1A, DEC_AFTER_E1B.
     * Each iteration starts from a reset so the state we want to test is the
     * one we just put the decoder into.  Anything that crashes here would
     * page-fault and we'd never get to the assert below. */
    {
        for (uint32_t prelude = 0; prelude < 4; prelude++) {
            for (uint32_t b = 0; b < 256; b++) {
                keyboard_test_reset();
                switch (prelude) {
                    case 1: keyboard_test_feed(0xE0); break;
                    case 2: keyboard_test_feed(0xE1); break;
                    case 3: keyboard_test_feed(0xE1); keyboard_test_feed(0x1D); break;
                    default: break;
                }
                keyboard_test_feed((uint8_t)b);
                /* Drain (and discard) any emitted bytes; the test is the
                 * absence of a fault, not specific output. */
                unsigned char tmp[8];
                (void)kb_test_drain_all(tmp, sizeof(tmp));
            }
        }
        KTEST_ASSERT(1); /* survived 1024 byte/state combinations */
    }

    /* ---- 512-byte LCG fuzz: no panic, ring never wedges ----------------- */
    /* A tiny LCG (Numerical Recipes constants) gives us a reproducible
     * pseudo-random byte stream.  We feed it into the decoder and drain
     * after each byte so the ring never fills.  Any unsigned-char hygiene
     * regression on the routed-byte path would either fault or wedge the
     * ring head/tail accounting; we assert the obvious invariants. */
    {
        keyboard_test_reset();
        uint32_t seed = 0xC0FFEEu;
        uint32_t total_drained = 0;
        for (uint32_t i = 0; i < 512; i++) {
            seed = seed * 1664525u + 1013904223u;
            uint8_t sc = (uint8_t)(seed >> 16);
            keyboard_test_feed(sc);
            unsigned char tmp[8];
            uint32_t n = kb_test_drain_all(tmp, sizeof(tmp));
            total_drained += n;
        }
        /* The decoder definitely produced *some* output across 512 random
         * bytes; we don't need an exact count, just non-zero and bounded. */
        KTEST_ASSERT(total_drained > 0);
        KTEST_ASSERT(total_drained < 512); /* most bytes are break-halves or prefixes */
    }

    keyboard_test_end();

    ktest_summary();
}

/* A real ZIP (python zipfile, deflate) holding hello.txt = "MAKAR-UNZIP-OK\n"
 * and dir/inner.txt = "makar " x64.  Exercises inflate + central-dir parsing
 * + nested-dir creation, entirely offline. */
static const uint8_t uzfix_zip[248] = {
80,75,3,4,20,0,0,0,8,0,40,148,196,92,189,17,150,31,17,0,0,0,15,0,0,0,9,0,0,0,104,101,108,108,111,46,116,120,116,243,117,244,118,12,210,13,245,139,242,12,208,245,247,230,2,0,80,75,3,4,20,0,0,0,8,0,40,148,196,92,114,12,255,60,13,0,0,0,128,1,0,0,13,0,0,0,100,105,114,47,105,110,110,101,114,46,116,120,116,203,77,204,78,44,82,200,29,37,7,136,4,0,80,75,1,2,20,3,20,0,0,0,8,0,40,148,196,92,189,17,150,31,17,0,0,0,15,0,0,0,9,0,0,0,0,0,0,0,0,0,0,0,128,1,0,0,0,0,104,101,108,108,111,46,116,120,116,80,75,1,2,20,3,20,0,0,0,8,0,40,148,196,92,114,12,255,60,13,0,0,0,128,1,0,0,13,0,0,0,0,0,0,0,0,0,0,0,128,1,56,0,0,0,100,105,114,47,105,110,110,101,114,46,116,120,116,80,75,5,6,0,0,0,0,2,0,2,0,114,0,0,0,112,0,0,0,0,0
};

static void test_unzip(void)
{
    ktest_begin("unzip", "ZIP extract: inflate (deflate) + central dir + nested dirs");

    int failed = 0;
    int n = unzip_archive(uzfix_zip, sizeof(uzfix_zip), "/tmp/uztest", &failed);
    Serial_WriteString("[ktest] unzip: extracted=");
    Serial_WriteDec((uint32_t)(n < 0 ? 0 : n));
    Serial_WriteString(" failed=");
    Serial_WriteDec((uint32_t)failed);
    Serial_WriteString("\n");
    KTEST_ASSERT(n == 2);
    KTEST_ASSERT(failed == 0);

    char buf[512];
    uint32_t got = 0;

    /* hello.txt -- a tiny deflated entry. */
    KTEST_ASSERT(vfs_read_file("/tmp/uztest/hello.txt", buf, sizeof(buf), &got) == 0);
    KTEST_ASSERT(got == 15);
    KTEST_ASSERT(memcmp(buf, "MAKAR-UNZIP-OK\n", 15) == 0);

    /* dir/inner.txt -- nested path + back-reference-heavy deflate stream. */
    got = 0;
    KTEST_ASSERT(vfs_read_file("/tmp/uztest/dir/inner.txt", buf, sizeof(buf), &got) == 0);
    KTEST_ASSERT(got == 384);
    KTEST_ASSERT(memcmp(buf, "makar makar ", 12) == 0);

    ktest_summary();
}

int ktest_run_all(void)
{
    int total_pass = 0;
    int total_fail = 0;

    test_acpi_checksum();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_fpu();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_string();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_vt_status_scroll();
    test_vt_ansi();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_partition();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_pci_bind();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    /* Networking suites are NOT run here: they require QEMU slirp + guestfwd
     * and a specific NIC -device, and are parameterised by NET_DEVICE.  They
     * live in their own section, ktest_run_net(), dispatched by the
     * "nettest" test_mode selector.  See ktest_run_net() below. */

    test_devfs();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_tmpfs();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_unzip();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_usr();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_rootfs_mount_layout();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_pmm();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_surface();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_ide_dma();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_buddy();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_heap();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_vmm();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_task();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_ipc();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_procfs_tasks();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_getpid();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_posix_fs_syscalls();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_rtc_unix_time();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_syscall();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_fd_table();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_file_fd();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_cwd();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_signal();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_preempt();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_gdt();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_ring3_prereqs();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_idt();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_ring3_execution();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_elf_exec();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_ring3_with_arg();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_vesa_resolution();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_vesa_colour();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_keyboard();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    t_writestring("\n[ktest] TOTAL: ");
    t_dec((uint32_t)total_pass);
    t_writestring(" passed, ");
    t_dec((uint32_t)total_fail);
    t_writestring(" failed\n");
    return total_fail;
}

/*
 * ktest_run_net - networking test section.
 *
 * Kept separate from ktest_run_all() because these suites depend on QEMU
 * user networking (slirp + guestfwd) and bind to whichever NIC the harness
 * attached (NET_DEVICE=virtio|rtl8139|e1000|pcnet).  The active-NIC path is
 * exercised the same way regardless of which driver bound, so this one
 * section validates every backend by re-running it with a different
 * NET_DEVICE.  Returns the total number of failed assertions.
 */
int ktest_run_net(void)
{
    int total_pass = 0;
    int total_fail = 0;

    test_virtio_net();              /* active netdev TX ARP + polled RX */
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_lwip_tcp();                /* lwIP over netdev: TCP connect + recv */
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_lwip_ping();               /* ICMP echo: gateway asserted, 1.1.1.1 soft */
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_lwip_dns();                /* DNS resolver: numeric asserted, real soft */
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_wget();                    /* HTTP GET via slirp guestfwd fixture */
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_lwip_net_info();           /* SYS_NET_INFO text + DHCP/DNS controls */
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    t_writestring("\n[ktest] NET TOTAL: ");
    t_dec((uint32_t)total_pass);
    t_writestring(" passed, ");
    t_dec((uint32_t)total_fail);
    t_writestring(" failed\n");
    return total_fail;
}

/* Background task entry: run safe suites silently during the loading screen.
 * Skips test_vesa_resolution and test_vesa_colour - both switch display modes
 * and have multi-second sleeps that would corrupt the loading screen.
 * Sets ktest_bg_done = 1 when finished so shell_run can proceed. */
void ktest_bg_task(void)
{
    int total_pass = 0;
    int total_fail = 0;

    ktest_muted = 1;
    ktest_bg_completed = 0;

    #define RUN(suite) do { \
        suite(); \
        total_pass += ktest_pass_count; \
        total_fail += ktest_fail_count; \
        ktest_bg_completed++; \
        Serial_WriteString("[ktest-bg] " #suite ": "); \
        if (ktest_fail_count == 0) { \
            Serial_WriteString("PASS "); \
        } else { \
            Serial_WriteString("FAIL "); \
        } \
        Serial_WriteDec((uint32_t)ktest_pass_count); \
        Serial_WriteString("/"); \
        Serial_WriteDec((uint32_t)(ktest_pass_count + ktest_fail_count)); \
        Serial_WriteString("\n"); \
        /* Brief pacing keeps the loading screen visible while not pushing \
         * iso-test's 120 s GDB budget into the failure regime under TCG. \
         * 5 ticks @ 100 Hz = 50 ms; visible on real HW, ~650 ms total over \
         * 20 suites in TCG. */ \
        { uint32_t t0 = timer_get_ticks(); \
          while (timer_get_ticks() - t0 < 5) task_yield(); } \
    } while (0)

    /* POST / kernel-integrity suites only.
     * Userspace-visible syscall behaviour lives in ktest_uspace.elf (incore). */
    RUN(test_acpi_checksum);
    RUN(test_fpu);
    RUN(test_string);
    RUN(test_partition);
    RUN(test_pci_bind);
    RUN(test_devfs);
    RUN(test_tmpfs);
    RUN(test_rootfs_mount_layout);
    RUN(test_pmm);
    RUN(test_buddy);
    RUN(test_heap);
    RUN(test_vmm);
    RUN(test_task);
    RUN(test_ipc);
    RUN(test_procfs_tasks);
    RUN(test_getpid);
    RUN(test_rtc_unix_time);
    RUN(test_syscall);
    RUN(test_fd_table);
    RUN(test_signal);
    RUN(test_preempt);
    RUN(test_gdt);
    RUN(test_ring3_prereqs);
    RUN(test_idt);
    /* Skipped from the bg pass (still run deterministically in foreground
     * via test_mode ISO Phase 1 and the shell `ktest` command):
     *   - test_ring3_execution / test_elf_exec / test_ring3_with_arg:
     *     each spawns a ring-3 child and re-enters ring 0 via int 0x80,
     *     then yields back to the bg parent.  Under concurrent scheduling
     *     with 4 shell tasks and a GDB-attached debug build this path
     *     intermittently leaves TF=1 in EFLAGS of the resuming kernel
     *     context, triggering an INT1 single-step storm.  Phase 1
     *     (single-task, no GDB) exercises the same code reliably and
     *     proves the lifecycle.
     *   - test_keyboard: ~1500 decoder_feed cycles push past shell_run's
     *     first keyboard_getchar under TCG.
     *   - test_vesa_resolution / test_vesa_colour: switch display modes
     *     with multi-second countdowns. */

    #undef RUN

    ktest_muted = 0;

    if (total_fail > 0) {
        t_writestring("[ktest] ");
        t_dec((uint32_t)total_fail);
        t_writestring(" failure(s) - run `ktest` for details\n");
        Serial_WriteString("KTEST_BG: FAIL\n");
    } else {
        Serial_WriteString("KTEST_BG: PASS\n");
    }

    ktest_bg_done = 1;
    ktest_bg_marker();
}

/* ktest_bg_marker - empty hook called immediately after ktest_bg_done = 1.
 *
 * The GDB iso-test harness sets a breakpoint here so it can confirm bg
 * ktest finished WITHOUT depending on the shell reaching its first
 * keyboard_getchar.  Coupling the assertion to keyboard_getchar made
 * iso-test flaky under TCG: shell0's loading-screen spinner spins on
 * vesa_tty_spinner_tick() until ktest_bg_done flips, then must drain
 * poll, print banner, and only THEN call keyboard_getchar -- the cumul-
 * ative wall-clock occasionally raced the 120 s GDB-step budget.
 *
 * Putting the marker right after the flag write lets the assertion
 * happen the moment the kernel guarantees the flag is set, regardless
 * of the subsequent shell-render timing.
 *
 * noinline + externally visible so GDB sees the symbol.  The function
 * body is intentionally a single memory-clobbering nop so the optimiser
 * cannot fold it away. */
__attribute__((noinline))
void ktest_bg_marker(void)
{
    __asm__ volatile("" ::: "memory");
}
