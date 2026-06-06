/*
 * acpi.c -- ACPI power management: clean S5 ("soft off") shutdown.
 *
 * Strategy (tried in order):
 *   1. Full ACPI: scan BIOS areas for the RSDP, walk RSDP → RSDT → FADT,
 *      then scan the DSDT for the \_S5_ AML package to obtain SLP_TYP values;
 *      write SLP_TYPa | SLP_EN to PM1a_CNT (and PM1b_CNT if present).
 *   2. QEMU new-style  – outw(0x604, 0x2000)
 *   3. Bochs / old QEMU – outw(0xB004, 0x2000)
 *   4. Unconditional cli + hlt spin (machine appears frozen but is safe).
 *
 * References:
 *   - ACPI Specification 6.5, §5 (ACPI Hardware)
 *   - OSDev wiki: ACPI, RSDP, FADT
 */

#include <kernel/acpi.h>
#include <kernel/asm.h>
#include <kernel/tty.h>
#include <kernel/serial.h>
#include <kernel/paging.h>
#include <stddef.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * ACPI table structures (packed, per spec)
 * ------------------------------------------------------------------------- */

/* Common ACPI SDT header (36 bytes). */
typedef struct __attribute__((packed)) {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} acpi_sdt_header_t;

/* RSDP (ACPI 1.0 portion, 20 bytes). */
typedef struct __attribute__((packed)) {
    char     signature[8];   /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;       /* 0 = ACPI 1.0, 2 = ACPI 2.0+ */
    uint32_t rsdt_address;
    /* ACPI 2.0+ extended fields follow (we only use rsdt_address). */
} acpi_rsdp_t;

/* FADT – Fixed ACPI Description Table (we only read the fields we need). */
typedef struct __attribute__((packed)) {
    acpi_sdt_header_t hdr;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t  _reserved;
    uint8_t  preferred_pm_profile;
    uint16_t sci_interrupt;
    uint32_t smi_cmd;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_cnt;
    uint32_t pm1a_event_blk;
    uint32_t pm1b_event_blk;
    uint32_t pm1a_cnt_blk;   /* PM1a control block I/O port */
    uint32_t pm1b_cnt_blk;   /* PM1b control block I/O port (0 if absent) */
    /* Fields below are present from ACPI 1.0 onward but we only use them
       when hdr.length >= 129 (ACPI 2.0+) to reach the reset register.
       We read them via raw byte offsets to avoid padding issues. */
} acpi_fadt_t;

/*
 * FADT raw byte offsets for the ACPI 2.0+ reset register fields.
 * (ACPI spec §5.2.9, Table 5-9)
 *
 *   Offset 116 – RESET_REG.AddressSpaceID  (1 = I/O port)
 *   Offset 120 – RESET_REG.Address         (64-bit; we only read low 16 bits)
 *   Offset 128 – RESET_VALUE
 *
 * These are only valid when FADT.Length >= 129 and FADT.Revision >= 2.
 */
#define FADT_OFFSET_RESET_SPACE  116u
#define FADT_OFFSET_RESET_ADDR   120u
#define FADT_OFFSET_RESET_VALUE  128u
#define FADT_MIN_LEN_RESET       129u

/* ---------------------------------------------------------------------------
 * ACPI shutdown state (filled in by acpi_init)
 * ------------------------------------------------------------------------- */

static int      acpi_enabled   = 0;
static uint16_t pm1a_cnt_port  = 0;
static uint16_t pm1b_cnt_port  = 0;
static uint16_t slp_typa       = 0;
static uint16_t slp_typb       = 0;
static uint16_t smi_cmd_port   = 0;   /* FADT SMI_CMD port (0 if HW-reduced) */
static uint8_t  acpi_enable_val = 0;  /* value to write to SMI_CMD to enable  */

/* Cached for reboot: raw FADT pointer and its length. */
static const uint8_t *fadt_raw  = NULL;
static uint32_t       fadt_len  = 0;

/* Cached for the on-screen acpi_diag() (serial is impractical on VMware/VBox/
 * Hyper-V).  Set as far through acpi_init() as the parse reached. */
static uint32_t       diag_rsdt_addr = 0;
static uint32_t       diag_xsdt_addr = 0;
static int            diag_fadt_ok   = 0;
static const uint8_t *diag_dsdt      = NULL;
static uint32_t       diag_dsdt_len  = 0;

#define SLP_EN   (1u << 13)    /* SLP_EN bit in PM1 control register */

/* ---------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/*
 * acpi_checksum – verify the ACPI table checksum.
 *
 * Per spec, the byte sum of all bytes in the table (including the checksum
 * byte itself) must be 0.  Returns 1 if valid, 0 if not.
 */
int acpi_checksum(const void *table, size_t length)
{
    const uint8_t *p = (const uint8_t *)table;
    uint8_t sum = 0;
    for (size_t i = 0; i < length; i++)
        sum += p[i];
    return sum == 0;
}

/* Scan [start, end) for the RSDP signature "RSD PTR " on 16-byte alignment. */
static const acpi_rsdp_t *find_rsdp_in_range(uint32_t start, uint32_t end)
{
    for (uint32_t addr = start; addr < end; addr += 16) {
        const char *p = (const char *)(uintptr_t)addr;
        if (memcmp(p, "RSD PTR ", 8) == 0) {
            const acpi_rsdp_t *rsdp = (const acpi_rsdp_t *)p;
            if (acpi_checksum(rsdp, 20))
                return rsdp;
        }
    }
    return NULL;
}

/* Locate the RSDP: first check the EBDA, then the BIOS ROM area. */
static const acpi_rsdp_t *locate_rsdp(void)
{
    /* EBDA segment address is stored at physical 0x40E (two bytes). */
    uint16_t ebda_seg = *(volatile uint16_t *)(uintptr_t)0x40E;
    uint32_t ebda_addr = (uint32_t)ebda_seg << 4;
    if (ebda_addr >= 0x80000 && ebda_addr < 0xA0000) {
        const acpi_rsdp_t *r = find_rsdp_in_range(ebda_addr, ebda_addr + 0x400);
        if (r) return r;
    }

    /* BIOS ROM search range. */
    return find_rsdp_in_range(0xE0000, 0x100000);
}

/* Decode an AML PkgLength at d[*pj]; advance *pj past it.  The lead byte's top
 * two bits give how many extra length bytes follow (0..3); the rest are the low
 * length nibble/bits.  (Value itself is unused -- we only need to skip it.) */
static void aml_skip_pkglength(const uint8_t *d, uint32_t *pj, uint32_t end)
{
    uint32_t j = *pj;
    if (j >= end) { *pj = j; return; }
    uint32_t nfollow = (uint32_t)(d[j] >> 6);
    j += 1 + nfollow;
    *pj = j;
}

/* Read an AML integer constant (Zero/One/Ones/Byte/Word/DWord) at d[*pj] into
 * *val; advance *pj.  Returns 1 if a recognised constant was read. */
static int aml_read_const(const uint8_t *d, uint32_t *pj, uint32_t end, uint32_t *val)
{
    uint32_t j = *pj;
    if (j >= end) return 0;
    switch (d[j]) {
    case 0x00: *val = 0;    *pj = j + 1; return 1;          /* ZeroOp */
    case 0x01: *val = 1;    *pj = j + 1; return 1;          /* OneOp  */
    case 0xFF: *val = 0xFF; *pj = j + 1; return 1;          /* OnesOp */
    case 0x0A:                                              /* BytePrefix */
        if (j + 1 >= end) return 0;
        *val = d[j + 1]; *pj = j + 2; return 1;
    case 0x0B:                                              /* WordPrefix */
        if (j + 2 >= end) return 0;
        *val = d[j + 1] | ((uint32_t)d[j + 2] << 8); *pj = j + 3; return 1;
    case 0x0C:                                              /* DWordPrefix */
        if (j + 4 >= end) return 0;
        *val = d[j+1] | ((uint32_t)d[j+2]<<8) | ((uint32_t)d[j+3]<<16) | ((uint32_t)d[j+4]<<24);
        *pj = j + 5; return 1;
    default: return 0;
    }
}

/*
 * s5_has_nameop – verify the "_S5_" at d[i] is introduced by a NameOp (0x08).
 *
 * The AML NameString grammar permits a RootChar '\' (0x5C) or one-or-more
 * ParentPrefixChar '^' (0x5E) between the NameOp and the NameSeg:
 *
 *   QEMU/SeaBIOS:  Name(_S5_,  ...)  =>  08 5F 53 35 5F ...
 *   Hyper-V:       Name(\_S5_, ...)  =>  08 5C 5F 53 35 5F ...
 *
 * The original check only accepted a NameOp *immediately* before the name, so
 * Hyper-V's leading RootChar made it reject a perfectly valid \_S5_ and soft-
 * off silently fell through to the QEMU-only I/O ports (machine hung).  Walk
 * back over any ParentPrefix run and an optional RootChar, then require 0x08.
 */
static int s5_has_nameop(const uint8_t *d, uint32_t i)
{
    if (i == 0) return 0;
    uint32_t p = i - 1;
    while (p > 0 && d[p] == 0x5E) p--;          /* ParentPrefixChar(s) '^'   */
    if (d[p] == 0x5C) {                          /* optional RootChar '\'     */
        if (p == 0) return 0;
        p--;
    }
    return d[p] == 0x08;                          /* NameOp                    */
}

/*
 * scan_s5 – search a DSDT byte stream for the \_S5_ AML package and extract the
 * SLP_TYPa/b values written to PM1{a,b}_CNT for soft-off.
 *
 * AML: Name(_S5_, Package(){ SLP_TYPa, SLP_TYPb, ... })
 *      08 5F 53 35 5F  12  <PkgLength>  <NumElements>  <const a>  <const b> ...
 *
 * The original parser hard-coded a 1-byte PkgLength, a NumElements of exactly
 * 0x04, and only 0x0A/0x00 constants -- which matches QEMU/SeaBIOS but not the
 * encodings emitted by other firmwares (VirtualBox, VMware, Hyper-V), so soft-
 * off silently failed there (the system hung instead of powering down).  This
 * version decodes the variable-length PkgLength and the full set of integer
 * constant ops, so it works across hypervisors.
 *
 * Returns 1 on success and writes *typa, *typb (already shifted into PM1_CNT
 * position); 0 if not found.
 */
static int scan_s5(const uint8_t *dsdt_data, uint32_t dsdt_len,
                   uint16_t *typa, uint16_t *typb)
{
    for (uint32_t i = 36; i + 8 < dsdt_len; i++) {
        if (memcmp(dsdt_data + i, "_S5_", 4) != 0)
            continue;
        /* The definition is introduced by NameOp (0x08), possibly with a
         * RootChar/ParentPrefix between it and the NameSeg; a bare "_S5_" not so
         * preceded is a reference, not the package we want. */
        if (!s5_has_nameop(dsdt_data, i))
            continue;

        uint32_t j = i + 4;                       /* past "_S5_" */
        if (j >= dsdt_len || dsdt_data[j] != 0x12) /* expect PackageOp */
            continue;
        j++;
        aml_skip_pkglength(dsdt_data, &j, dsdt_len);
        if (j >= dsdt_len) continue;
        j++;                                       /* skip NumElements byte */

        uint32_t a = 0, b = 0;
        if (!aml_read_const(dsdt_data, &j, dsdt_len, &a))
            continue;                              /* need at least SLP_TYPa */
        aml_read_const(dsdt_data, &j, dsdt_len, &b);   /* SLP_TYPb optional */

        *typa = (uint16_t)(a & 0x7) << 10;
        *typb = (uint16_t)(b & 0x7) << 10;
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/*
 * acpi_map_table – ensure a physical ACPI table is accessible.
 *
 * Maps a minimum region large enough to read the SDT header, reads the
 * `length` field, then maps the full table.  Returns the header pointer
 * (usable immediately after this call) or NULL if phys_addr is 0.
 *
 * paging_map_region() is a no-op for ranges already covered by the initial
 * 0–8 MiB identity map, so it is safe to call for any address.
 */
static const acpi_sdt_header_t *acpi_map_table(uint32_t phys_addr)
{
    if (phys_addr == 0)
        return NULL;

    /* Map the header first so we can read the length field safely. */
    paging_map_region(phys_addr, sizeof(acpi_sdt_header_t));

    const acpi_sdt_header_t *hdr =
        (const acpi_sdt_header_t *)(uintptr_t)phys_addr;

    /* Now map the complete table using the length from the header. */
    if (hdr->length > sizeof(acpi_sdt_header_t))
        paging_map_region(phys_addr, hdr->length);

    return hdr;
}

/* Walk an RSDT (entry_bytes=4) or XSDT (entry_bytes=8) for the FADT ("FACP"),
 * mapping each candidate before reading it.  On i386 we use the low 32 bits of
 * each entry address (firmware places ACPI tables below 4 GiB).  Returns NULL on
 * a null/invalid/checksum-failed table or no FADT. */
static const acpi_fadt_t *find_fadt(uint32_t sdt_phys, int entry_bytes)
{
    if (!sdt_phys) return NULL;
    const acpi_sdt_header_t *sdt = acpi_map_table(sdt_phys);
    if (!sdt || !acpi_checksum(sdt, sdt->length)) return NULL;

    const uint8_t *ents = (const uint8_t *)(sdt + 1);
    uint32_t n = (sdt->length - sizeof(acpi_sdt_header_t)) / (uint32_t)entry_bytes;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ep = *(const uint32_t *)(ents + i * (uint32_t)entry_bytes);
        if (!ep) continue;
        paging_map_region(ep, sizeof(acpi_sdt_header_t));
        const acpi_sdt_header_t *h = (const acpi_sdt_header_t *)(uintptr_t)ep;
        if (memcmp(h->signature, "FACP", 4) == 0) {
            if (h->length > sizeof(acpi_sdt_header_t))
                paging_map_region(ep, h->length);
            return (const acpi_fadt_t *)h;
        }
    }
    return NULL;
}

int acpi_init(void)
{
    const acpi_rsdp_t *rsdp = locate_rsdp();
    if (!rsdp) {
        KLOG("acpi: RSDP not found\n");
        return 0;
    }
    KLOG("acpi: RSDP found\n");

    /* Prefer the 64-bit XSDT on ACPI 2.0+ (what Linux does -- some firmwares,
     * incl. hypervisors, populate the XSDT more reliably than the legacy RSDT),
     * then fall back to the 32-bit RSDT. */
    diag_rsdt_addr = rsdp->rsdt_address;
    const acpi_fadt_t *fadt = NULL;
    if (rsdp->revision >= 2) {
        diag_xsdt_addr = *(const uint32_t *)((const uint8_t *)rsdp + 24); /* XSDT addr low32 */
        if (diag_xsdt_addr) fadt = find_fadt(diag_xsdt_addr, 8);
    }
    if (!fadt) fadt = find_fadt(rsdp->rsdt_address, 4);
    if (!fadt) {
        KLOG("acpi: FADT not found in RSDT/XSDT\n");
        return 0;
    }
    diag_fadt_ok = 1;
    if (!acpi_checksum(fadt, fadt->hdr.length)) {
        KLOG("acpi: FADT checksum bad\n");
        return 0;
    }

    pm1a_cnt_port  = (uint16_t)fadt->pm1a_cnt_blk;
    pm1b_cnt_port  = (uint16_t)fadt->pm1b_cnt_blk;
    smi_cmd_port   = (uint16_t)fadt->smi_cmd;
    acpi_enable_val = fadt->acpi_enable;

    /* Cache raw FADT bytes for acpi_reboot(). */
    fadt_raw = (const uint8_t *)fadt;
    fadt_len = fadt->hdr.length;

    /* Map and validate the DSDT, then scan for \_S5_.  Prefer the 64-bit X_DSDT
     * (FADT offset 140, ACPI 2.0+) over the 32-bit DSDT field, like Linux. */
    uint32_t dsdt_phys = fadt->dsdt;
    if (fadt->hdr.length >= 148) {
        uint32_t xdsdt = *(const uint32_t *)((const uint8_t *)fadt + 140);
        if (xdsdt) dsdt_phys = xdsdt;
    }
    const acpi_sdt_header_t *dsdt_hdr = acpi_map_table(dsdt_phys);
    if (!dsdt_hdr) {
        KLOG("acpi: DSDT address is null\n");
        return 0;
    }
    diag_dsdt = (const uint8_t *)dsdt_hdr;
    diag_dsdt_len = dsdt_hdr->length;
    if (!acpi_checksum(dsdt_hdr, dsdt_hdr->length)) {
        KLOG("acpi: DSDT checksum bad\n");
        return 0;
    }

    if (!scan_s5((const uint8_t *)dsdt_hdr, dsdt_hdr->length,
                 &slp_typa, &slp_typb)) {
        /* Diagnostic: dump the bytes around any "_S5_" so an unrecognised AML
         * encoding (a hypervisor whose firmware soft-off still fails) can be
         * decoded from the serial log -- pm1a port + DSDT len + a hex window. */
        KLOG("acpi: _S5_ not found in DSDT (soft-off will fail)\n");
        KLOG("acpi: pm1a_cnt port="); KLOG_HEX(pm1a_cnt_port); KLOG("\n");
        KLOG("acpi: dsdt len=");      KLOG_HEX(dsdt_hdr->length); KLOG("\n");
        const uint8_t *d = (const uint8_t *)dsdt_hdr;
        for (uint32_t i = 36; i + 4 < dsdt_hdr->length; i++) {
            if (memcmp(d + i, "_S5_", 4) != 0) continue;
            uint32_t lo = (i >= 4) ? i - 4 : 0;
            KLOG("acpi: _S5_ window: ");
            for (uint32_t k = lo; k < i + 16 && k < dsdt_hdr->length; k++) {
                KLOG_HEX(d[k]); KLOG(" ");
            }
            KLOG("\n");
            break;
        }
        return 0;
    }

    KLOG("acpi: init OK\n");
    acpi_enabled = 1;
    return 1;
}

/* Print the ACPI soft-off parse state to the *screen* (serial is impractical on
 * VMware/VirtualBox/Hyper-V).  Shown when soft-off is about to fail so it can be
 * photographed: which stage broke, and the AML bytes around \_S5_ for decoding. */
static void acpi_diag(void)
{
    t_writestring("\n--- ACPI soft-off diagnostic (photograph this) ---\n");
    t_writestring("RSDT=");    t_hex(diag_rsdt_addr);
    t_writestring(" XSDT=");   t_hex(diag_xsdt_addr);
    t_writestring(" FADT=");   t_writestring(diag_fadt_ok ? "ok" : "MISSING");
    t_writestring("\npm1a=");  t_hex(pm1a_cnt_port);
    t_writestring(" dsdt=");   t_hex((uint32_t)(uintptr_t)diag_dsdt);
    t_writestring(" len=");    t_hex(diag_dsdt_len);
    t_writestring(" enabled="); t_dec(acpi_enabled);
    t_writestring(" smi=");    t_hex(smi_cmd_port);
    t_writestring(" sci_en="); t_dec(pm1a_cnt_port ? (inw(pm1a_cnt_port) & 0x1) : 0);
    t_writestring("\n");
    if (diag_dsdt && diag_dsdt_len) {
        int found = 0;
        for (uint32_t i = 36; i + 4 < diag_dsdt_len; i++) {
            if (memcmp(diag_dsdt + i, "_S5_", 4) != 0) continue;
            found = 1;
            uint32_t lo = (i >= 2) ? i - 2 : 0;
            t_writestring("_S5_ AML:");
            for (uint32_t k = lo; k < i + 18 && k < diag_dsdt_len; k++) {
                t_writestring(" "); t_hex(diag_dsdt[k]);
            }
            t_writestring("\n");
            break;
        }
        if (!found) t_writestring("_S5_ not present in DSDT\n");
    }
}

/*
 * acpi_enable_mode – bring the platform into ACPI mode if firmware left it in
 * legacy/SMM mode (SCI_EN, bit 0 of PM1a_CNT, clear).  Mirrors Linux's
 * acpi_enable(): write the FADT ACPI_ENABLE value to SMI_CMD and poll SCI_EN.
 *
 * Hyper-V Gen1 boots in legacy mode, so the S5 SLP write to PM1a_CNT is ignored
 * until this handshake completes; QEMU/Bochs/VMware/VirtualBox boot with ACPI
 * already enabled (and hardware-reduced platforms have no SMI_CMD), so this is a
 * no-op there.
 */
static void acpi_enable_mode(void)
{
    if (!pm1a_cnt_port)             return;     /* nothing to drive            */
    if (inw(pm1a_cnt_port) & 0x1)   return;     /* SCI_EN already set          */
    if (!smi_cmd_port || !acpi_enable_val) return; /* HW-reduced / not needed  */

    outb(smi_cmd_port, acpi_enable_val);
    for (int t = 0; t < 1000000; t++) {
        if (inw(pm1a_cnt_port) & 0x1) break;    /* SCI_EN came up              */
        asm volatile("pause");
    }
}

__attribute__((noreturn)) void acpi_shutdown(void)
{
    t_writestring("System shutting down...\n");

    /* 1. Full ACPI S5 power-off. */
    if (acpi_enabled) {
        acpi_enable_mode();   /* enter ACPI mode first (Hyper-V Gen1 needs it) */
        outw(pm1a_cnt_port, slp_typa | (uint16_t)SLP_EN);
        if (pm1b_cnt_port)
            outw(pm1b_cnt_port, slp_typb | (uint16_t)SLP_EN);
        /* Give hardware a moment; if we reach here the write didn't work. */
    }

    /* 2. QEMU new-style ACPI power-off (port 0x604, value 0x2000). */
    outw(0x604, 0x2000);

    /* 3. Bochs / old QEMU power-off (port 0xB004, value 0x2000). */
    outw(0xB004, 0x2000);

    /* 4. Soft-off didn't take.  Show the ACPI parse state on screen (no serial
     * needed) so a hypervisor that still hangs can be diagnosed from a photo,
     * then disable interrupts and spin on HLT. */
    acpi_diag();
    t_writestring("It is now safe to turn off your computer.\n");
    asm volatile("cli");
    for (;;)
        asm volatile("hlt");
}

/* ---------------------------------------------------------------------------
 * acpi_reboot – reset the machine.
 *
 * Tries (in order):
 *   1. ACPI RESET_REG (FADT revision >= 2, I/O-port variant only).
 *   2. PS/2 keyboard controller CPU-reset pulse (port 0x64, command 0xFE).
 *   3. Triple-fault: load a zero-limit IDT and fire int $0.
 * ------------------------------------------------------------------------- */

/* Wait for the PS/2 controller input buffer to be empty, then send cmd. */
static void kbd_reset(void)
{
    int timeout = 100000;
    while ((inb(0x64) & 0x02) && --timeout)
        ; /* spin */
    outb(0x64, 0xFE); /* pulse CPU RESET# line */
}

/* Triple-fault reboot: clobber the IDT limit to 0 and trigger an interrupt. */
static __attribute__((noreturn)) void triple_fault_reboot(void)
{
    struct { uint16_t limit; uint32_t base; } __attribute__((packed)) null_idt = {0, 0};
    asm volatile("lidt %0" :: "m"(null_idt));
    asm volatile("int $0");
    for (;;) asm volatile("hlt");
}

__attribute__((noreturn)) void acpi_reboot(void)
{
    t_writestring("System rebooting...\n");

    /* 1. ACPI RESET_REG (only if we parsed a FADT long enough to contain it). */
    if (acpi_enabled
        && fadt_raw != NULL
        && fadt_len >= FADT_MIN_LEN_RESET) {

        uint8_t space = fadt_raw[FADT_OFFSET_RESET_SPACE];
        if (space == 1) { /* 1 = System I/O space */
            uint16_t port = (uint16_t)(fadt_raw[FADT_OFFSET_RESET_ADDR]
                          | ((uint16_t)fadt_raw[FADT_OFFSET_RESET_ADDR + 1] << 8));
            uint8_t  val  = fadt_raw[FADT_OFFSET_RESET_VALUE];
            outb(port, val);
            /* Short spin - most hardware resets within microseconds. */
            for (volatile int i = 0; i < 100000; i++)
                asm volatile("pause");
        }
    }

    /* 2. PS/2 keyboard controller reset pulse. */
    kbd_reset();
    for (volatile int i = 0; i < 100000; i++)
        asm volatile("pause");

    /* 3. Triple-fault (last resort - always works). */
    triple_fault_reboot();
}
