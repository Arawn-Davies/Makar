/*
 * ide.c - ATA PIO driver (28-bit LBA, polling mode).
 *
 * Supports up to four drives across two channels:
 *   Index 0: primary   master  (base 0x1F0, ctrl 0x3F6)
 *   Index 1: primary   slave   (base 0x1F0, ctrl 0x3F6)
 *   Index 2: secondary master  (base 0x170, ctrl 0x376)
 *   Index 3: secondary slave   (base 0x170, ctrl 0x376)
 *
 * ATAPI devices are detected but only ATA (hard-disk) drives support
 * sector read/write through this driver.
 */

#include <kernel/ide.h>
#include <kernel/asm.h>
#include <kernel/tty.h>
#include <kernel/serial.h>
#include <kernel/debug.h>
#include <kernel/pci.h>

#include <stddef.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * ATA command-block register offsets (relative to channel I/O base)
 * ---------------------------------------------------------------------- */
#define ATA_REG_DATA      0x00  /* Data port (16-bit R/W)         */
#define ATA_REG_ERROR     0x01  /* Error info (R)                  */
#define ATA_REG_FEATURES  0x01  /* Features   (W)                  */
#define ATA_REG_SECCOUNT  0x02  /* Sector Count                    */
#define ATA_REG_LBA0      0x03  /* LBA bits  0-7                   */
#define ATA_REG_LBA1      0x04  /* LBA bits  8-15                  */
#define ATA_REG_LBA2      0x05  /* LBA bits 16-23                  */
#define ATA_REG_HDDEVSEL  0x06  /* Drive / Head select             */
#define ATA_REG_STATUS    0x07  /* Status  (R)                     */
#define ATA_REG_COMMAND   0x07  /* Command (W)                     */

/* -------------------------------------------------------------------------
 * ATA status register bits
 * ---------------------------------------------------------------------- */
#define ATA_SR_BSY   0x80  /* Busy                        */
#define ATA_SR_DRDY  0x40  /* Drive ready                 */
#define ATA_SR_DF    0x20  /* Drive write fault           */
#define ATA_SR_DRQ   0x08  /* Data request (ready to transfer) */
#define ATA_SR_ERR   0x01  /* Error                       */

/* -------------------------------------------------------------------------
 * ATA commands
 * ---------------------------------------------------------------------- */
#define ATA_CMD_READ_PIO    0x20  /* Read sectors (LBA28, PIO)   */
#define ATA_CMD_WRITE_PIO   0x30  /* Write sectors (LBA28, PIO)  */
#define ATA_CMD_READ_DMA    0xC8  /* Read sectors (LBA28, DMA)   */
#define ATA_CMD_WRITE_DMA   0xCA  /* Write sectors (LBA28, DMA)  */
#define ATA_CMD_CACHE_FLUSH 0xE7  /* Flush write cache           */
#define ATA_CMD_IDENTIFY    0xEC  /* Identify ATA device         */
#define ATA_CMD_PACKET      0xA0  /* ATAPI PACKET command        */
#define ATAPI_CMD_IDENTIFY  0xA1  /* Identify ATAPI device       */
#define ATAPI_CMD_READ12    0xA8  /* ATAPI READ(12) command      */
#define ATAPI_CMD_READ_CAP  0x25  /* ATAPI READ CAPACITY(10)     */

/* CD-ROM sector size (2048 bytes per ISO9660 logical sector). */
#define ATAPI_CD_SECTOR_SIZE  2048

/* -------------------------------------------------------------------------
 * Drive-select byte components for the HDDEVSEL register
 * ---------------------------------------------------------------------- */
#define ATA_SEL_MASTER  0xA0  /* Select master (bit4=0)        */
#define ATA_SEL_SLAVE   0xB0  /* Select slave  (bit4=1)        */
#define ATA_SEL_LBA     0x40  /* LBA addressing mode (bit6=1)  */

/* -------------------------------------------------------------------------
 * IDENTIFY response buffer byte offsets (one word = two bytes)
 * ---------------------------------------------------------------------- */
#define IDENT_DEVICETYPE    0    /* Word  0 */
#define IDENT_CAPABILITIES  98   /* Word 49 */
#define IDENT_FIELDVALID    106  /* Word 53 */
#define IDENT_MAX_LBA       120  /* Word 60 – 28-bit sector count */
#define IDENT_COMMANDSETS   164  /* Word 82 */
#define IDENT_MAX_LBA_EXT   200  /* Word 100 – 48-bit sector count (lo 32 b) */
#define IDENT_MODEL         54   /* Word 27 – 40 bytes of model string */

/* -------------------------------------------------------------------------
 * Channel descriptors
 * ---------------------------------------------------------------------- */
typedef struct {
    uint16_t base;   /* Command-block I/O base  */
    uint16_t ctrl;   /* Control-block I/O base  */
} ide_channel_t;

static const ide_channel_t channels[2] = {
    { 0x1F0, 0x3F6 },   /* Primary channel   */
    { 0x170, 0x376 },   /* Secondary channel */
};

/* Drive table: indices 0-3 as documented above. */
static ide_drive_t drives[IDE_MAX_DRIVES];

/* -------------------------------------------------------------------------
 * Bus-master IDE (BMIDE) DMA.
 *
 * The PCI IDE controller (PIIX and friends, emulated identically by QEMU,
 * VirtualBox, Hyper-V Gen 1, VMware and real hardware) can copy sector data
 * straight to/from memory instead of the per-word port loop the PIO path uses.
 * On a VT-x hypervisor every port access is a VM exit, so PIO costs ~256 exits
 * per sector; DMA costs a handful of exits per transfer regardless of size.
 *
 * Polling implementation: nIEN stays set (no IRQs) and completion is detected
 * by polling the BMIDE Active bit with a bounded timeout.  Polling is the safe
 * choice across every hypervisor and emulator -- emulated controllers complete
 * the transfer synchronously when the engine is started, so the poll returns
 * almost immediately (no busy-wait of VM exits), and there is no task-sleep to
 * collide with the kernel's non-preemptible-syscall model.  Transfers go
 * through a static, 64 KiB-aligned bounce buffer (identity-mapped, so its
 * virtual address is its physical address) and are memcpy'd to/from the caller,
 * sidestepping PRD physical-contiguity / 64 KiB-boundary constraints on
 * arbitrary caller buffers.
 *
 * Linux-style resilience so the same binary works on type-1/type-2 hypervisors,
 * paravirtual and fully-emulated platforms alike: DMA is used only for drives
 * whose IDENTIFY advertises it; every setup/transfer failure soft-resets the
 * channel and retries the request via PIO; and after a few consecutive failures
 * the driver demotes itself to PIO entirely.  A platform whose BMIDE we cannot
 * drive therefore still works (just slower) instead of hanging or erroring.
 * (Paravirtual virtio-blk and AHCI/NVMe are separate drivers, not this path.)
 * ---------------------------------------------------------------------- */

/* BMIDE register offsets, relative to (s_bmide_base + channel * 8). */
#define BM_REG_CMD     0x00  /* command:  bit0 Start, bit3 direction       */
#define BM_REG_STATUS  0x02  /* status:   bit0 Active, bit1 Err, bit2 IRQ  */
#define BM_REG_PRDT    0x04  /* 32-bit physical address of the PRDT        */

#define BM_CMD_START   0x01u /* start the DMA engine                       */
#define BM_CMD_TO_MEM  0x08u /* direction = bus master writes memory (read)*/
#define BM_SR_ACTIVE   0x01u
#define BM_SR_ERR      0x02u
#define BM_SR_IRQ      0x04u

/* Physical Region Descriptor: one entry is enough for a <=64 KiB transfer. */
typedef struct __attribute__((packed)) {
    uint32_t addr;    /* physical base of the data buffer */
    uint16_t count;   /* byte count (0 == 64 KiB)         */
    uint16_t flags;   /* 0x8000 = end-of-table            */
} prd_t;

/* Per-transfer chunk: 32 KiB through the bounce buffer.  64 sectors of 512 B
 * (ATA) or 16 sectors of 2048 B (ATAPI).  Keeps the PRD count nonzero and,
 * with a 64 KiB-aligned buffer, never crosses a 64 KiB boundary. */
#define DMA_BOUNCE_BYTES   65536u
#define DMA_CHUNK_BYTES    32768u
#define DMA_ATA_CHUNK_SECS (DMA_CHUNK_BYTES / 512u)    /* 64  */
#define DMA_ATAPI_CHUNK_SECS (DMA_CHUNK_BYTES / ATAPI_CD_SECTOR_SIZE) /* 16 */

/* IDENTIFY word 49, bit 8: device supports DMA (drives[].capabilities). */
#define ATA_CAP_DMA    0x0100u

#define DMA_MAX_FAILS  3        /* consecutive DMA errors before global demotion */

static uint16_t s_bmide_base  = 0;  /* 0 = no bus-master IDE controller found    */
static int      s_dma_enabled = 0;  /* armed once the controller binds; cleared  */
                                    /* (demoted to PIO) after repeated DMA errors */
static int      s_dma_fails   = 0;  /* consecutive DMA failures                  */
static uint8_t  s_dma_buf[DMA_BOUNCE_BYTES] __attribute__((aligned(65536)));
static prd_t    s_prdt[1] __attribute__((aligned(16)));

static int ide_poll(uint8_t ch, int check_drq);   /* defined below */

/* Linux-style resilience: a DMA error is never fatal while PIO can do the job.
 * Each failure soft-resets the channel and the caller retries via PIO; after a
 * few consecutive failures we demote the whole driver to PIO so a hypervisor
 * whose BMIDE we mis-drive still works (just slower) instead of erroring. */
static void ide_soft_reset(uint8_t ch);   /* defined below */

static void dma_note_failure(uint8_t ch)
{
    ide_soft_reset(ch);
    if (++s_dma_fails >= DMA_MAX_FAILS) {
        s_dma_enabled = 0;
        Serial_WriteString("ide: repeated DMA errors -> disabled, using PIO\n");
    }
}

static inline void dma_note_success(void) { s_dma_fails = 0; }

/* DMA usable for this ATA drive right now? (controller present, not demoted,
 * drive present + ATA + advertises DMA in its IDENTIFY capabilities). */
static int dma_ata_usable(uint8_t drive_num)
{
    return s_bmide_base && s_dma_enabled &&
           drive_num < IDE_MAX_DRIVES && drives[drive_num].present &&
           drives[drive_num].type == IDE_TYPE_ATA &&
           (drives[drive_num].capabilities & ATA_CAP_DMA);
}

static int dma_atapi_usable(uint8_t drive_num)
{
    return s_bmide_base && s_dma_enabled &&
           drive_num < IDE_MAX_DRIVES && drives[drive_num].present &&
           drives[drive_num].type == IDE_TYPE_ATAPI &&
           (drives[drive_num].capabilities & ATA_CAP_DMA);
}

/* Disk I/O runs with the timer effectively stalled (long polled waits, often
 * with interrupts masked in syscall context), so the boot/status spinner
 * freezes and a slow read/write looks like a hang.  Pump it directly from the
 * I/O path: animation then tracks actual disk progress.  `t_spinner_tick`
 * advances one frame per 12 counts, so step the counter by 12 each pump. */
static uint32_t s_io_spin = 0;
static inline void io_spin_pump(void)
{
    s_io_spin += 12u;
    t_spinner_tick(s_io_spin);
}

/* Program the BMIDE engine for one transfer and clear stale status bits.
 * to_mem != 0 selects device->memory (a read). */
static void bm_setup(uint8_t ch, uint16_t nbytes, int to_mem)
{
    uint16_t bm = (uint16_t)(s_bmide_base + ch * 8);

    s_prdt[0].addr  = (uint32_t)(uintptr_t)s_dma_buf;  /* phys == virt */
    s_prdt[0].count = nbytes;
    s_prdt[0].flags = 0x8000u;                         /* EOT */

    outb(bm + BM_REG_CMD, 0x00);                       /* stop engine */
    outl(bm + BM_REG_PRDT, (uint32_t)(uintptr_t)s_prdt);
    outb(bm + BM_REG_CMD, to_mem ? BM_CMD_TO_MEM : 0x00);
    outb(bm + BM_REG_STATUS, (uint8_t)(BM_SR_ERR | BM_SR_IRQ)); /* W1C */
}

/* Start the engine, then poll the Active bit until the transfer drains.
 * Returns 0 on success, positive on a BMIDE/drive error. */
static int bm_run_and_wait(uint8_t ch, int to_mem)
{
    uint16_t bm = (uint16_t)(s_bmide_base + ch * 8);

    outb(bm + BM_REG_CMD,
         (uint8_t)((to_mem ? BM_CMD_TO_MEM : 0x00) | BM_CMD_START));

    uint32_t limit = 5000000;
    uint8_t  sr;
    do {
        sr = inb(bm + BM_REG_STATUS);
        if ((limit & 0x3FFFFu) == 0)        /* keep the spinner alive on long DMA */
            io_spin_pump();
        if (--limit == 0)
            break;
    } while ((sr & BM_SR_ACTIVE) && !(sr & BM_SR_ERR));

    outb(bm + BM_REG_CMD, 0x00);                        /* stop engine */
    outb(bm + BM_REG_STATUS, (uint8_t)(BM_SR_ERR | BM_SR_IRQ)); /* W1C */

    if (limit == 0 || (sr & BM_SR_ERR))
        return 1;
    /* Drain the ATA side: BSY must clear with no ERR/DF. */
    return ide_poll(ch, 0);
}

/* -------------------------------------------------------------------------
 * Low-level I/O helpers
 * ---------------------------------------------------------------------- */

static inline uint8_t ide_read(uint8_t ch, uint8_t reg)
{
    return inb((uint16_t)(channels[ch].base + reg));
}

static inline void ide_write(uint8_t ch, uint8_t reg, uint8_t val)
{
    outb((uint16_t)(channels[ch].base + reg), val);
}

/* Read the alternate-status register - does NOT clear a pending IRQ. */
static inline uint8_t ide_read_altstatus(uint8_t ch)
{
    return inb(channels[ch].ctrl);
}

/* Generate a ~400 ns delay by reading alternate-status four times. */
static inline void ide_400ns_delay(uint8_t ch)
{
    ide_read_altstatus(ch);
    ide_read_altstatus(ch);
    ide_read_altstatus(ch);
    ide_read_altstatus(ch);
}

/*
 * ide_poll – wait for BSY to clear then optionally check for DRQ.
 *
 * Returns:
 *   0  – success
 *   1  – error bit set in status register
 *   2  – drive fault
 *   3  – DRQ not set (when check_drq != 0)
 */
static int ide_poll(uint8_t ch, int check_drq)
{
    ide_400ns_delay(ch);

    /* Bounded poll: ~5 million iterations covers any realistic ATA response
     * time under QEMU TCG.  If BSY never clears the drive is gone and we
     * must not spin forever. */
    io_spin_pump();                         /* one frame per poll = per sector/chunk */

    uint8_t status;
    uint32_t limit = 5000000;
    do {
        status = ide_read_altstatus(ch);
        if ((limit & 0x3FFFFu) == 0)        /* keep moving during a long wait */
            io_spin_pump();
        if (--limit == 0)
            KPANIC("ide_poll: ATA drive BSY never cleared (drive hung or absent)");
    } while (status & ATA_SR_BSY);

    if (status & ATA_SR_ERR)  return 1;
    if (status & ATA_SR_DF)   return 2;
    if (check_drq && !(status & ATA_SR_DRQ)) return 3;

    return 0;
}

/* Software-reset one channel (ATA spec §9.1): pulse SRST in the Device Control
 * register with nIEN held so no IRQs fire, then let the drives recalibrate.
 * Used at probe time and to recover a channel after a DMA error. */
static void ide_soft_reset(uint8_t ch)
{
    outb(channels[ch].ctrl, 0x06);   /* nIEN | SRST */
    for (int r = 0; r < 5; r++) ide_400ns_delay(ch);
    outb(channels[ch].ctrl, 0x02);   /* nIEN, SRST cleared */
    for (int r = 0; r < 250; r++) ide_400ns_delay(ch);  /* ~100 µs settle */
}

/* -------------------------------------------------------------------------
 * ide_init
 * ---------------------------------------------------------------------- */
void ide_init(void)
{
    uint8_t identify_buf[512];

    /*
     * Software-reset both channels before probing.  GRUB leaves the primary
     * channel's drive selected and potentially still busy after loading the
     * kernel; without a reset, IDENTIFY returns 0x00 and the drive is skipped.
     *
     * Sequence (ATA spec §9.1):
     *   1. Assert SRST (bit 2) in the Device Control register while keeping
     *      nIEN (bit 1) set so no IRQs fire.
     *   2. Hold for ≥5 µs (a few reads of alt-status is sufficient).
     *   3. Deassert SRST; drives recalibrate in ≤31 ms (QEMU is instant).
     *   4. Wait briefly before issuing any commands.
     */
    ide_soft_reset(0);
    ide_soft_reset(1);

    for (uint8_t ch = 0; ch < 2; ch++) {
        for (uint8_t dr = 0; dr < 2; dr++) {
            uint8_t idx = (uint8_t)(ch * 2 + dr);
            drives[idx].present = 0;

            /* Select the drive. */
            ide_write(ch, ATA_REG_HDDEVSEL,
                      (dr == 0) ? ATA_SEL_MASTER : ATA_SEL_SLAVE);
            ide_400ns_delay(ch);

            /* Issue IDENTIFY. */
            ide_write(ch, ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
            ide_400ns_delay(ch);

            /* A status of 0x00 or 0xFF means no drive is present on this
             * slot (0xFF = floating bus lines, common when a slave exists
             * without a master on the same channel). */
            uint8_t st0 = ide_read(ch, ATA_REG_STATUS);
            if (st0 == 0x00u || st0 == 0xFFu)
                continue;

            /* Wait for BSY to clear (with a timeout guard). */
            uint32_t timeout = 0;
            uint8_t status;
            do {
                status = ide_read(ch, ATA_REG_STATUS);
                timeout++;
            } while ((status & ATA_SR_BSY) && timeout < 100000);

            if (timeout >= 100000)
                continue;

            /* Classify: ATA vs ATAPI (OSDev Wiki detection method). */
            uint8_t drive_type = IDE_TYPE_ATA;
            uint8_t lba_md = ide_read(ch, ATA_REG_LBA1);
            uint8_t lba_hi = ide_read(ch, ATA_REG_LBA2);

            if (lba_md == 0x14 && lba_hi == 0xEB) {
                drive_type = IDE_TYPE_ATAPI;
                ide_write(ch, ATA_REG_COMMAND, ATAPI_CMD_IDENTIFY);
                ide_400ns_delay(ch);
            } else if (lba_md != 0x00 || lba_hi != 0x00) {
                /* Unknown / non-standard signature - skip slot. */
                continue;
            }

            /* Wait for DRQ or ERR. */
            do {
                status = ide_read(ch, ATA_REG_STATUS);
            } while (!(status & (ATA_SR_DRQ | ATA_SR_ERR)));

            if (status & ATA_SR_ERR)
                continue;

            /* Read the 512-byte IDENTIFY response (256 16-bit words). */
            uint16_t *id = (uint16_t *)(void *)identify_buf;
            for (int i = 0; i < 256; i++)
                id[i] = inw(channels[ch].base + ATA_REG_DATA);

            /* Fill in the drive descriptor. */
            drives[idx].present      = 1;
            drives[idx].channel      = ch;
            drives[idx].drive        = dr;
            drives[idx].type         = drive_type;
            drives[idx].signature    = id[IDENT_DEVICETYPE / 2];
            drives[idx].capabilities = id[IDENT_CAPABILITIES / 2];
            drives[idx].command_sets =
                (uint32_t)id[IDENT_COMMANDSETS / 2] |
                ((uint32_t)id[IDENT_COMMANDSETS / 2 + 1] << 16);

            /* Use 48-bit LBA sector count when the drive supports it. */
            if (drives[idx].command_sets & (1u << 26))
                drives[idx].size =
                    (uint32_t)id[IDENT_MAX_LBA_EXT / 2] |
                    ((uint32_t)id[IDENT_MAX_LBA_EXT / 2 + 1] << 16);
            else
                drives[idx].size =
                    (uint32_t)id[IDENT_MAX_LBA / 2] |
                    ((uint32_t)id[IDENT_MAX_LBA / 2 + 1] << 16);

            /* Copy model string (big-endian byte pairs in IDENTIFY buffer). */
            for (int k = 0; k < 40; k += 2) {
                drives[idx].model[k]     = identify_buf[IDENT_MODEL + k + 1];
                drives[idx].model[k + 1] = identify_buf[IDENT_MODEL + k];
            }
            drives[idx].model[40] = '\0';

            /* Trim trailing spaces. */
            int end = 39;
            while (end >= 0 && drives[idx].model[end] == ' ')
                drives[idx].model[end--] = '\0';
        }
    }
}

/* -------------------------------------------------------------------------
 * ide_access – internal read/write dispatcher (LBA28, PIO polling).
 *
 * direction: 0 = read, 1 = write
 * ---------------------------------------------------------------------- */
static int ide_access(uint8_t direction, uint8_t drive_num,
                      uint32_t lba, uint8_t count, void *buf)
{
    if (drive_num >= IDE_MAX_DRIVES || !drives[drive_num].present)
        return -1;

    if (drives[drive_num].type != IDE_TYPE_ATA)
        return -2;   /* ATAPI not supported by this PIO driver */

    if (count == 0)
        return 0;

    uint8_t  ch = drives[drive_num].channel;
    uint8_t  dr = drives[drive_num].drive;
    uint16_t *wbuf = (uint16_t *)buf;

    /* Disable IRQs. */
    outb(channels[ch].ctrl, 0x02);

    /*
     * Send LBA28 parameters.
     * HDDEVSEL bits: [7]=1 [6]=LBA [5]=1 [4]=drive [3:0]=LBA[27:24]
     */
    ide_write(ch, ATA_REG_HDDEVSEL,
              (uint8_t)((dr == 0 ? ATA_SEL_MASTER : ATA_SEL_SLAVE) |
                        ATA_SEL_LBA |
                        ((lba >> 24) & 0x0F)));
    ide_400ns_delay(ch);

    /* Wait for BSY to clear before writing registers. */
    int err = ide_poll(ch, 0);
    if (err)
        return err;

    ide_write(ch, ATA_REG_FEATURES,  0x00);
    ide_write(ch, ATA_REG_SECCOUNT,  count);
    ide_write(ch, ATA_REG_LBA0,      (uint8_t)(lba));
    ide_write(ch, ATA_REG_LBA1,      (uint8_t)(lba >> 8));
    ide_write(ch, ATA_REG_LBA2,      (uint8_t)(lba >> 16));

    ide_write(ch, ATA_REG_COMMAND,
              direction ? ATA_CMD_WRITE_PIO : ATA_CMD_READ_PIO);

    for (uint8_t s = 0; s < count; s++) {
        /* Wait for the drive to assert DRQ for this sector. */
        err = ide_poll(ch, 1);
        if (err)
            return err;

        if (direction == 0) {
            /* Read 256 words (512 bytes) from the data port. */
            for (int i = 0; i < 256; i++)
                wbuf[s * 256 + i] = inw(channels[ch].base + ATA_REG_DATA);
        } else {
            /* Write 256 words (512 bytes) to the data port. */
            for (int i = 0; i < 256; i++)
                outw(channels[ch].base + ATA_REG_DATA,
                     wbuf[s * 256 + i]);
        }
    }

    if (direction == 1) {
        /* Flush the drive's write cache. */
        ide_write(ch, ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
        ide_poll(ch, 0);
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * ide_dma_ata – bus-master DMA read/write for ATA drives, in <=32 KiB chunks
 * through the bounce buffer.  Returns 0 on success, -3 if DMA is unavailable
 * for this request (caller should use PIO), positive on a drive/BMIDE error.
 * ---------------------------------------------------------------------- */
static int ide_dma_ata(uint8_t direction, uint8_t drive_num,
                       uint32_t lba, uint8_t count, void *buf)
{
    if (!dma_ata_usable(drive_num))
        return -3;                            /* not usable: caller uses PIO */
    if (count == 0)
        return 0;

    uint8_t  ch = drives[drive_num].channel;
    uint8_t  dr = drives[drive_num].drive;
    uint8_t *p  = (uint8_t *)buf;
    uint32_t remaining = count;
    uint32_t cur_lba   = lba;

    while (remaining) {
        uint32_t secs   = remaining > DMA_ATA_CHUNK_SECS
                        ? DMA_ATA_CHUNK_SECS : remaining;
        uint16_t nbytes = (uint16_t)(secs * 512u);

        if (direction == 1)                       /* write: stage into bounce */
            memcpy(s_dma_buf, p, nbytes);

        outb(channels[ch].ctrl, 0x02);            /* nIEN: poll, no IRQs */
        bm_setup(ch, nbytes, direction == 0);     /* to_mem on read */

        ide_write(ch, ATA_REG_HDDEVSEL,
                  (uint8_t)((dr == 0 ? ATA_SEL_MASTER : ATA_SEL_SLAVE) |
                            ATA_SEL_LBA | ((cur_lba >> 24) & 0x0F)));
        ide_400ns_delay(ch);
        if (ide_poll(ch, 0))
            return 1;

        ide_write(ch, ATA_REG_FEATURES, 0x00);
        ide_write(ch, ATA_REG_SECCOUNT, (uint8_t)secs);
        ide_write(ch, ATA_REG_LBA0, (uint8_t)(cur_lba));
        ide_write(ch, ATA_REG_LBA1, (uint8_t)(cur_lba >> 8));
        ide_write(ch, ATA_REG_LBA2, (uint8_t)(cur_lba >> 16));
        ide_write(ch, ATA_REG_COMMAND,
                  direction ? ATA_CMD_WRITE_DMA : ATA_CMD_READ_DMA);

        if (bm_run_and_wait(ch, direction == 0))
            return 1;

        if (direction == 0)                       /* read: copy out of bounce */
            memcpy(p, s_dma_buf, nbytes);

        p         += nbytes;
        cur_lba   += secs;
        remaining -= secs;
    }

    if (direction == 1) {
        /* Commit the drive's write cache, exactly like the PIO path.  Without
         * this the tail of a large write (e.g. an OS install) can be lost on
         * reset on real hardware / hypervisors that honour the write cache. */
        ide_write(ch, ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
        ide_poll(ch, 0);
    }
    dma_note_success();
    return 0;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

int ide_read_sectors(uint8_t drive_num, uint32_t lba, uint8_t count,
                     void *buf)
{
    int r = ide_dma_ata(0, drive_num, lba, count, buf);
    if (r == 0)
        return 0;
    if (r != -3)                        /* DMA tried but errored: reset + retry */
        dma_note_failure(drives[drive_num].channel);
    return ide_access(0, drive_num, lba, count, buf);   /* PIO fallback/retry */
}

int ide_write_sectors(uint8_t drive_num, uint32_t lba, uint8_t count,
                      const void *buf)
{
    int r = ide_dma_ata(1, drive_num, lba, count, (void *)buf);
    if (r == 0)
        return 0;
    if (r != -3)
        dma_note_failure(drives[drive_num].channel);
    return ide_access(1, drive_num, lba, count, (void *)buf);   /* PIO */
}

const ide_drive_t *ide_get_drive(uint8_t drive_num)
{
    if (drive_num >= IDE_MAX_DRIVES)
        return NULL;
    return &drives[drive_num];
}

/* -------------------------------------------------------------------------
 * atapi_read_one – read one 2048-byte CD-ROM sector from an ATAPI drive
 * using PIO polling and a READ(12) command packet.
 * ---------------------------------------------------------------------- */
static int atapi_read_one(uint8_t ch, uint8_t dr, uint32_t lba, uint8_t *buf)
{
    uint8_t pkt[12];

    /* Select drive. */
    ide_write(ch, ATA_REG_HDDEVSEL,
              (dr == 0) ? ATA_SEL_MASTER : ATA_SEL_SLAVE);
    ide_400ns_delay(ch);

    /* Configure for PIO data transfer; set byte-count limit to 2048. */
    ide_write(ch, ATA_REG_FEATURES, 0x00);  /* PIO mode, no DMA          */
    ide_write(ch, ATA_REG_LBA1,     0x00);  /* byte count low  (2048&FF) */
    ide_write(ch, ATA_REG_LBA2,     0x08);  /* byte count high (2048>>8) */

    /* Issue the PACKET command. */
    ide_write(ch, ATA_REG_COMMAND, ATA_CMD_PACKET);
    ide_400ns_delay(ch);

    /* Wait for DRQ - device is ready to accept the 12-byte command packet. */
    if (ide_poll(ch, 1))
        return 1;

    /* Build a READ(12) command packet (big-endian LBA, 1 sector). */
    pkt[0]  = ATAPI_CMD_READ12;
    pkt[1]  = 0x00;
    pkt[2]  = (uint8_t)(lba >> 24);
    pkt[3]  = (uint8_t)(lba >> 16);
    pkt[4]  = (uint8_t)(lba >>  8);
    pkt[5]  = (uint8_t)(lba);
    pkt[6]  = 0x00;
    pkt[7]  = 0x00;
    pkt[8]  = 0x00;
    pkt[9]  = 0x01;   /* transfer length: 1 sector */
    pkt[10] = 0x00;
    pkt[11] = 0x00;

    /* Send the packet to the data port as six 16-bit writes. */
    for (int i = 0; i < 6; i++) {
        uint16_t w = (uint16_t)pkt[i * 2]
                   | ((uint16_t)pkt[i * 2 + 1] << 8);
        outw(channels[ch].base + ATA_REG_DATA, w);
    }

    /* Wait for DRQ - data is ready to be read. */
    if (ide_poll(ch, 1))
        return 1;

    /* Read 2048 bytes as 1024 16-bit words. */
    uint16_t *wbuf = (uint16_t *)(void *)buf;
    for (int i = 0; i < (int)(ATAPI_CD_SECTOR_SIZE / 2); i++)
        wbuf[i] = inw(channels[ch].base + ATA_REG_DATA);

    /* Wait for the drive to return to idle. */
    ide_poll(ch, 0);

    return 0;
}

/* -------------------------------------------------------------------------
 * atapi_read_dma – read 'secs' (<=DMA_ATAPI_CHUNK_SECS) 2048-byte CD-ROM
 * sectors into the bounce buffer via a PACKET READ(12) in DMA mode.  Returns
 * 0 on success, positive on error.
 * ---------------------------------------------------------------------- */
static int atapi_read_dma(uint8_t ch, uint8_t dr, uint32_t lba, uint32_t secs)
{
    uint16_t nbytes = (uint16_t)(secs * ATAPI_CD_SECTOR_SIZE);

    outb(channels[ch].ctrl, 0x02);            /* nIEN: poll, no IRQs */

    ide_write(ch, ATA_REG_HDDEVSEL,
              (dr == 0) ? ATA_SEL_MASTER : ATA_SEL_SLAVE);
    ide_400ns_delay(ch);

    /* Arm the DMA engine (device->memory) before issuing the command. */
    bm_setup(ch, nbytes, 1);

    ide_write(ch, ATA_REG_FEATURES, 0x01);    /* bit0 = DMA data transfer */
    ide_write(ch, ATA_REG_LBA1, 0x00);        /* byte-count limit unused for DMA */
    ide_write(ch, ATA_REG_LBA2, 0x00);

    ide_write(ch, ATA_REG_COMMAND, ATA_CMD_PACKET);
    ide_400ns_delay(ch);
    if (ide_poll(ch, 1))                       /* DRQ: ready for the packet */
        return 1;

    /* READ(12): big-endian LBA + transfer length (in logical blocks). */
    uint8_t pkt[12] = {0};
    pkt[0] = ATAPI_CMD_READ12;
    pkt[2] = (uint8_t)(lba >> 24);
    pkt[3] = (uint8_t)(lba >> 16);
    pkt[4] = (uint8_t)(lba >>  8);
    pkt[5] = (uint8_t)(lba);
    pkt[6] = (uint8_t)(secs >> 24);
    pkt[7] = (uint8_t)(secs >> 16);
    pkt[8] = (uint8_t)(secs >>  8);
    pkt[9] = (uint8_t)(secs);
    for (int i = 0; i < 6; i++) {
        uint16_t w = (uint16_t)pkt[i * 2] | ((uint16_t)pkt[i * 2 + 1] << 8);
        outw(channels[ch].base + ATA_REG_DATA, w);
    }

    /* Start the engine and wait for the DMA transfer to drain. */
    return bm_run_and_wait(ch, 1);
}

/* -------------------------------------------------------------------------
 * ide_read_atapi_sectors – read 'count' 2048-byte CD-ROM sectors starting
 * at 'lba' from an ATAPI drive into 'buf'.
 *
 * Returns 0 on success, -1 on invalid args, -2 if drive is not ATAPI,
 * positive on drive error.
 * ---------------------------------------------------------------------- */
int ide_read_atapi_sectors(uint8_t drive_num, uint32_t lba,
                           uint16_t count, void *buf)
{
    if (drive_num >= IDE_MAX_DRIVES || !drives[drive_num].present)
        return -1;

    if (drives[drive_num].type != IDE_TYPE_ATAPI)
        return -2;

    if (count == 0)
        return 0;

    uint8_t  ch = drives[drive_num].channel;
    uint8_t  dr = drives[drive_num].drive;
    uint8_t *p  = (uint8_t *)buf;

    /* Fast path: bus-master DMA in chunks.  On any error soft-reset, demote,
     * and fall back to PIO from the start so a flaky DMA controller never
     * wedges CD reads. */
    if (dma_atapi_usable(drive_num)) {
        int      ok        = 1;
        uint32_t remaining = count;
        uint32_t cur       = lba;
        uint8_t *q         = p;
        while (remaining) {
            uint32_t secs = remaining > DMA_ATAPI_CHUNK_SECS
                          ? DMA_ATAPI_CHUNK_SECS : remaining;
            if (atapi_read_dma(ch, dr, cur, secs)) { ok = 0; break; }
            memcpy(q, s_dma_buf, secs * ATAPI_CD_SECTOR_SIZE);
            q         += secs * ATAPI_CD_SECTOR_SIZE;
            cur       += secs;
            remaining -= secs;
        }
        if (ok) {
            dma_note_success();
            return 0;
        }
        dma_note_failure(ch);
    }

    for (uint16_t i = 0; i < count; i++) {
        int err = atapi_read_one(ch, dr, lba + (uint32_t)i, p);
        if (err)
            return err;
        p += ATAPI_CD_SECTOR_SIZE;
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * ide_atapi_capacity – issue READ CAPACITY(10) and report the medium's
 * sector count and sector size.  Lets devfs expose a real /dev/cdrom size
 * (ATAPI IDENTIFY doesn't carry an LBA range like ATA does).
 *
 * Returns 0 on success (out_sectors = last_lba + 1, out_sec_size = block
 * length, usually 2048), -1 on invalid/absent drive, -2 if not ATAPI,
 * positive on a protocol error.
 * ---------------------------------------------------------------------- */
int ide_atapi_capacity(uint8_t drive_num, uint32_t *out_sectors,
                       uint32_t *out_sec_size)
{
    if (drive_num >= IDE_MAX_DRIVES || !drives[drive_num].present)
        return -1;
    if (drives[drive_num].type != IDE_TYPE_ATAPI)
        return -2;

    uint8_t ch = drives[drive_num].channel;
    uint8_t dr = drives[drive_num].drive;
    uint8_t pkt[12] = {0};

    ide_write(ch, ATA_REG_HDDEVSEL,
              (dr == 0) ? ATA_SEL_MASTER : ATA_SEL_SLAVE);
    ide_400ns_delay(ch);

    /* READ CAPACITY returns an 8-byte parameter block. */
    ide_write(ch, ATA_REG_FEATURES, 0x00);
    ide_write(ch, ATA_REG_LBA1,     0x08);   /* byte count low  (8)  */
    ide_write(ch, ATA_REG_LBA2,     0x00);   /* byte count high      */

    ide_write(ch, ATA_REG_COMMAND, ATA_CMD_PACKET);
    ide_400ns_delay(ch);
    if (ide_poll(ch, 1))
        return 1;

    pkt[0] = ATAPI_CMD_READ_CAP;
    for (int i = 0; i < 6; i++) {
        uint16_t w = (uint16_t)pkt[i * 2] | ((uint16_t)pkt[i * 2 + 1] << 8);
        outw(channels[ch].base + ATA_REG_DATA, w);
    }

    if (ide_poll(ch, 1))
        return 1;

    /* Read the 8-byte response as four 16-bit words. */
    uint16_t resp[4];
    for (int i = 0; i < 4; i++)
        resp[i] = inw(channels[ch].base + ATA_REG_DATA);
    ide_poll(ch, 0);

    /* Bytes arrive little-endian-per-word but the fields are big-endian.
     * Reassemble the byte stream first. */
    uint8_t b[8];
    for (int i = 0; i < 4; i++) {
        b[i * 2]     = (uint8_t)(resp[i] & 0xFF);
        b[i * 2 + 1] = (uint8_t)(resp[i] >> 8);
    }
    uint32_t last_lba = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                        ((uint32_t)b[2] << 8)  |  (uint32_t)b[3];
    uint32_t blk_len  = ((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) |
                        ((uint32_t)b[6] << 8)  |  (uint32_t)b[7];

    if (out_sectors)  *out_sectors  = last_lba + 1u;
    if (out_sec_size) *out_sec_size = blk_len ? blk_len : ATAPI_CD_SECTOR_SIZE;
    return 0;
}

/* -------------------------------------------------------------------------
 * ide_eject_atapi – send an ATAPI START/STOP UNIT command with the eject
 * bit set, causing the CD-ROM tray to open (QEMU honours this; real drives
 * that have a software-controllable tray will also open).
 *
 * Returns  0 on success.
 * Returns -1 if drive_num is out of range or not present.
 * Returns -2 if the drive is not ATAPI.
 * Returns  1 on a protocol-level error (drive busy / error bit).
 * ---------------------------------------------------------------------- */
int ide_eject_atapi(uint8_t drive_num)
{
    if (drive_num >= IDE_MAX_DRIVES || !drives[drive_num].present)
        return -1;

    if (drives[drive_num].type != IDE_TYPE_ATAPI)
        return -2;

    uint8_t ch = drives[drive_num].channel;
    uint8_t dr = drives[drive_num].drive;

    /* Select drive and set byte-count limit to 0 (no data transfer). */
    ide_write(ch, ATA_REG_HDDEVSEL,
              (dr == 0) ? ATA_SEL_MASTER : ATA_SEL_SLAVE);
    ide_400ns_delay(ch);

    ide_write(ch, ATA_REG_FEATURES, 0x00);
    ide_write(ch, ATA_REG_LBA1,     0x00);   /* byte-count low  */
    ide_write(ch, ATA_REG_LBA2,     0x00);   /* byte-count high */

    /* Issue the PACKET command. */
    ide_write(ch, ATA_REG_COMMAND, ATA_CMD_PACKET);
    ide_400ns_delay(ch);

    /* Wait for DRQ - drive is ready to receive the 12-byte command packet. */
    if (ide_poll(ch, 1))
        return 1;

    /*
     * START/STOP UNIT packet:
     *   byte 0  = 0x1B (START STOP UNIT opcode)
     *   byte 4  = 0x02 (LoEj=1, Start=0 → eject / open tray)
     *   all other bytes = 0x00
     */
    uint8_t pkt[12] = {0};
    pkt[0] = 0x1Bu;   /* START STOP UNIT opcode */
    pkt[4] = 0x02u;   /* LoEj=1, Start=0 → open tray */

    for (int i = 0; i < 6; i++) {
        uint16_t w = (uint16_t)pkt[i * 2]
                   | ((uint16_t)pkt[i * 2 + 1] << 8);
        outw(channels[ch].base + ATA_REG_DATA, w);
    }

    /* Wait for completion (no data phase). */
    ide_poll(ch, 0);

    return 0;
}

/* -------------------------------------------------------------------------
 * PCI binding: find the bus-master IDE controller and arm DMA.
 * ---------------------------------------------------------------------- */
static int ide_dma_probe(pci_device_t *d)
{
    uint32_t io = pci_bar_io(d, 4);     /* BAR4 = bus-master I/O base */
    if (!io)
        return 1;                       /* no BMIDE here: don't claim */

    pci_enable_bus_master(d);
    s_bmide_base  = (uint16_t)io;
    s_dma_enabled = 1;
    s_dma_fails   = 0;

    Serial_WriteString("ide: BMIDE DMA at io ");
    Serial_WriteHex(s_bmide_base);
    Serial_WriteString("\n");
    return 0;
}

static const pci_driver_t ide_pci_driver = {
    .name        = "ide-dma",
    .class_code  = 0x01,    /* mass storage controller */
    .subclass    = 0x01,    /* IDE                     */
    .match_class = 1,
    .probe       = ide_dma_probe,
};

void ide_pci_register(void)
{
    pci_register_driver(&ide_pci_driver);
}
