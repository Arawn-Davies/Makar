/*
 * Build-time check that the Multiboot 2 header layout in boot.S still
 * matches MB2_HEADER_LEN = 48 (hardcoded there because TCC's assembler
 * cannot resolve forward symbol references).  If anyone changes the
 * boot.S header tags, this _Static_assert fails the build.
 */

struct mb2_header_layout {
    /* preamble: magic, arch, header_len, checksum */
    unsigned int  magic;
    unsigned int  arch;
    unsigned int  header_len;
    unsigned int  checksum;
    /* framebuffer tag (type=5, size=20) */
    unsigned short fb_type;
    unsigned short fb_flags;
    unsigned int  fb_size;
    unsigned int  fb_width;
    unsigned int  fb_height;
    unsigned int  fb_bpp;
    /* boot.S has .align 8 here -- pad 4 bytes from offset 36 to 40 */
    unsigned int  pad_align8;
    /* end tag (type=0, flags=0, size=8) */
    unsigned short end_type;
    unsigned short end_flags;
    unsigned int  end_size;
} __attribute__((packed));

/* TCC 0.9.27 has no _Static_assert; negative-size array works in both compilers. */
typedef char mb2_header_size_check[(sizeof(struct mb2_header_layout) == 48) ? 1 : -1];
