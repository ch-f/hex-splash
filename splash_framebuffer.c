// SPDX-License-Identifier: GPL-2.0-only
#include "splash_framebuffer.h"

#include "splash_util.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static inline uint32_t chan_to_field(uint8_t c, const struct fb_bitfield *bf)
{
	uint32_t offset;
	uint32_t len;
	uint32_t max;
	uint32_t val;

	if (!bf || bf->length == 0u)
		return 0u;

	offset = bf->offset;
	len = bf->length;

	if (offset >= 32u)
		return 0u;
	if (len > 32u - offset)
		len = 32u - offset;
	if (len == 0u)
		return 0u;

	max = (len == 32u) ? 0xFFFFFFFFu : ((1u << len) - 1u);
	val = (uint32_t)((uint64_t)c * (uint64_t)max + 127u) / 255u;
	return val << offset;
}

static void build_channel_lut(uint32_t lut[256], const struct fb_bitfield *bf)
{
	for (uint32_t i = 0; i < 256u; i++)
		lut[i] = chan_to_field((uint8_t)i, bf);
}

static inline void write_pixel_bytes(uint8_t *dst, uint32_t pix, int bytes_per_pixel)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	for (int i = 0; i < bytes_per_pixel; i++)
		dst[i] = (uint8_t)((pix >> (i * 8)) & 0xFFu);
#else
	for (int i = 0; i < bytes_per_pixel; i++)
		dst[i] = (uint8_t)((pix >> ((bytes_per_pixel - 1 - i) * 8)) & 0xFFu);
#endif
}

static const char *validate_framebuffer_geometry(const struct fb_fix_screeninfo *finfo,
					  const struct fb_var_screeninfo *vinfo,
					  int bytes_per_pixel)
{
	const size_t fb_size = (size_t)finfo->smem_len;
	const size_t stride = (size_t)finfo->line_length;
	const size_t xres = (size_t)vinfo->xres;
	const size_t yres = (size_t)vinfo->yres;
	const size_t xres_virtual = (size_t)vinfo->xres_virtual;
	const size_t yres_virtual = (size_t)vinfo->yres_virtual;
	const size_t xoffset = (size_t)vinfo->xoffset;
	const size_t yoffset = (size_t)vinfo->yoffset;
	const size_t bpp = (size_t)bytes_per_pixel;

	size_t visible_row_bytes;
	size_t last_row_start;
	size_t last_visible_row;

	if (fb_size == 0u)
		return "framebuffer memory size is zero";
	if (stride == 0u)
		return "framebuffer reports line_length=0";

	if (xres_virtual < xres || yres_virtual < yres)
		return "framebuffer virtual resolution smaller than visible";
	if (xoffset > xres_virtual || xres > xres_virtual - xoffset)
		return "framebuffer x offset out of range for virtual resolution";
	if (yoffset > yres_virtual || yres > yres_virtual - yoffset)
		return "framebuffer y offset out of range for virtual resolution";

	if (splash_mul_overflow_size_t(xoffset + xres, bpp, &visible_row_bytes))
		return "framebuffer geometry overflow";
	if (visible_row_bytes > stride)
		return "visible row exceeds finfo.line_length (stride)";

	last_visible_row = yoffset + yres - 1u;
	if (splash_mul_overflow_size_t(last_visible_row, stride, &last_row_start))
		return "framebuffer geometry overflow";
	if (last_row_start > SIZE_MAX - visible_row_bytes)
		return "framebuffer geometry overflow";
	if (last_row_start + visible_row_bytes > fb_size)
		return "framebuffer smem_len too small for reported geometry";
	return NULL;
}

int splash_framebuffer_open(struct splash_framebuffer *fb, const char *fb_path)
{
	size_t xoffset;
	size_t yoffset;
	size_t stride;
	size_t bpp;
	const char *geometry_error;

	memset(fb, 0, sizeof(*fb));
	fb->fd = -1;

	fb->fd = open(fb_path, O_RDWR);
	if (fb->fd < 0) {
		splash_report_errno("open framebuffer");
		goto fail;
	}

	(void)ioctl(fb->fd, FBIOBLANK, FB_BLANK_UNBLANK);

	if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &fb->finfo) != 0) {
		splash_report_errno("FBIOGET_FSCREENINFO");
		goto fail;
	}
	if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &fb->vinfo) != 0) {
		splash_report_errno("FBIOGET_VSCREENINFO");
		goto fail;
	}

	if (fb->vinfo.xres == 0u || fb->vinfo.yres == 0u) {
		splash_report_error("framebuffer reports zero resolution");
		goto fail;
	}

	fb->bytes_per_pixel = (int)((fb->vinfo.bits_per_pixel + 7) / 8);
	if (!(fb->bytes_per_pixel == 2 || fb->bytes_per_pixel == 3 || fb->bytes_per_pixel == 4)) {
		fprintf(stderr, "Unsupported framebuffer bpp=%u (bytes_per_pixel=%d).\n",
			fb->vinfo.bits_per_pixel, fb->bytes_per_pixel);
		goto fail;
	}
	if (fb->finfo.visual != FB_VISUAL_TRUECOLOR && fb->finfo.visual != FB_VISUAL_DIRECTCOLOR) {
		fprintf(stderr, "Unsupported framebuffer visual=%u (need TRUECOLOR/DIRECTCOLOR).\n", fb->finfo.visual);
		goto fail;
	}

	geometry_error = validate_framebuffer_geometry(&fb->finfo, &fb->vinfo, fb->bytes_per_pixel);
	if (geometry_error) {
		splash_report_error(geometry_error);
		goto fail;
	}

	fb->map_size = (size_t)fb->finfo.smem_len;
	fb->map = (uint8_t *)mmap(NULL, fb->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
	if (fb->map == MAP_FAILED) {
		fb->map = NULL;
		splash_report_errno("mmap framebuffer");
		goto fail;
	}

	fb->line_length = fb->finfo.line_length;

	xoffset = (size_t)fb->vinfo.xoffset;
	yoffset = (size_t)fb->vinfo.yoffset;
	stride = (size_t)fb->line_length;
	bpp = (size_t)fb->bytes_per_pixel;

	fb->base = fb->map + yoffset * stride + xoffset * bpp;
	return 0;

fail:
	splash_framebuffer_release(fb);
	return 1;
}

void splash_framebuffer_release(struct splash_framebuffer *fb)
{
	if (!fb)
		return;
	if (fb->map)
		munmap(fb->map, fb->map_size);
	if (fb->fd >= 0)
		close(fb->fd);

	fb->map = NULL;
	fb->base = NULL;
	fb->map_size = 0u;
	fb->fd = -1;
}

void splash_fb_clear_black(uint8_t *fb_base, uint32_t xres, uint32_t yres, uint32_t line_length, int bytes_per_pixel)
{
	size_t row_bytes = (size_t)xres * (size_t)bytes_per_pixel;
	for (uint32_t y = 0; y < yres; y++)
		memset(fb_base + (size_t)y * line_length, 0, row_bytes);
}

void splash_fb_blit_centered(uint8_t *fb_base, uint32_t fb_w, uint32_t fb_h,
			     uint32_t line_length, int bytes_per_pixel,
			     const struct fb_var_screeninfo *vinfo,
			     const struct splash_image *img)
{
	uint32_t draw_w;
	uint32_t draw_h;
	uint32_t x0;
	uint32_t y0;

	uint32_t lut_r[256];
	uint32_t lut_g[256];
	uint32_t lut_b[256];
	uint32_t alpha_mask;

	if (!img || !img->rgba)
		return;

	draw_w = (img->w <= fb_w) ? img->w : fb_w;
	draw_h = (img->h <= fb_h) ? img->h : fb_h;
	x0 = (fb_w - draw_w) / 2u;
	y0 = (fb_h - draw_h) / 2u;

	alpha_mask = vinfo->transp.length ? chan_to_field(255u, &vinfo->transp) : 0u;
	build_channel_lut(lut_r, &vinfo->red);
	build_channel_lut(lut_g, &vinfo->green);
	build_channel_lut(lut_b, &vinfo->blue);

	for (uint32_t y = 0; y < draw_h; y++) {
		uint8_t *dst_px = fb_base + (size_t)(y0 + y) * line_length + (size_t)x0 * (size_t)bytes_per_pixel;
		const uint8_t *src_px = img->rgba + (size_t)y * (size_t)img->w * 4u;

		for (uint32_t x = 0; x < draw_w; x++) {
			const uint32_t pix = lut_r[src_px[0]] | lut_g[src_px[1]] | lut_b[src_px[2]] | alpha_mask;
			write_pixel_bytes(dst_px, pix, bytes_per_pixel);

			src_px += 4u;
			dst_px += (size_t)bytes_per_pixel;
		}
	}
}
