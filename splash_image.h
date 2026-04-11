// SPDX-License-Identifier: GPL-2.0-only
#ifndef SPLASH_IMAGE_H
#define SPLASH_IMAGE_H

#include <stddef.h>
#include <stdint.h>

struct splash_image {
	uint32_t w;
	uint32_t h;
	uint8_t *rgba; /* w*h*4, RGBA8 */
};

void splash_image_free(struct splash_image *img);

/* Downscale-to-fit (preserve aspect ratio, no upscaling). */
void splash_compute_fit_dimensions(uint32_t src_w, uint32_t src_h, uint32_t max_w, uint32_t max_h,
				   uint32_t *dst_w, uint32_t *dst_h);

/* Heuristic: should we use the fast decode path for src->dst? */
int splash_should_use_fast_decode(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h);

/* PNG helpers */
int splash_png_try_probe_dimensions_from_file(const char *path, uint32_t *w, uint32_t *h);
int splash_png_try_load_rgba8_from_file(const char *path, struct splash_image *out);
int splash_png_try_load_nearest_scaled_rgba8_from_file(const char *path, uint32_t dst_w, uint32_t dst_h,
							   struct splash_image *out);
struct splash_image splash_png_load_rgba8_from_memory(const void *data, size_t size, const char *source_name);

/* Image processing */
void splash_composite_onto_black_inplace(struct splash_image *img);
struct splash_image splash_rotate_rgba(const struct splash_image *src, uint32_t rotation_degrees);
struct splash_image splash_scale_bilinear_rgba(const struct splash_image *src, uint32_t dst_w, uint32_t dst_h);

/*
 * Load logo:
 * - if png_path is provided: try file, else fall back to built-in
 * - composite onto black (RGBA becomes fully opaque)
 * If used_builtin != NULL, it is set to 1 when built-in is used.
 */
struct splash_image splash_load_logo_image(const char *png_path, int *used_builtin);

#endif