/*
 * ide.c — ATA PIO driver (28-bit LBA, polling mode).
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
#define ATA_CMD_CACHE_FLUSH 0xE7  /* Flush write cache           */
#define ATA_CMD_IDENTIFY    0xEC  /* Identify ATA device         */
#define ATA_CMD_PACKET      0xA0  /* ATAPI PACKET command        */
#define ATAPI_CMD_IDENTIFY  0xA1  /* Identify ATAPI device       */
#define ATAPI_CMD_READ12    0xA8  /* ATAPI READ(12) command      */

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

/* Read the alternate-status register — does NOT clear a pending IRQ. */
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

    uint8_t status;
    do {
        status = ide_read_altstatus(ch);
    } while (status & ATA_SR_BSY);

    if (status & ATA_SR_ERR)  return 1;
    if (status & ATA_SR_DF)   return 2;
    if (check_drq && !(status & ATA_SR_DRQ)) return 3;

    return 0;
}

/* -------------------------------------------------------------------------
 * ide_init
 * ---------------------------------------------------------------------- */
void ide_init(void)
{
    uint8_t identify_buf[512];

    /* Disable IRQs on both channels by setting nIEN in the control register. */
    outb(channels[0].ctrl, 0x02);
    outb(channels[1].ctrl, 0x02);

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
                /* Unknown / non-standard signature — skip slot. */
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
 * Public API
 * ---------------------------------------------------------------------- */

int ide_read_sectors(uint8_t drive_num, uint32_t lba, uint8_t count,
                     void *buf)
{
    return ide_access(0, drive_num, lba, count, buf);
}

int ide_write_sectors(uint8_t drive_num, uint32_t lba, uint8_t count,
                      const void *buf)
{
    return ide_access(1, drive_num, lba, count, (void *)buf);
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

    /* Wait for DRQ — device is ready to accept the 12-byte command packet. */
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

    /* Wait for DRQ — data is ready to be read. */
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

    for (uint16_t i = 0; i < count; i++) {
        int err = atapi_read_one(ch, dr, lba + (uint32_t)i, p);
        if (err)
            return err;
        p += ATAPI_CD_SECTOR_SIZE;
    }

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

    /* Wait for DRQ — drive is ready to receive the 12-byte command packet. */
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
