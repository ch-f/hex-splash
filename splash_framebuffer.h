// SPDX-License-Identifier: GPL-2.0-only
#ifndef SPLASH_FRAMEBUFFER_H
#define SPLASH_FRAMEBUFFER_H

#include "splash_image.h"

#include <linux/fb.h>
#include <stddef.h>
#include <stdint.h>

struct splash_framebuffer {
	int fd;
	size_t map_size;
	uint8_t *map;
	uint8_t *base;
	uint32_t line_length;
	int bytes_per_pixel;
	struct fb_fix_screeninfo finfo;
	struct fb_var_screeninfo vinfo;
};

int splash_framebuffer_open(struct splash_framebuffer *fb, const char *fb_path);
void splash_framebuffer_release(struct splash_framebuffer *fb);

void splash_fb_clear_black(uint8_t *fb_base, uint32_t xres, uint32_t yres, uint32_t line_length, int bytes_per_pixel);
void splash_fb_blit_centered(uint8_t *fb_base, uint32_t fb_w, uint32_t fb_h,
			     uint32_t line_length, int bytes_per_pixel,
			     const struct fb_var_screeninfo *vinfo,
			     const struct splash_image *img);

#endif
