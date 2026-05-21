/*
 * ext2.c - ext2 filesystem driver (read + write).
 *
 * Single mounted volume.  Superblock + block-group descriptor table are
 * cached in the heap and flushed on metadata changes / unmount.  Files are
 * read and written via direct + single-indirect + double-indirect block
 * maps (triple-indirect is not implemented - far past the sizes Makar
 * volumes use).  Directories are flat linked lists of variable-length
 * entries, traversed and edited in place.
 *
 * On-disk layout reference: the classic ext2 spec (rev 0 + rev 1 dynamic).
 * The FILETYPE incompat feature is supported; any other incompat feature
 * causes ext2_mount to refuse (we will not corrupt a journalled / ext4
 * volume).  Sector I/O goes through ide_read_sectors/ide_write_sectors.
 */

#include <kernel/ext2.h>
#include <kernel/ide.h>
#include <kernel/heap.h>
#include <kernel/tty.h>
#include <string.h>
#include <stddef.h>

/* ---- on-disk structures ------------------------------------------------- */

#define EXT2_MAGIC          0xEF53
#define EXT2_ROOT_INO       2
#define EXT2_GOOD_OLD_REV   0
#define EXT2_DYNAMIC_REV    1
#define EXT2_GOOD_OLD_INODE_SIZE 128

/* incompat feature bits */
#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002

/* i_mode top nibble */
#define EXT2_S_IFMT   0xF000
#define EXT2_S_IFREG  0x8000
#define EXT2_S_IFDIR  0x4000

/* dir entry file_type (when FILETYPE feature present) */
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2

#define EXT2_MAX_BLOCK   4096u
#define SECTOR_SIZE      512u
#define EXT2_NAME_MAX    255

typedef struct __attribute__((packed)) {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    /* EXT2_DYNAMIC_REV extension */
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    /* remainder ignored */
} ext2_super_t;

typedef struct __attribute__((packed)) {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} ext2_gd_t;

typedef struct __attribute__((packed)) {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;        /* count of 512-byte sectors */
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} ext2_inode_t;

typedef struct __attribute__((packed)) {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    /* name[] follows */
} ext2_dirent_t;

/* ---- mount state -------------------------------------------------------- */

static int          s_mounted;
static uint8_t      s_drive;
static uint32_t     s_part_lba;
static uint32_t     s_block_size;
static uint32_t     s_spb;            /* sectors per block                  */
static uint32_t     s_inode_size;
static uint32_t     s_inodes_per_group;
static uint32_t     s_blocks_per_group;
static uint32_t     s_first_data_block;
static uint32_t     s_groups;
static int          s_has_filetype;

static ext2_super_t s_sb;
static ext2_gd_t   *s_gd;             /* group descriptor table (heap)      */
static int          s_sb_dirty;
static int          s_gd_dirty;

/* Scratch block buffers.  Cooperative-within-syscall use, same model as
 * the FAT32 driver. */
static uint8_t s_blk[EXT2_MAX_BLOCK];
static uint8_t s_blk2[EXT2_MAX_BLOCK];
static uint8_t s_ind[EXT2_MAX_BLOCK];   /* indirect-pointer block scratch   */

/* ---- block I/O ---------------------------------------------------------- */

static int e2_read_block(uint32_t blk, void *buf)
{
    if (s_spb > 255) return -1;
    uint32_t lba = s_part_lba + blk * s_spb;
    return ide_read_sectors(s_drive, lba, (uint8_t)s_spb, buf);
}

static int e2_write_block(uint32_t blk, const void *buf)
{
    if (s_spb > 255) return -1;
    uint32_t lba = s_part_lba + blk * s_spb;
    return ide_write_sectors(s_drive, lba, (uint8_t)s_spb, buf);
}

/* ---- superblock / group-descriptor flush -------------------------------- */

static void e2_flush_meta(void)
{
    if (s_sb_dirty) {
        /* Superblock lives at byte offset 1024.  With 1024-byte blocks it is
         * block 1; with larger blocks it shares block 0 at offset 1024.
         * Read-modify-write the containing block to be safe. */
        uint32_t sb_blk = (s_block_size == 1024) ? 1 : 0;
        uint32_t off    = (s_block_size == 1024) ? 0 : 1024;
        if (e2_read_block(sb_blk, s_blk) == 0) {
            memcpy(s_blk + off, &s_sb, sizeof(s_sb));
            e2_write_block(sb_blk, s_blk);
        }
        s_sb_dirty = 0;
    }
    if (s_gd_dirty) {
        uint32_t gd_blk = s_first_data_block + 1;
        uint32_t bytes  = s_groups * (uint32_t)sizeof(ext2_gd_t);
        uint32_t nblk   = (bytes + s_block_size - 1) / s_block_size;
        for (uint32_t i = 0; i < nblk; i++) {
            memset(s_blk, 0, s_block_size);
            uint32_t chunk = bytes - i * s_block_size;
            if (chunk > s_block_size) chunk = s_block_size;
            memcpy(s_blk, (uint8_t *)s_gd + i * s_block_size, chunk);
            e2_write_block(gd_blk + i, s_blk);
        }
        s_gd_dirty = 0;
    }
}

/* ---- inode read / write ------------------------------------------------- */

static int e2_read_inode(uint32_t ino, ext2_inode_t *out)
{
    if (ino == 0) return -1;
    uint32_t g     = (ino - 1) / s_inodes_per_group;
    uint32_t idx   = (ino - 1) % s_inodes_per_group;
    if (g >= s_groups) return -1;
    uint32_t byte  = idx * s_inode_size;
    uint32_t blk   = s_gd[g].bg_inode_table + byte / s_block_size;
    uint32_t boff  = byte % s_block_size;
    if (e2_read_block(blk, s_blk) != 0) return -1;
    memset(out, 0, sizeof(*out));
    uint32_t n = (s_inode_size < sizeof(*out)) ? s_inode_size : sizeof(*out);
    memcpy(out, s_blk + boff, n);
    return 0;
}

static int e2_write_inode(uint32_t ino, const ext2_inode_t *in)
{
    if (ino == 0) return -1;
    uint32_t g     = (ino - 1) / s_inodes_per_group;
    uint32_t idx   = (ino - 1) % s_inodes_per_group;
    if (g >= s_groups) return -1;
    uint32_t byte  = idx * s_inode_size;
    uint32_t blk   = s_gd[g].bg_inode_table + byte / s_block_size;
    uint32_t boff  = byte % s_block_size;
    if (e2_read_block(blk, s_blk) != 0) return -1;
    uint32_t n = (s_inode_size < sizeof(*in)) ? s_inode_size : sizeof(*in);
    memcpy(s_blk + boff, in, n);
    return e2_write_block(blk, s_blk);
}

/* ---- block bitmap allocation ------------------------------------------- */

static uint32_t e2_ptrs_per_block(void) { return s_block_size / 4; }

/* Allocate a fresh zeroed data block; returns block number or 0 on failure. */
static uint32_t e2_alloc_block(void)
{
    for (uint32_t g = 0; g < s_groups; g++) {
        if (s_gd[g].bg_free_blocks_count == 0) continue;
        if (e2_read_block(s_gd[g].bg_block_bitmap, s_blk2) != 0) continue;
        for (uint32_t i = 0; i < s_blocks_per_group; i++) {
            uint32_t byte = i >> 3, bit = i & 7;
            if (!(s_blk2[byte] & (1u << bit))) {
                s_blk2[byte] |= (uint8_t)(1u << bit);
                if (e2_write_block(s_gd[g].bg_block_bitmap, s_blk2) != 0)
                    return 0;
                s_gd[g].bg_free_blocks_count--;
                s_sb.s_free_blocks_count--;
                s_gd_dirty = s_sb_dirty = 1;
                uint32_t blk = g * s_blocks_per_group + s_first_data_block + i;
                memset(s_blk, 0, s_block_size);
                e2_write_block(blk, s_blk);
                return blk;
            }
        }
    }
    return 0;
}

static void e2_free_block(uint32_t blk)
{
    if (blk < s_first_data_block) return;
    uint32_t rel = blk - s_first_data_block;
    uint32_t g   = rel / s_blocks_per_group;
    uint32_t i   = rel % s_blocks_per_group;
    if (g >= s_groups) return;
    if (e2_read_block(s_gd[g].bg_block_bitmap, s_blk2) != 0) return;
    uint32_t byte = i >> 3, bit = i & 7;
    if (s_blk2[byte] & (1u << bit)) {
        s_blk2[byte] &= (uint8_t)~(1u << bit);
        e2_write_block(s_gd[g].bg_block_bitmap, s_blk2);
        s_gd[g].bg_free_blocks_count++;
        s_sb.s_free_blocks_count++;
        s_gd_dirty = s_sb_dirty = 1;
    }
}

/* Allocate an inode; returns inode number or 0.  is_dir bumps used_dirs. */
static uint32_t e2_alloc_inode(int is_dir)
{
    for (uint32_t g = 0; g < s_groups; g++) {
        if (s_gd[g].bg_free_inodes_count == 0) continue;
        if (e2_read_block(s_gd[g].bg_inode_bitmap, s_blk2) != 0) continue;
        for (uint32_t i = 0; i < s_inodes_per_group; i++) {
            uint32_t byte = i >> 3, bit = i & 7;
            if (!(s_blk2[byte] & (1u << bit))) {
                s_blk2[byte] |= (uint8_t)(1u << bit);
                if (e2_write_block(s_gd[g].bg_inode_bitmap, s_blk2) != 0)
                    return 0;
                s_gd[g].bg_free_inodes_count--;
                s_sb.s_free_inodes_count--;
                if (is_dir) s_gd[g].bg_used_dirs_count++;
                s_gd_dirty = s_sb_dirty = 1;
                return g * s_inodes_per_group + i + 1;
            }
        }
    }
    return 0;
}

static void e2_free_inode(uint32_t ino, int is_dir)
{
    if (ino == 0) return;
    uint32_t g = (ino - 1) / s_inodes_per_group;
    uint32_t i = (ino - 1) % s_inodes_per_group;
    if (g >= s_groups) return;
    if (e2_read_block(s_gd[g].bg_inode_bitmap, s_blk2) != 0) return;
    uint32_t byte = i >> 3, bit = i & 7;
    if (s_blk2[byte] & (1u << bit)) {
        s_blk2[byte] &= (uint8_t)~(1u << bit);
        e2_write_block(s_gd[g].bg_inode_bitmap, s_blk2);
        s_gd[g].bg_free_inodes_count++;
        s_sb.s_free_inodes_count++;
        if (is_dir && s_gd[g].bg_used_dirs_count > 0) s_gd[g].bg_used_dirs_count--;
        s_gd_dirty = s_sb_dirty = 1;
    }
}

/* ---- block map (logical -> physical) ----------------------------------- */

/* Map logical block 'lbn' of inode to a physical block (0 = hole/none).
 * Read-only; does not allocate. */
static uint32_t e2_bmap(const ext2_inode_t *in, uint32_t lbn)
{
    uint32_t ppb = e2_ptrs_per_block();
    if (lbn < 12) return in->i_block[lbn];
    lbn -= 12;
    if (lbn < ppb) {                                  /* single indirect */
        if (in->i_block[12] == 0) return 0;
        if (e2_read_block(in->i_block[12], s_ind) != 0) return 0;
        return ((uint32_t *)s_ind)[lbn];
    }
    lbn -= ppb;
    if (lbn < ppb * ppb) {                            /* double indirect */
        if (in->i_block[13] == 0) return 0;
        if (e2_read_block(in->i_block[13], s_ind) != 0) return 0;
        uint32_t mid = ((uint32_t *)s_ind)[lbn / ppb];
        if (mid == 0) return 0;
        if (e2_read_block(mid, s_ind) != 0) return 0;
        return ((uint32_t *)s_ind)[lbn % ppb];
    }
    return 0;   /* triple indirect unsupported */
}

/* Map logical block 'lbn', allocating direct/indirect blocks as needed.
 * Writes back the modified inode pointers via *in (caller flushes inode).
 * Returns physical block or 0 on failure. */
static uint32_t e2_bmap_alloc(ext2_inode_t *in, uint32_t lbn, int *grew)
{
    uint32_t ppb = e2_ptrs_per_block();
    *grew = 0;
    if (lbn < 12) {
        if (in->i_block[lbn] == 0) {
            uint32_t b = e2_alloc_block();
            if (!b) return 0;
            in->i_block[lbn] = b; *grew = 1;
        }
        return in->i_block[lbn];
    }
    lbn -= 12;
    if (lbn < ppb) {
        if (in->i_block[12] == 0) {
            uint32_t b = e2_alloc_block();
            if (!b) return 0;
            in->i_block[12] = b; in->i_blocks += s_block_size / SECTOR_SIZE;
        }
        if (e2_read_block(in->i_block[12], s_ind) != 0) return 0;
        uint32_t *p = (uint32_t *)s_ind;
        if (p[lbn] == 0) {
            uint32_t b = e2_alloc_block();
            if (!b) return 0;
            p[lbn] = b; *grew = 1;
            e2_write_block(in->i_block[12], s_ind);
        }
        return p[lbn];
    }
    lbn -= ppb;
    if (lbn < ppb * ppb) {
        if (in->i_block[13] == 0) {
            uint32_t b = e2_alloc_block();
            if (!b) return 0;
            in->i_block[13] = b; in->i_blocks += s_block_size / SECTOR_SIZE;
        }
        if (e2_read_block(in->i_block[13], s_ind) != 0) return 0;
        uint32_t *top = (uint32_t *)s_ind;
        uint32_t midi = lbn / ppb;
        uint32_t mid  = top[midi];
        if (mid == 0) {
            mid = e2_alloc_block();
            if (!mid) return 0;
            top[midi] = mid; in->i_blocks += s_block_size / SECTOR_SIZE;
            e2_write_block(in->i_block[13], s_ind);
        }
        if (e2_read_block(mid, s_ind) != 0) return 0;
        uint32_t *p = (uint32_t *)s_ind;
        if (p[lbn % ppb] == 0) {
            uint32_t b = e2_alloc_block();
            if (!b) return 0;
            p[lbn % ppb] = b; *grew = 1;
            e2_write_block(mid, s_ind);
        }
        return p[lbn % ppb];
    }
    return 0;
}

/* Free every data + metadata block owned by an inode (direct, single,
 * double indirect).  Leaves the inode struct's pointers stale (caller
 * resets size/blocks). */
static void e2_free_inode_blocks(ext2_inode_t *in)
{
    uint32_t ppb = e2_ptrs_per_block();
    for (int i = 0; i < 12; i++)
        if (in->i_block[i]) e2_free_block(in->i_block[i]);

    if (in->i_block[12]) {
        if (e2_read_block(in->i_block[12], s_ind) == 0) {
            uint32_t *p = (uint32_t *)s_ind;
            for (uint32_t i = 0; i < ppb; i++) if (p[i]) e2_free_block(p[i]);
        }
        e2_free_block(in->i_block[12]);
    }
    if (in->i_block[13]) {
        if (e2_read_block(in->i_block[13], s_ind) == 0) {
            uint32_t top[EXT2_MAX_BLOCK / 4];
            memcpy(top, s_ind, s_block_size);
            uint32_t n = ppb;
            for (uint32_t i = 0; i < n; i++) {
                if (!top[i]) continue;
                if (e2_read_block(top[i], s_ind) == 0) {
                    uint32_t *p = (uint32_t *)s_ind;
                    for (uint32_t j = 0; j < ppb; j++) if (p[j]) e2_free_block(p[j]);
                }
                e2_free_block(top[i]);
            }
        }
        e2_free_block(in->i_block[13]);
    }
    for (int i = 0; i < 15; i++) in->i_block[i] = 0;
    in->i_blocks = 0;
}

/* ---- path resolution ---------------------------------------------------- */

/* Find entry 'name' (length nlen) in directory inode 'dir'.  On success
 * returns the child inode number and (optionally) its file_type. */
static uint32_t e2_dir_lookup(const ext2_inode_t *dir, const char *name,
                              uint32_t nlen, uint8_t *out_ft)
{
    uint32_t nblocks = (dir->i_size + s_block_size - 1) / s_block_size;
    for (uint32_t lb = 0; lb < nblocks; lb++) {
        uint32_t pb = e2_bmap(dir, lb);
        if (!pb) continue;
        if (e2_read_block(pb, s_blk) != 0) continue;
        uint32_t off = 0;
        while (off + 8 <= s_block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(s_blk + off);
            if (de->rec_len < 8) break;
            if (de->inode != 0 && de->name_len == nlen &&
                memcmp(s_blk + off + 8, name, nlen) == 0) {
                if (out_ft) *out_ft = de->file_type;
                return de->inode;
            }
            off += de->rec_len;
        }
    }
    return 0;
}

/* Resolve an absolute volume path to its inode number.  '/' -> root.
 * Returns 0 if any component is missing.  Fills *out_inode if non-NULL. */
static uint32_t e2_resolve(const char *path, ext2_inode_t *out_inode)
{
    ext2_inode_t cur;
    if (e2_read_inode(EXT2_ROOT_INO, &cur) != 0) return 0;
    uint32_t cur_ino = EXT2_ROOT_INO;

    const char *p = path;
    while (*p == '/') p++;
    while (*p) {
        const char *seg = p;
        while (*p && *p != '/') p++;
        uint32_t nlen = (uint32_t)(p - seg);
        while (*p == '/') p++;
        if (nlen == 0) continue;
        if (nlen == 1 && seg[0] == '.') continue;
        if ((cur.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return 0;
        uint32_t child = e2_dir_lookup(&cur, seg, nlen, NULL);
        if (!child) return 0;
        if (e2_read_inode(child, &cur) != 0) return 0;
        cur_ino = child;
    }
    if (out_inode) *out_inode = cur;
    return cur_ino;
}

/* Split 'path' into parent dir path + leaf name.  Returns leaf length;
 * parent_out gets the parent path (always absolute, "/" for root). */
static uint32_t e2_split(const char *path, char *parent_out, uint32_t psz,
                         const char **leaf_out)
{
    int len = (int)strlen(path);
    while (len > 0 && path[len - 1] == '/') len--;   /* trim trailing '/' */
    int slash = -1;
    for (int i = len - 1; i >= 0; i--) if (path[i] == '/') { slash = i; break; }
    const char *leaf = (slash >= 0) ? path + slash + 1 : path;
    uint32_t plen = (slash <= 0) ? 1u : (uint32_t)slash;
    if (plen >= psz) plen = psz - 1;
    if (slash <= 0) { parent_out[0] = '/'; parent_out[1] = '\0'; }
    else { memcpy(parent_out, path, plen); parent_out[plen] = '\0'; }
    *leaf_out = leaf;
    return (uint32_t)(len - (int)(leaf - path));
}

/* ---- directory entry insert / remove ------------------------------------ */

#define EXT2_DIR_ALIGN(n) (((n) + 3u) & ~3u)

/* Add an entry (child_ino, name) into directory dir_ino.  Allocates a new
 * dir block if no slack fits.  Returns 0 on success. */
static int e2_dir_add(uint32_t dir_ino, ext2_inode_t *dir,
                      const char *name, uint32_t nlen,
                      uint32_t child_ino, uint8_t ftype)
{
    uint32_t need = EXT2_DIR_ALIGN(8 + nlen);
    uint32_t nblocks = (dir->i_size + s_block_size - 1) / s_block_size;

    for (uint32_t lb = 0; lb < nblocks; lb++) {
        uint32_t pb = e2_bmap(dir, lb);
        if (!pb) continue;
        if (e2_read_block(pb, s_blk) != 0) continue;
        uint32_t off = 0;
        while (off + 8 <= s_block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(s_blk + off);
            if (de->rec_len < 8 || off + de->rec_len > s_block_size) break;
            uint32_t used = (de->inode == 0) ? 0 : EXT2_DIR_ALIGN(8 + de->name_len);
            uint32_t slack = de->rec_len - used;
            if (slack >= need) {
                uint16_t old_rec = de->rec_len;
                ext2_dirent_t *ne;
                if (de->inode == 0) {
                    ne = de;                       /* reuse empty slot      */
                    ne->rec_len = old_rec;
                } else {
                    de->rec_len = (uint16_t)used;  /* shrink predecessor    */
                    ne = (ext2_dirent_t *)(s_blk + off + used);
                    ne->rec_len = (uint16_t)(old_rec - used);
                }
                ne->inode     = child_ino;
                ne->name_len  = (uint8_t)nlen;
                ne->file_type = s_has_filetype ? ftype : 0;
                memcpy(s_blk + ((uint8_t *)ne - s_blk) + 8, name, nlen);
                return e2_write_block(pb, s_blk);
            }
            off += de->rec_len;
        }
    }

    /* No slack: append a new directory block. */
    int grew;
    uint32_t pb = e2_bmap_alloc(dir, nblocks, &grew);
    if (!pb) return -1;
    dir->i_size += s_block_size;
    dir->i_blocks += s_block_size / SECTOR_SIZE;
    memset(s_blk, 0, s_block_size);
    ext2_dirent_t *de = (ext2_dirent_t *)s_blk;
    de->inode     = child_ino;
    de->rec_len   = (uint16_t)s_block_size;
    de->name_len  = (uint8_t)nlen;
    de->file_type = s_has_filetype ? ftype : 0;
    memcpy(s_blk + 8, name, nlen);
    if (e2_write_block(pb, s_blk) != 0) return -1;
    return e2_write_inode(dir_ino, dir);
}

/* Remove entry 'name' from directory dir.  Merges its space into the
 * predecessor entry (or zeroes the inode field for the first entry). */
static int e2_dir_remove(const ext2_inode_t *dir, const char *name, uint32_t nlen)
{
    uint32_t nblocks = (dir->i_size + s_block_size - 1) / s_block_size;
    for (uint32_t lb = 0; lb < nblocks; lb++) {
        uint32_t pb = e2_bmap(dir, lb);
        if (!pb) continue;
        if (e2_read_block(pb, s_blk) != 0) continue;
        uint32_t off = 0, prev = 0;
        int have_prev = 0;
        while (off + 8 <= s_block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(s_blk + off);
            if (de->rec_len < 8) break;
            if (de->inode != 0 && de->name_len == nlen &&
                memcmp(s_blk + off + 8, name, nlen) == 0) {
                if (have_prev) {
                    ext2_dirent_t *pd = (ext2_dirent_t *)(s_blk + prev);
                    pd->rec_len = (uint16_t)(pd->rec_len + de->rec_len);
                } else {
                    de->inode = 0;   /* first entry: just mark unused */
                }
                return e2_write_block(pb, s_blk);
            }
            prev = off; have_prev = 1;
            off += de->rec_len;
        }
    }
    return -1;
}

/* Count real entries (excluding . and ..) in a directory. */
static int e2_dir_is_empty(const ext2_inode_t *dir)
{
    uint32_t nblocks = (dir->i_size + s_block_size - 1) / s_block_size;
    for (uint32_t lb = 0; lb < nblocks; lb++) {
        uint32_t pb = e2_bmap(dir, lb);
        if (!pb) continue;
        if (e2_read_block(pb, s_blk) != 0) continue;
        uint32_t off = 0;
        while (off + 8 <= s_block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(s_blk + off);
            if (de->rec_len < 8) break;
            if (de->inode != 0) {
                if (!((de->name_len == 1 && s_blk[off + 8] == '.') ||
                      (de->name_len == 2 && s_blk[off + 8] == '.' &&
                       s_blk[off + 9] == '.')))
                    return 0;
            }
            off += de->rec_len;
        }
    }
    return 1;
}

/* ---- mount -------------------------------------------------------------- */

static int e2_load_super(uint8_t drive, uint32_t part_lba, ext2_super_t *sb)
{
    /* Superblock is always at byte offset 1024 = LBA part_lba+2. */
    uint8_t sec[SECTOR_SIZE * 2];
    if (ide_read_sectors(drive, part_lba + 2, 2, sec) != 0) return -1;
    memcpy(sb, sec, sizeof(*sb));
    return (sb->s_magic == EXT2_MAGIC) ? 0 : -2;
}

int ext2_probe(uint8_t drive, uint32_t part_lba)
{
    ext2_super_t sb;
    return (e2_load_super(drive, part_lba, &sb) == 0) ? 1 : 0;
}

int ext2_mount(uint8_t drive, uint32_t part_lba)
{
    if (s_mounted) ext2_unmount();

    int r = e2_load_super(drive, part_lba, &s_sb);
    if (r != 0) return r;     /* -2 = not ext2 */

    uint32_t incompat = (s_sb.s_rev_level >= EXT2_DYNAMIC_REV)
                            ? s_sb.s_feature_incompat : 0;
    s_has_filetype = (incompat & EXT2_FEATURE_INCOMPAT_FILETYPE) ? 1 : 0;
    if (incompat & ~(uint32_t)EXT2_FEATURE_INCOMPAT_FILETYPE)
        return -3;            /* journal / extents / meta_bg etc. */

    s_drive    = drive;
    s_part_lba = part_lba;
    s_block_size = 1024u << s_sb.s_log_block_size;
    if (s_block_size == 0 || s_block_size > EXT2_MAX_BLOCK) return -2;
    s_spb = s_block_size / SECTOR_SIZE;
    s_inode_size = (s_sb.s_rev_level >= EXT2_DYNAMIC_REV && s_sb.s_inode_size)
                       ? s_sb.s_inode_size : EXT2_GOOD_OLD_INODE_SIZE;
    s_inodes_per_group = s_sb.s_inodes_per_group;
    s_blocks_per_group = s_sb.s_blocks_per_group;
    s_first_data_block = s_sb.s_first_data_block;
    if (s_inodes_per_group == 0 || s_blocks_per_group == 0) return -2;

    s_groups = (s_sb.s_blocks_count - s_first_data_block + s_blocks_per_group - 1)
               / s_blocks_per_group;

    /* Load the group-descriptor table (starts at first_data_block + 1). */
    uint32_t gd_bytes = s_groups * (uint32_t)sizeof(ext2_gd_t);
    s_gd = (ext2_gd_t *)kmalloc(gd_bytes);
    if (!s_gd) return -1;
    uint32_t gd_blk = s_first_data_block + 1;
    uint32_t nblk   = (gd_bytes + s_block_size - 1) / s_block_size;
    for (uint32_t i = 0; i < nblk; i++) {
        if (e2_read_block(gd_blk + i, s_blk) != 0) { kfree(s_gd); s_gd = NULL; return -1; }
        uint32_t chunk = gd_bytes - i * s_block_size;
        if (chunk > s_block_size) chunk = s_block_size;
        memcpy((uint8_t *)s_gd + i * s_block_size, s_blk, chunk);
    }

    s_sb_dirty = s_gd_dirty = 0;
    s_mounted  = 1;
    return 0;
}

void ext2_unmount(void)
{
    if (!s_mounted) return;
    e2_flush_meta();
    if (s_gd) { kfree(s_gd); s_gd = NULL; }
    s_mounted = 0;
}

int ext2_mounted(void) { return s_mounted; }

/* ---- read API ----------------------------------------------------------- */

int ext2_file_exists(const char *path)
{
    if (!s_mounted) return 0;
    return e2_resolve(path, NULL) != 0;
}

int ext2_read_file(const char *path, void *buf, uint32_t bufsz, uint32_t *out_sz)
{
    if (!s_mounted) return -1;
    ext2_inode_t in;
    uint32_t ino = e2_resolve(path, &in);
    if (!ino) return -1;
    if ((in.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG) return -1;

    uint32_t size = in.i_size;
    if (size > bufsz) size = bufsz;
    uint32_t done = 0;
    uint8_t *out = (uint8_t *)buf;
    uint32_t lb = 0;
    while (done < size) {
        uint32_t pb = e2_bmap(&in, lb);
        uint32_t chunk = s_block_size;
        if (chunk > size - done) chunk = size - done;
        if (pb == 0) {
            memset(out + done, 0, chunk);   /* sparse hole */
        } else {
            if (e2_read_block(pb, s_blk) != 0) break;
            memcpy(out + done, s_blk, chunk);
        }
        done += chunk;
        lb++;
    }
    if (out_sz) *out_sz = done;
    return 0;
}

int ext2_ls(const char *path)
{
    if (!s_mounted) { t_writestring("ext2: not mounted\n"); return -1; }
    ext2_inode_t dir;
    uint32_t ino = e2_resolve(path && *path ? path : "/", &dir);
    if (!ino) { t_writestring("ls: path not found\n"); return -1; }
    if ((dir.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) {
        t_writestring("ls: not a directory\n");
        return -1;
    }
    uint32_t nblocks = (dir.i_size + s_block_size - 1) / s_block_size;
    for (uint32_t lb = 0; lb < nblocks; lb++) {
        uint32_t pb = e2_bmap(&dir, lb);
        if (!pb) continue;
        if (e2_read_block(pb, s_blk) != 0) continue;
        uint32_t off = 0;
        while (off + 8 <= s_block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(s_blk + off);
            if (de->rec_len < 8) break;
            if (de->inode != 0) {
                int is_dir;
                if (s_has_filetype) {
                    is_dir = (de->file_type == EXT2_FT_DIR);
                } else {
                    ext2_inode_t ti;
                    is_dir = (e2_read_inode(de->inode, &ti) == 0 &&
                              (ti.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR);
                }
                if (is_dir) t_putchar('[');
                for (int i = 0; i < de->name_len; i++)
                    t_putchar((char)s_blk[off + 8 + i]);
                if (is_dir) t_putchar(']');
                t_putchar('\n');
            }
            off += de->rec_len;
        }
    }
    return 0;
}

int ext2_cd(const char *path)
{
    if (!s_mounted) return -1;
    ext2_inode_t in;
    uint32_t ino = e2_resolve(path && *path ? path : "/", &in);
    if (!ino) return -1;
    return ((in.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) ? 0 : -1;
}

int ext2_complete(const char *dir_path, const char *prefix,
                  fat32_complete_cb_t cb, void *ctx)
{
    (void)prefix;
    if (!s_mounted || !cb) return -1;
    ext2_inode_t dir;
    uint32_t ino = e2_resolve(dir_path && *dir_path ? dir_path : "/", &dir);
    if (!ino || (dir.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;
    uint32_t nblocks = (dir.i_size + s_block_size - 1) / s_block_size;
    for (uint32_t lb = 0; lb < nblocks; lb++) {
        uint32_t pb = e2_bmap(&dir, lb);
        if (!pb) continue;
        if (e2_read_block(pb, s_blk) != 0) continue;
        uint32_t off = 0;
        while (off + 8 <= s_block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(s_blk + off);
            if (de->rec_len < 8) break;
            if (de->inode != 0 && de->name_len > 0) {
                char nm[EXT2_NAME_MAX + 1];
                uint32_t n = de->name_len;
                if (n > EXT2_NAME_MAX) n = EXT2_NAME_MAX;
                memcpy(nm, s_blk + off + 8, n);
                nm[n] = '\0';
                int is_dir;
                if (s_has_filetype) is_dir = (de->file_type == EXT2_FT_DIR);
                else {
                    ext2_inode_t ti;
                    is_dir = (e2_read_inode(de->inode, &ti) == 0 &&
                              (ti.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR);
                }
                if (!(n == 1 && nm[0] == '.') &&
                    !(n == 2 && nm[0] == '.' && nm[1] == '.'))
                    cb(nm, is_dir, ctx);
            }
            off += de->rec_len;
        }
    }
    return 0;
}

/* ---- write API ---------------------------------------------------------- */

int ext2_write_file(const char *path, const void *buf, uint32_t size)
{
    if (!s_mounted) return -1;

    char parent[256];
    const char *leaf;
    uint32_t nlen = e2_split(path, parent, sizeof(parent), &leaf);
    if (nlen == 0 || nlen > EXT2_NAME_MAX) return -1;

    ext2_inode_t pdir;
    uint32_t pino = e2_resolve(parent, &pdir);
    if (!pino || (pdir.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;

    /* Find or create the file inode. */
    ext2_inode_t in;
    uint32_t ino = e2_dir_lookup(&pdir, leaf, nlen, NULL);
    if (ino) {
        if (e2_read_inode(ino, &in) != 0) return -1;
        if ((in.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG) return -1;
        e2_free_inode_blocks(&in);     /* truncate to zero before rewrite   */
        in.i_size = 0;
    } else {
        ino = e2_alloc_inode(0);
        if (!ino) return -1;
        memset(&in, 0, sizeof(in));
        in.i_mode = EXT2_S_IFREG | 0644;
        in.i_links_count = 1;
        if (e2_dir_add(pino, &pdir, leaf, nlen, ino, EXT2_FT_REG_FILE) != 0) {
            e2_free_inode(ino, 0);
            return -1;
        }
    }

    /* Write the data, block by block. */
    uint32_t done = 0, lb = 0;
    const uint8_t *src = (const uint8_t *)buf;
    while (done < size) {
        int grew;
        uint32_t pb = e2_bmap_alloc(&in, lb, &grew);
        if (!pb) { /* out of space: keep what we wrote */ break; }
        uint32_t chunk = s_block_size;
        if (chunk > size - done) chunk = size - done;
        memset(s_blk, 0, s_block_size);
        memcpy(s_blk, src + done, chunk);
        if (e2_write_block(pb, s_blk) != 0) break;
        if (grew) in.i_blocks += s_block_size / SECTOR_SIZE;
        done += chunk;
        lb++;
    }
    in.i_size = done;
    e2_write_inode(ino, &in);
    e2_flush_meta();
    return (done == size) ? 0 : -2;
}

int ext2_delete_file(const char *path)
{
    if (!s_mounted) return -1;
    char parent[256];
    const char *leaf;
    uint32_t nlen = e2_split(path, parent, sizeof(parent), &leaf);
    if (nlen == 0) return -1;

    ext2_inode_t pdir;
    uint32_t pino = e2_resolve(parent, &pdir);
    if (!pino) return -1;
    uint8_t ft;
    uint32_t ino = e2_dir_lookup(&pdir, leaf, nlen, &ft);
    if (!ino) return -1;

    ext2_inode_t in;
    if (e2_read_inode(ino, &in) != 0) return -1;
    if ((in.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) return -1;  /* use rmdir */

    if (e2_dir_remove(&pdir, leaf, nlen) != 0) return -1;
    e2_free_inode_blocks(&in);
    in.i_links_count = 0;
    in.i_dtime = 0;
    e2_write_inode(ino, &in);
    e2_free_inode(ino, 0);
    e2_flush_meta();
    return 0;
}

int ext2_mkdir(const char *path)
{
    if (!s_mounted) return -1;
    char parent[256];
    const char *leaf;
    uint32_t nlen = e2_split(path, parent, sizeof(parent), &leaf);
    if (nlen == 0 || nlen > EXT2_NAME_MAX) return -1;

    ext2_inode_t pdir;
    uint32_t pino = e2_resolve(parent, &pdir);
    if (!pino || (pdir.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;
    if (e2_dir_lookup(&pdir, leaf, nlen, NULL)) return -1;   /* exists */

    uint32_t ino = e2_alloc_inode(1);
    if (!ino) return -1;
    uint32_t blk = e2_alloc_block();
    if (!blk) { e2_free_inode(ino, 1); return -1; }

    /* Build the "." and ".." entries in the new block. */
    memset(s_blk, 0, s_block_size);
    ext2_dirent_t *dot = (ext2_dirent_t *)s_blk;
    dot->inode = ino; dot->name_len = 1; dot->rec_len = 12;
    dot->file_type = s_has_filetype ? EXT2_FT_DIR : 0; s_blk[8] = '.';
    ext2_dirent_t *dd = (ext2_dirent_t *)(s_blk + 12);
    dd->inode = pino; dd->name_len = 2;
    dd->rec_len = (uint16_t)(s_block_size - 12);
    dd->file_type = s_has_filetype ? EXT2_FT_DIR : 0;
    s_blk[20] = '.'; s_blk[21] = '.';
    if (e2_write_block(blk, s_blk) != 0) { e2_free_block(blk); e2_free_inode(ino, 1); return -1; }

    ext2_inode_t in;
    memset(&in, 0, sizeof(in));
    in.i_mode = EXT2_S_IFDIR | 0755;
    in.i_size = s_block_size;
    in.i_links_count = 2;                   /* self + "." */
    in.i_blocks = s_block_size / SECTOR_SIZE;
    in.i_block[0] = blk;
    e2_write_inode(ino, &in);

    if (e2_dir_add(pino, &pdir, leaf, nlen, ino, EXT2_FT_DIR) != 0) {
        e2_free_block(blk); e2_free_inode(ino, 1);
        return -1;
    }
    pdir.i_links_count++;                    /* parent gains a ".." child */
    e2_write_inode(pino, &pdir);
    e2_flush_meta();
    return 0;
}

int ext2_delete_dir(const char *path)
{
    if (!s_mounted) return -1;
    char parent[256];
    const char *leaf;
    uint32_t nlen = e2_split(path, parent, sizeof(parent), &leaf);
    if (nlen == 0) return -1;

    ext2_inode_t pdir;
    uint32_t pino = e2_resolve(parent, &pdir);
    if (!pino) return -1;
    uint32_t ino = e2_dir_lookup(&pdir, leaf, nlen, NULL);
    if (!ino) return -1;

    ext2_inode_t in;
    if (e2_read_inode(ino, &in) != 0) return -1;
    if ((in.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;
    if (!e2_dir_is_empty(&in)) return -5;

    if (e2_dir_remove(&pdir, leaf, nlen) != 0) return -1;
    e2_free_inode_blocks(&in);
    in.i_links_count = 0;
    e2_write_inode(ino, &in);
    e2_free_inode(ino, 1);
    if (pdir.i_links_count > 0) pdir.i_links_count--;   /* lost ".." */
    e2_write_inode(pino, &pdir);
    e2_flush_meta();
    return 0;
}

/* Rename within the volume by relinking the same inode under a new name.
 * Works for files and directories (directory ".." is left pointing at the
 * old parent only when moving across directories - we update it). */
static int e2_rename_common(const char *oldp, const char *newp)
{
    if (!s_mounted) return -1;

    char oparent[256], nparent[256];
    const char *oleaf, *nleaf;
    uint32_t onlen = e2_split(oldp, oparent, sizeof(oparent), &oleaf);
    uint32_t nnlen = e2_split(newp, nparent, sizeof(nparent), &nleaf);
    if (onlen == 0 || nnlen == 0 || nnlen > EXT2_NAME_MAX) return -1;

    ext2_inode_t opd, npd;
    uint32_t opino = e2_resolve(oparent, &opd);
    uint32_t npino = e2_resolve(nparent, &npd);
    if (!opino || !npino) return -1;

    uint8_t ft;
    uint32_t ino = e2_dir_lookup(&opd, oleaf, onlen, &ft);
    if (!ino) return -1;
    if (e2_dir_lookup(&npd, nleaf, nnlen, NULL)) return -6;   /* target exists */

    ext2_inode_t in;
    if (e2_read_inode(ino, &in) != 0) return -1;
    uint8_t ftype = ((in.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR)
                        ? EXT2_FT_DIR : EXT2_FT_REG_FILE;

    if (e2_dir_add(npino, &npd, nleaf, nnlen, ino, ftype) != 0) return -1;
    /* opd may be stale if it equals npd and e2_dir_add grew it; re-read. */
    if (e2_read_inode(opino, &opd) != 0) return -1;
    if (e2_dir_remove(&opd, oleaf, onlen) != 0) return -1;

    /* Moving a directory to a different parent: fix its ".." and link counts. */
    if (ftype == EXT2_FT_DIR && opino != npino) {
        uint32_t pb = e2_bmap(&in, 0);
        if (pb && e2_read_block(pb, s_blk) == 0) {
            ext2_dirent_t *dd = (ext2_dirent_t *)(s_blk + 12);  /* ".." slot */
            if (dd->name_len == 2 && s_blk[20] == '.' && s_blk[21] == '.') {
                dd->inode = npino;
                e2_write_block(pb, s_blk);
            }
        }
        if (opd.i_links_count > 0) opd.i_links_count--;
        npd.i_links_count++;
        e2_read_inode(npino, &npd);   /* refresh after add */
        npd.i_links_count++;
        e2_write_inode(npino, &npd);
        e2_write_inode(opino, &opd);
    }
    e2_flush_meta();
    return 0;
}

int ext2_rename_file(const char *old_path, const char *new_path)
{
    return e2_rename_common(old_path, new_path);
}

int ext2_rename_dir(const char *old_path, const char *new_path)
{
    return e2_rename_common(old_path, new_path);
}

/* ---- mkfs --------------------------------------------------------------- */

/* mkfs writes raw blocks before any volume is mounted, so it uses local
 * block I/O rather than the mount-state helpers above.  Fixed 1 KiB blocks,
 * rev 1, FILETYPE feature, super+GDT backup in every group (no sparse_super). */

#define MKFS_BS    1024u
#define MKFS_SPB   (MKFS_BS / SECTOR_SIZE)

static uint8_t  mk_drive;
static uint32_t mk_lba;

static int mk_wb(uint32_t blk, const void *buf)
{
    return ide_write_sectors(mk_drive, mk_lba + blk * MKFS_SPB, MKFS_SPB, buf);
}

static uint32_t count_zero_bits(const uint8_t *bm, uint32_t nbits)
{
    uint32_t z = 0;
    for (uint32_t i = 0; i < nbits; i++)
        if (!(bm[i >> 3] & (1u << (i & 7)))) z++;
    return z;
}

static void set_bit(uint8_t *bm, uint32_t i)  { bm[i >> 3] |= (uint8_t)(1u << (i & 7)); }

int ext2_mkfs(uint8_t drive, uint32_t part_lba, uint32_t part_sectors)
{
    mk_drive = drive;
    mk_lba   = part_lba;

    const uint32_t bs   = MKFS_BS;
    const uint32_t isize = EXT2_GOOD_OLD_INODE_SIZE;   /* 128 */
    uint32_t total = part_sectors / MKFS_SPB;          /* total blocks */
    if (total < 64) return -6;

    uint32_t first_data = 1;                            /* 1 KiB blocks */
    uint32_t bpg = 8u * bs;                             /* 8192 blocks/group */
    uint32_t ipg = (bpg * bs) / 16384u;                 /* ~1 inode / 16 KiB */
    ipg = (ipg + 7u) & ~7u;
    if (ipg < 16u) ipg = 16u;
    if (ipg > bs * 8u) ipg = bs * 8u;
    uint32_t groups = (total - first_data + bpg - 1) / bpg;
    uint32_t gdt_blocks = (groups * (uint32_t)sizeof(ext2_gd_t) + bs - 1) / bs;
    uint32_t itb = (ipg * isize + bs - 1) / bs;         /* inode-table blocks */
    uint32_t overhead = 1u + gdt_blocks + 1u + 1u + itb;
    if (overhead + 4u >= bpg) return -6;                /* group too cramped */

    uint8_t  blk[MKFS_BS];
    ext2_gd_t *gd = (ext2_gd_t *)kmalloc(groups * sizeof(ext2_gd_t));
    if (!gd) return -2;
    memset(gd, 0, groups * sizeof(ext2_gd_t));

    /* group0 data blocks for root dir and lost+found */
    uint32_t root_block = first_data + overhead;
    uint32_t lf_block   = root_block + 1;

    /* ---- per-group bitmaps + inode tables ---- */
    for (uint32_t g = 0; g < groups; g++) {
        uint32_t fb = first_data + g * bpg;
        uint32_t bbm = fb + 1 + gdt_blocks;
        uint32_t ibm = bbm + 1;
        uint32_t itab = ibm + 1;
        gd[g].bg_block_bitmap = bbm;
        gd[g].bg_inode_bitmap = ibm;
        gd[g].bg_inode_table  = itab;

        /* how many real blocks this group owns */
        uint32_t remaining = total - first_data - g * bpg;
        uint32_t gblocks = (remaining < bpg) ? remaining : bpg;

        /* block bitmap */
        memset(blk, 0, bs);
        for (uint32_t i = 0; i < overhead; i++) set_bit(blk, i);     /* metadata */
        if (g == 0) { set_bit(blk, overhead); set_bit(blk, overhead + 1); } /* root, l+f */
        for (uint32_t i = gblocks; i < bpg; i++) set_bit(blk, i);    /* past EOF */
        gd[g].bg_free_blocks_count = (uint16_t)count_zero_bits(blk, bpg);
        if (mk_wb(bbm, blk) != 0) { kfree(gd); return -2; }

        /* inode bitmap */
        memset(blk, 0, bs);
        if (g == 0) for (uint32_t i = 0; i < 11u; i++) set_bit(blk, i); /* ino 1..11 */
        for (uint32_t i = ipg; i < bs * 8u; i++) set_bit(blk, i);       /* padding */
        gd[g].bg_free_inodes_count = (uint16_t)count_zero_bits(blk, ipg);
        if (mk_wb(ibm, blk) != 0) { kfree(gd); return -2; }

        /* zero the inode table */
        memset(blk, 0, bs);
        for (uint32_t i = 0; i < itb; i++)
            if (mk_wb(itab + i, blk) != 0) { kfree(gd); return -2; }
    }
    gd[0].bg_used_dirs_count = 2;   /* root + lost+found */

    /* ---- root + lost+found inodes (group0 table) ---- */
    {
        uint32_t itab = gd[0].bg_inode_table;
        /* table block 0 holds inodes 1..8; root = inode 2 at offset 128 */
        memset(blk, 0, bs);
        ext2_inode_t root;
        memset(&root, 0, sizeof(root));
        root.i_mode = EXT2_S_IFDIR | 0755;
        root.i_size = bs;
        root.i_links_count = 3;                 /* "."  ".."  l+f's ".." */
        root.i_blocks = bs / SECTOR_SIZE;
        root.i_block[0] = root_block;
        memcpy(blk + 1 * isize, &root, sizeof(root));
        if (mk_wb(itab + 0, blk) != 0) { kfree(gd); return -2; }

        /* table block 1 holds inodes 9..16; lost+found = inode 11 at offset 256 */
        memset(blk, 0, bs);
        ext2_inode_t lf;
        memset(&lf, 0, sizeof(lf));
        lf.i_mode = EXT2_S_IFDIR | 0700;
        lf.i_size = bs;
        lf.i_links_count = 2;
        lf.i_blocks = bs / SECTOR_SIZE;
        lf.i_block[0] = lf_block;
        memcpy(blk + 2 * isize, &lf, sizeof(lf));
        if (mk_wb(itab + 1, blk) != 0) { kfree(gd); return -2; }
    }

    /* ---- root + lost+found directory blocks ---- */
    {
        /* root: ".", "..", "lost+found" */
        memset(blk, 0, bs);
        ext2_dirent_t *d = (ext2_dirent_t *)blk;
        d->inode = EXT2_ROOT_INO; d->rec_len = 12; d->name_len = 1;
        d->file_type = EXT2_FT_DIR; blk[8] = '.';
        d = (ext2_dirent_t *)(blk + 12);
        d->inode = EXT2_ROOT_INO; d->rec_len = 12; d->name_len = 2;
        d->file_type = EXT2_FT_DIR; blk[20] = '.'; blk[21] = '.';
        d = (ext2_dirent_t *)(blk + 24);
        d->inode = 11; d->rec_len = (uint16_t)(bs - 24); d->name_len = 10;
        d->file_type = EXT2_FT_DIR; memcpy(blk + 32, "lost+found", 10);
        if (mk_wb(root_block, blk) != 0) { kfree(gd); return -2; }

        /* lost+found: ".", ".." */
        memset(blk, 0, bs);
        d = (ext2_dirent_t *)blk;
        d->inode = 11; d->rec_len = 12; d->name_len = 1;
        d->file_type = EXT2_FT_DIR; blk[8] = '.';
        d = (ext2_dirent_t *)(blk + 12);
        d->inode = EXT2_ROOT_INO; d->rec_len = (uint16_t)(bs - 12); d->name_len = 2;
        d->file_type = EXT2_FT_DIR; blk[20] = '.'; blk[21] = '.';
        if (mk_wb(lf_block, blk) != 0) { kfree(gd); return -2; }
    }

    /* ---- superblock ---- */
    ext2_super_t sb;
    memset(&sb, 0, sizeof(sb));
    uint32_t free_blocks = 0, free_inodes = 0;
    for (uint32_t g = 0; g < groups; g++) {
        free_blocks += gd[g].bg_free_blocks_count;
        free_inodes += gd[g].bg_free_inodes_count;
    }
    sb.s_inodes_count      = ipg * groups;
    sb.s_blocks_count      = total;
    sb.s_r_blocks_count    = 0;
    sb.s_free_blocks_count = free_blocks;
    sb.s_free_inodes_count = free_inodes;
    sb.s_first_data_block  = first_data;
    sb.s_log_block_size    = 0;          /* 1024 << 0 */
    sb.s_log_frag_size     = 0;
    sb.s_blocks_per_group  = bpg;
    sb.s_frags_per_group   = bpg;
    sb.s_inodes_per_group  = ipg;
    sb.s_magic             = EXT2_MAGIC;
    sb.s_state             = 1;          /* clean */
    sb.s_errors            = 1;          /* continue */
    sb.s_rev_level         = EXT2_DYNAMIC_REV;
    sb.s_first_ino         = 11;
    sb.s_inode_size        = (uint16_t)isize;
    sb.s_feature_incompat  = EXT2_FEATURE_INCOMPAT_FILETYPE;
    memcpy(sb.s_volume_name, "makar", 5);

    /* Write super + GDT backups into every group (no sparse_super). */
    for (uint32_t g = 0; g < groups; g++) {
        uint32_t fb = first_data + g * bpg;
        memset(blk, 0, bs);
        sb.s_block_group_nr = (uint16_t)g;
        memcpy(blk, &sb, sizeof(sb));
        if (mk_wb(fb, blk) != 0) { kfree(gd); return -2; }   /* group0: primary @ blk1 */

        for (uint32_t i = 0; i < gdt_blocks; i++) {
            memset(blk, 0, bs);
            uint32_t off = i * bs;
            uint32_t chunk = groups * (uint32_t)sizeof(ext2_gd_t) - off;
            if (chunk > bs) chunk = bs;
            memcpy(blk, (uint8_t *)gd + off, chunk);
            if (mk_wb(fb + 1 + i, blk) != 0) { kfree(gd); return -2; }
        }
    }

    kfree(gd);
    return 0;
}
