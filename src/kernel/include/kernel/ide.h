#ifndef _KERNEL_IDE_H
#define _KERNEL_IDE_H

#include <kernel/types.h>

/* Maximum drives: 2 channels × 2 positions each. */
#define IDE_MAX_DRIVES 4

/* Drive type constants returned by ide_get_drive(). */
#define IDE_TYPE_NONE   0
#define IDE_TYPE_ATA    1
#define IDE_TYPE_ATAPI  2

/*
 * Descriptor for one detected drive.  Populated by ide_init().
 */
typedef struct {
    uint8_t  present;       /* 1 if a drive exists at this index          */
    uint8_t  channel;       /* 0 = primary, 1 = secondary                 */
    uint8_t  drive;         /* 0 = master,  1 = slave                     */
    uint8_t  type;          /* IDE_TYPE_ATA or IDE_TYPE_ATAPI             */
    uint16_t signature;     /* Device type word from IDENTIFY             */
    uint16_t capabilities;  /* Capabilities word from IDENTIFY            */
    uint32_t command_sets;  /* Supported command sets from IDENTIFY       */
    uint32_t size;          /* Size in 512-byte sectors                   */
    char     model[41];     /* Model string (NUL-terminated, trimmed)     */
} ide_drive_t;

/*
 * ide_init – scan both ATA channels, run IDENTIFY on every slot, and
 * populate internal drive descriptors.  Call once during kernel init.
 */
void ide_init(void);

/*
 * ide_read_sectors – read 'count' 512-byte sectors starting at 'lba'
 * from drive 'drive_num' into 'buf'.
 * Returns 0 on success, negative on invalid args, positive on ATA error.
 */
int ide_read_sectors(uint8_t drive_num, uint32_t lba, uint8_t count,
                     void *buf);

/*
 * ide_write_sectors – write 'count' 512-byte sectors from 'buf' to
 * drive 'drive_num' starting at 'lba'.
 * Returns 0 on success, negative on invalid args, positive on ATA error.
 */
int ide_write_sectors(uint8_t drive_num, uint32_t lba, uint8_t count,
                      const void *buf);

/*
 * ide_get_drive – return a pointer to the drive descriptor for
 * 'drive_num' (0-3), or NULL if out of range.
 */
const ide_drive_t *ide_get_drive(uint8_t drive_num);

/*
 * ide_read_atapi_sectors – read 'count' 2048-byte CD-ROM sectors starting
 * at 'lba' from an ATAPI drive into 'buf'.
 *
 * Returns 0 on success, -1 on invalid/absent drive, -2 if drive is not
 * ATAPI, or a positive value on a drive-level error.
 */
int ide_read_atapi_sectors(uint8_t drive_num, uint32_t lba,
                           uint16_t count, void *buf);

/*
 * ide_atapi_capacity – query an ATAPI medium's capacity via READ
 * CAPACITY(10).  Writes the sector count (last LBA + 1) and sector size
 * (typically 2048) to the out pointers.
 *
 * Returns  0 on success.
 * Returns -1 if drive_num is out of range or not present.
 * Returns -2 if the drive is not ATAPI.
 * Returns  positive on a protocol-level error.
 */
int ide_atapi_capacity(uint8_t drive_num, uint32_t *out_sectors,
                       uint32_t *out_sec_size);

/*
 * ide_eject_atapi – send an ATAPI START/STOP UNIT command that opens the
 * CD-ROM tray (eject).
 *
 * Returns  0 on success.
 * Returns -1 if drive_num is out of range or not present.
 * Returns -2 if the drive is not ATAPI.
 * Returns  1 on a protocol-level error.
 */
int ide_eject_atapi(uint8_t drive_num);

#endif /* _KERNEL_IDE_H */
