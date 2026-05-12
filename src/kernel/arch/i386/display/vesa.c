#include <kernel/vesa.h>
#include <kernel/logo.h>
#include <kernel/tty.h>
#include <kernel/serial.h>
#include <kernel/system.h>
#include <string.h>

static vesa_fb_t fb;
static bool fb_ready = false;

bool vesa_init(multiboot2_info_t *mbi)
{
	if (!mbi)
		return false;

	/* Walk the Multiboot 2 tag list looking for the framebuffer tag. */
	uint8_t *tag_ptr = (uint8_t *)mbi + sizeof(multiboot2_info_t);
	uint8_t *info_end = (uint8_t *)mbi + mbi->total_size;

	multiboot2_tag_framebuffer_t *fb_tag = NULL;

	while (tag_ptr < info_end) {
		multiboot2_tag_t *tag = (multiboot2_tag_t *)tag_ptr;

		if (tag->type == MULTIBOOT2_TAG_TYPE_END)
			break;

		if (tag->type == MULTIBOOT2_TAG_TYPE_FRAMEBUFFER) {
			fb_tag = (multiboot2_tag_framebuffer_t *)tag;
			break;
		}

		tag_ptr += (tag->size + 7) & ~7u;
	}

	if (!fb_tag) {
		t_writestring("VESA: no framebuffer tag from bootloader\n");
		KLOG("vesa_init: no framebuffer tag from bootloader\n");
		return false;
	}

	if (fb_tag->framebuffer_type != MULTIBOOT2_FRAMEBUFFER_TYPE_RGB) {
		t_writestring("VESA: framebuffer is not direct-colour RGB\n");
		KLOG("vesa_init: framebuffer is not direct-colour RGB\n");
		return false;
	}

	if (fb_tag->framebuffer_bpp == 0) {
		t_writestring("VESA: framebuffer bpp is zero\n");
		KLOG("vesa_init: framebuffer bpp is zero\n");
		return false;
	}

	if (fb_tag->framebuffer_bpp % 8 != 0) {
		t_writestring("VESA: framebuffer bpp is not byte-aligned\n");
		KLOG("vesa_init: framebuffer bpp is not byte-aligned\n");
		return false;
	}

	fb.addr        = (uint32_t *)(uintptr_t)fb_tag->framebuffer_addr;
	fb.pitch       = fb_tag->framebuffer_pitch;
	fb.width       = fb_tag->framebuffer_width;
	fb.height      = fb_tag->framebuffer_height;
	fb.bpp         = fb_tag->framebuffer_bpp;
	fb.red_shift   = fb_tag->red_field_position;
	fb.red_bits    = fb_tag->red_mask_size;
	fb.green_shift = fb_tag->green_field_position;
	fb.green_bits  = fb_tag->green_mask_size;
	fb.blue_shift  = fb_tag->blue_field_position;
	fb.blue_bits   = fb_tag->blue_mask_size;

	fb_ready = true;

	t_writestring("VESA: framebuffer ");
	t_dec(fb.width);
	t_writestring("x");
	t_dec(fb.height);
	t_writestring("x");
	t_dec(fb.bpp);
	t_writestring(" @ 0x");
	t_hex((uint32_t)(uintptr_t)fb.addr);
	t_writestring("\n");

	KLOG("vesa_init: framebuffer ");
	KLOG_DEC(fb.width);
	KLOG("x");
	KLOG_DEC(fb.height);
	KLOG("x");
	KLOG_DEC(fb.bpp);
	KLOG(" @ ");
	KLOG_HEX((uint32_t)(uintptr_t)fb.addr);
	KLOG("\n");

	return true;
}

const vesa_fb_t *vesa_get_fb(void)
{
	return fb_ready ? &fb : NULL;
}

void vesa_put_pixel(uint32_t x, uint32_t y, uint32_t colour)
{
	if (!fb_ready)
		return;
	if (x >= fb.width || y >= fb.height)
		PANIC("vesa_put_pixel: coordinates out of framebuffer bounds");

	uint32_t bytes_per_pixel = fb.bpp / 8;
	uint8_t *pixel = (uint8_t *)fb.addr + y * fb.pitch + x * bytes_per_pixel;

	if (bytes_per_pixel == 4) {
		*(uint32_t *)pixel = colour;
	} else {
		for (uint32_t i = 0; i < bytes_per_pixel && i < 4; i++)
			pixel[i] = (uint8_t)(colour >> (i * 8));
	}
}

void vesa_clear(uint32_t colour)
{
	if (!fb_ready)
		return;

	uint32_t bytes_per_pixel = fb.bpp / 8;

	if (bytes_per_pixel == 4) {
		uint32_t *row = (uint32_t *)fb.addr;
		uint32_t  stride = fb.pitch / 4;
		uint32_t  total  = fb.height * stride;
		for (uint32_t i = 0; i < total; i++)
			row[i] = colour;
	} else {
		/* Non-32bpp fallback - write row-by-row using the byte layout. */
		uint8_t  px[4];
		for (uint32_t i = 0; i < bytes_per_pixel && i < 4; i++)
			px[i] = (uint8_t)(colour >> (i * 8));

		for (uint32_t y = 0; y < fb.height; y++) {
			uint8_t *row = (uint8_t *)fb.addr + y * fb.pitch;
			for (uint32_t x = 0; x < fb.width; x++) {
				uint8_t *p = row + x * bytes_per_pixel;
				for (uint32_t i = 0; i < bytes_per_pixel; i++)
					p[i] = px[i];
			}
		}
	}
}

void vesa_disable(void)
{
	fb_ready = false;
}

void vesa_update_geometry(uint32_t width, uint32_t height, uint8_t bpp)
{
	fb.width  = width;
	fb.height = height;
	fb.bpp    = bpp;
	fb.pitch  = width * ((uint32_t)bpp / 8u);
	fb_ready  = true;
}

void vesa_blit_logo(uint32_t fg, uint32_t bg)
{
	if (!fb_ready)
		return;

	uint32_t x_off = (fb.width  > LOGO_WIDTH)  ? (fb.width  - LOGO_WIDTH)  / 2 : 0;
	uint32_t y_off = (fb.height > LOGO_HEIGHT) ? (fb.height - LOGO_HEIGHT) / 2 : 0;

	for (uint32_t y = 0; y < LOGO_HEIGHT; y++) {
		for (uint32_t x = 0; x < LOGO_WIDTH; x++) {
			uint8_t bit = (logo_bits[y * LOGO_STRIDE + x / 8] >> (7 - x % 8)) & 1;
			vesa_put_pixel(x_off + x, y_off + y, bit ? fg : bg);
		}
	}
}
