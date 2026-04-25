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

/* Cached for reboot: raw FADT pointer and its length. */
static const uint8_t *fadt_raw  = NULL;
static uint32_t       fadt_len  = 0;

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

/*
 * scan_s5 – search a DSDT byte stream for the \_S5_ AML package.
 *
 * The AML encoding for a typical _S5_ object looks like:
 *   08 5F 53 35 5F  12 06  0A SLP_TYPa  0A SLP_TYPb  ...
 *   (DefName  "_S5_"  DefPackage  pkgLen  ByteConst  val  ByteConst  val)
 *
 * We search for the literal bytes "_S5_" (without the leading 08) and then
 * look backwards/forwards for the context bytes.
 *
 * Returns 1 on success and writes *typa, *typb; 0 if not found.
 */
static int scan_s5(const uint8_t *dsdt_data, uint32_t dsdt_len,
                   uint16_t *typa, uint16_t *typb)
{
    /* Walk the DSDT body (skip the 36-byte SDT header). */
    for (uint32_t i = 36; i + 8 < dsdt_len; i++) {
        if (memcmp(dsdt_data + i, "_S5_", 4) != 0)
            continue;

        /*
         * Found "_S5_".  The preceding byte should be 0x08 (DefName opcode).
         * After the name, expect: 0x12 (DefPackage), pkgLen byte, then
         * 0x0A (ByteConst) SLP_TYPa, 0x0A (ByteConst) SLP_TYPb.
         *
         * Some implementations use 0x00 (ZeroOp) instead of 0x0A 0x00.
         */
        if (i < 1 || dsdt_data[i - 1] != 0x08)
            continue;

        uint32_t j = i + 4; /* skip "_S5_" */

        /* Optional: skip 0x12 0xNN (DefPackage opcode + length). */
        if (j + 2 < dsdt_len && dsdt_data[j] == 0x12)
            j += 2; /* skip opcode and 1-byte length */

        /* Skip a count byte that DefPackage may emit. */
        if (j < dsdt_len && dsdt_data[j] == 0x04)
            j++;

        /* Read SLP_TYPa. */
        uint8_t ta, tb;
        if (j + 1 >= dsdt_len) continue;
        if (dsdt_data[j] == 0x0A) {           /* ByteConst */
            ta = dsdt_data[j + 1]; j += 2;
        } else if (dsdt_data[j] == 0x00) {    /* ZeroOp */
            ta = 0; j += 1;
        } else {
            continue;
        }

        /* Read SLP_TYPb. */
        if (j + 1 > dsdt_len) continue;
        if (dsdt_data[j] == 0x0A) {
            tb = dsdt_data[j + 1];
        } else if (dsdt_data[j] == 0x00) {
            tb = 0;
        } else {
            continue;
        }

        *typa = (uint16_t)ta << 10;
        *typb = (uint16_t)tb << 10;
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

int acpi_init(void)
{
    const acpi_rsdp_t *rsdp = locate_rsdp();
    if (!rsdp) {
        KLOG("acpi: RSDP not found\n");
        return 0;
    }
    KLOG("acpi: RSDP found\n");

    /* Map and validate the RSDT. */
    const acpi_sdt_header_t *rsdt = acpi_map_table(rsdp->rsdt_address);
    if (!rsdt) {
        KLOG("acpi: RSDT address is null\n");
        return 0;
    }
    if (!acpi_checksum(rsdt, rsdt->length)) {
        KLOG("acpi: RSDT checksum bad\n");
        return 0;
    }

    const uint32_t *entry = (const uint32_t *)(rsdt + 1);
    uint32_t n_entries = (rsdt->length - sizeof(acpi_sdt_header_t)) / 4;

    /* Find the FADT ("FACP") entry, mapping each candidate before reading it. */
    const acpi_fadt_t *fadt = NULL;
    for (uint32_t i = 0; i < n_entries; i++) {
        uint32_t entry_phys = entry[i];
        if (entry_phys == 0)
            continue;

        /* Map enough of this entry to read the 4-byte signature. */
        paging_map_region(entry_phys, sizeof(acpi_sdt_header_t));

        const acpi_sdt_header_t *hdr =
            (const acpi_sdt_header_t *)(uintptr_t)entry_phys;

        if (memcmp(hdr->signature, "FACP", 4) == 0) {
            /* Map the full FADT now that we know its address. */
            if (hdr->length > sizeof(acpi_sdt_header_t))
                paging_map_region(entry_phys, hdr->length);
            fadt = (const acpi_fadt_t *)hdr;
            break;
        }
    }
    if (!fadt) {
        KLOG("acpi: FADT not found in RSDT\n");
        return 0;
    }
    if (!acpi_checksum(fadt, fadt->hdr.length)) {
        KLOG("acpi: FADT checksum bad\n");
        return 0;
    }

    pm1a_cnt_port = (uint16_t)fadt->pm1a_cnt_blk;
    pm1b_cnt_port = (uint16_t)fadt->pm1b_cnt_blk;

    /* Cache raw FADT bytes for acpi_reboot(). */
    fadt_raw = (const uint8_t *)fadt;
    fadt_len = fadt->hdr.length;

    /* Map and validate the DSDT, then scan for \_S5_. */
    const acpi_sdt_header_t *dsdt_hdr = acpi_map_table(fadt->dsdt);
    if (!dsdt_hdr) {
        KLOG("acpi: DSDT address is null\n");
        return 0;
    }
    if (!acpi_checksum(dsdt_hdr, dsdt_hdr->length)) {
        KLOG("acpi: DSDT checksum bad\n");
        return 0;
    }

    if (!scan_s5((const uint8_t *)dsdt_hdr, dsdt_hdr->length,
                 &slp_typa, &slp_typb)) {
        KLOG("acpi: _S5_ not found in DSDT\n");
        return 0;
    }

    KLOG("acpi: init OK\n");
    acpi_enabled = 1;
    return 1;
}

__attribute__((noreturn)) void acpi_shutdown(void)
{
    t_writestring("System shutting down...\n");

    /* 1. Full ACPI S5 power-off. */
    if (acpi_enabled) {
        outw(pm1a_cnt_port, slp_typa | (uint16_t)SLP_EN);
        if (pm1b_cnt_port)
            outw(pm1b_cnt_port, slp_typb | (uint16_t)SLP_EN);
        /* Give hardware a moment; if we reach here the write didn't work. */
    }

    /* 2. QEMU new-style ACPI power-off (port 0x604, value 0x2000). */
    outw(0x604, 0x2000);

    /* 3. Bochs / old QEMU power-off (port 0xB004, value 0x2000). */
    outw(0xB004, 0x2000);

    /* 4. Last resort: disable interrupts and spin on HLT. */
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
            /* Short spin — most hardware resets within microseconds. */
            for (volatile int i = 0; i < 100000; i++)
                asm volatile("pause");
        }
    }

    /* 2. PS/2 keyboard controller reset pulse. */
    kbd_reset();
    for (volatile int i = 0; i < 100000; i++)
        asm volatile("pause");

    /* 3. Triple-fault (last resort — always works). */
    triple_fault_reboot();
}
