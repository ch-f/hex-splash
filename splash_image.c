// SPDX-License-Identifier: GPL-2.0-only
#include "splash_image.h"

#include "splash_util.h"

#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const unsigned char hex_splash_default_logo_png[];
extern const size_t hex_splash_default_logo_png_len;

static struct splash_image image_error(const char *message)
{
	splash_report_error(message);
	return (struct splash_image){ 0 };
}

/* Fast decode heuristics */
static const uint64_t k_fast_decode_min_src_pixels = 2000000ull;
static const uint64_t k_fast_decode_min_ratio = 4ull;

/* Center-of-pixel mapping for nearest-neighbor downscale sampling */
static uint32_t sample_index_centered(uint32_t i, uint32_t src_n, uint32_t dst_n)
{
	uint64_t num;
	uint64_t den;
	uint32_t idx;

	if (src_n <= 1u || dst_n <= 1u)
		return 0u;

	num = (uint64_t)(2u * i + 1u) * (uint64_t)src_n;
	den = (uint64_t)(2u * dst_n);
	idx = (uint32_t)(num / den);

	if (idx >= src_n)
		idx = src_n - 1u;

	return idx;
}

static inline void premultiply_over_black_to_opaque_rgb(uint8_t *dst_rgba, const uint8_t *src_rgba)
{
	const uint32_t a = src_rgba[3];

	if (a == 255u) {
		dst_rgba[0] = src_rgba[0];
		dst_rgba[1] = src_rgba[1];
		dst_rgba[2] = src_rgba[2];
	} else if (a == 0u) {
		dst_rgba[0] = 0u;
		dst_rgba[1] = 0u;
		dst_rgba[2] = 0u;
	} else {
		dst_rgba[0] = (uint8_t)((src_rgba[0] * a + 127u) / 255u);
		dst_rgba[1] = (uint8_t)((src_rgba[1] * a + 127u) / 255u);
		dst_rgba[2] = (uint8_t)((src_rgba[2] * a + 127u) / 255u);
	}
	dst_rgba[3] = 255u;
}

static inline void write_sampled_fast_decode_row(uint8_t *dst_row, const uint8_t *src_row,
						 const uint32_t *x_map, uint32_t dst_w)
{
	for (uint32_t x = 0; x < dst_w; x++) {
		const uint8_t *src_px = src_row + (size_t)x_map[x] * 4u;
		premultiply_over_black_to_opaque_rgb(dst_row, src_px);
		dst_row += 4u;
	}
}

static void fill_remaining_rows(uint8_t *dst_buf, uint32_t dst_w, uint32_t dst_h, uint32_t start_row)
{
	const size_t one_row = (size_t)dst_w * 4u;

	if (start_row >= dst_h)
		return;

	/* If nothing was written, just black everything. */
	if (start_row == 0u) {
		memset(dst_buf, 0, one_row * (size_t)dst_h);
		return;
	}

	/* Repeat the last written row (should only happen in edge cases). */
	uint8_t *last = dst_buf + (size_t)(start_row - 1u) * one_row;
	for (uint32_t y = start_row; y < dst_h; y++)
		memcpy(dst_buf + (size_t)y * one_row, last, one_row);
}

struct fast_decode_buffers {
	uint8_t *src_row;
	uint8_t *dst_buf;
	uint32_t *x_map;
	uint32_t *y_map;
};

static void free_fast_decode_buffers(struct fast_decode_buffers *bufs)
{
	if (!bufs)
		return;
	free(bufs->y_map);
	free(bufs->x_map);
	free(bufs->src_row);
	free(bufs->dst_buf);
	*bufs = (struct fast_decode_buffers){ 0 };
}

static int alloc_fast_decode_buffers(size_t row_bytes, uint32_t dst_w, uint32_t dst_h,
				     struct fast_decode_buffers *bufs)
{
	size_t dst_pixels;
	size_t dst_bytes;

	*bufs = (struct fast_decode_buffers){ 0 };

	if (splash_mul_overflow_size_t((size_t)dst_w, (size_t)dst_h, &dst_pixels) ||
	    splash_mul_overflow_size_t(dst_pixels, 4u, &dst_bytes))
		return 0;

	bufs->src_row = (uint8_t *)malloc(row_bytes);
	bufs->x_map = (uint32_t *)malloc((size_t)dst_w * sizeof(*bufs->x_map));
	bufs->y_map = (uint32_t *)malloc((size_t)dst_h * sizeof(*bufs->y_map));
	bufs->dst_buf = (uint8_t *)malloc(dst_bytes);

	if (!bufs->src_row || !bufs->x_map || !bufs->y_map || !bufs->dst_buf) {
		free_fast_decode_buffers(bufs);
		return 0;
	}

	return 1;
}

static void build_sampling_maps(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h,
				uint32_t *x_map, uint32_t *y_map)
{
	for (uint32_t x = 0; x < dst_w; x++)
		x_map[x] = sample_index_centered(x, src_w, dst_w);
	for (uint32_t y = 0; y < dst_h; y++)
		y_map[y] = sample_index_centered(y, src_h, dst_h);
}

static void decode_sampled_rows(png_structp png_ptr, const struct fast_decode_buffers *bufs,
				uint32_t src_h, uint32_t dst_w, uint32_t dst_h, uint32_t *dst_y_inout)
{
	uint32_t dst_y = *dst_y_inout;

	for (uint32_t src_y = 0; src_y < src_h; src_y++) {
		if (dst_y >= dst_h || bufs->y_map[dst_y] != src_y) {
			png_read_row(png_ptr, NULL, NULL);
			continue;
		}

		png_read_row(png_ptr, bufs->src_row, NULL);

		while (dst_y < dst_h && bufs->y_map[dst_y] == src_y) {
			uint8_t *out_row = bufs->dst_buf + (size_t)dst_y * (size_t)dst_w * 4u;
			write_sampled_fast_decode_row(out_row, bufs->src_row, bufs->x_map, dst_w);
			dst_y++;
		}
	}

	*dst_y_inout = dst_y;
}

int splash_png_try_load_rgba8_from_file(const char *path, struct splash_image *out)
{
	png_image img;
	size_t pixels;
	size_t size;
	uint8_t *buf = NULL;

	if (!out)
		return 0;
	*out = (struct splash_image){ 0 };

	memset(&img, 0, sizeof(img));
	img.version = PNG_IMAGE_VERSION;

	if (!png_image_begin_read_from_file(&img, path)) {
		fprintf(stderr, "warning: libpng (%s): %s; falling back to built-in logo\n", path, img.message);
		png_image_free(&img);
		return 0;
	}

	if (img.width == 0u || img.height == 0u) {
		fprintf(stderr, "warning: libpng (%s): zero-sized image; falling back to built-in logo\n", path);
		png_image_free(&img);
		return 0;
	}

	img.format = PNG_FORMAT_RGBA;

	/* size = width * height * 4, with overflow check */
	if (splash_mul_overflow_size_t((size_t)img.width, (size_t)img.height, &pixels) ||
		splash_mul_overflow_size_t(pixels, 4u, &size)) {
		fprintf(stderr, "warning: libpng (%s): image too large; falling back to built-in logo\n", path);
		png_image_free(&img);
		return 0;
		}

	buf = (uint8_t *)malloc(size);
	if (!buf) {
		fprintf(stderr, "warning: libpng (%s): out of memory; falling back to built-in logo\n", path);
		png_image_free(&img);
		return 0;
	}

	if (!png_image_finish_read(&img, NULL, buf, 0, NULL)) {
		fprintf(stderr, "warning: libpng (%s): %s; falling back to built-in logo\n", path, img.message);
		free(buf);
		png_image_free(&img);
		return 0;
	}

	out->w = img.width;
	out->h = img.height;
	out->rgba = buf;

	png_image_free(&img);
	return 1;
}

int splash_png_try_probe_dimensions_from_file(const char *path, uint32_t *w, uint32_t *h)
{
	png_image img;

	memset(&img, 0, sizeof(img));
	img.version = PNG_IMAGE_VERSION;

	if (!png_image_begin_read_from_file(&img, path)) {
		png_image_free(&img);
		return 0;
	}

	if (w)
		*w = img.width;
	if (h)
		*h = img.height;

	png_image_free(&img);
	return 1;
}

static int decode_nearest_scaled_rgba8_from_stream(FILE *fp, uint32_t dst_w, uint32_t dst_h,
						   struct splash_image *out)
{
	png_structp png_ptr = NULL;
	png_infop info_ptr = NULL;
	/* libpng can longjmp after allocations. Keep ownership off the stack:
	 * automatic objects modified after setjmp would be indeterminate. */
	struct fast_decode_buffers *bufs = calloc(1, sizeof(*bufs));

	png_uint_32 src_w = 0, src_h = 0;
	int bit_depth = 0, color_type = 0, interlace_type = 0, compression_type = 0, filter_method = 0;
	size_t row_bytes;
	uint32_t dst_y = 0;

	if (!bufs)
		return 0;

	png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!png_ptr)
		goto fail;

	info_ptr = png_create_info_struct(png_ptr);
	if (!info_ptr)
		goto fail;

	if (setjmp(png_jmpbuf(png_ptr)))
		goto fail;

	png_init_io(png_ptr, fp);
	png_read_info(png_ptr, info_ptr);

	png_get_IHDR(png_ptr, info_ptr, &src_w, &src_h, &bit_depth, &color_type,
		     &interlace_type, &compression_type, &filter_method);

	/* Fast path does not support interlaced PNGs -> let caller fall back to full decode. */
	if (interlace_type != PNG_INTERLACE_NONE)
		goto fail;
	if (src_w == 0u || src_h == 0u)
		goto fail;

	/* Convert to RGBA8 */
	if (bit_depth == 16)
		png_set_strip_16(png_ptr);
	if (color_type == PNG_COLOR_TYPE_PALETTE)
		png_set_palette_to_rgb(png_ptr);
	if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
		png_set_expand_gray_1_2_4_to_8(png_ptr);
	if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
		png_set_tRNS_to_alpha(png_ptr);
	if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
		png_set_gray_to_rgb(png_ptr);
	if ((color_type & PNG_COLOR_MASK_ALPHA) == 0)
		png_set_add_alpha(png_ptr, 0xFFu, PNG_FILLER_AFTER);

	png_read_update_info(png_ptr, info_ptr);
	row_bytes = png_get_rowbytes(png_ptr, info_ptr);

	if (row_bytes < (size_t)src_w * 4u)
		goto fail;

	if (!alloc_fast_decode_buffers(row_bytes, dst_w, dst_h, bufs))
		goto fail;

	build_sampling_maps((uint32_t)src_w, (uint32_t)src_h, dst_w, dst_h, bufs->x_map, bufs->y_map);

	decode_sampled_rows(png_ptr, bufs, (uint32_t)src_h, dst_w, dst_h, &dst_y);
	fill_remaining_rows(bufs->dst_buf, dst_w, dst_h, dst_y);

	png_read_end(png_ptr, NULL);

	out->w = dst_w;
	out->h = dst_h;
	out->rgba = bufs->dst_buf;
	bufs->dst_buf = NULL;
	free_fast_decode_buffers(bufs);
	free(bufs);
	png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
	return 1;

fail:
	free_fast_decode_buffers(bufs);
	free(bufs);
	if (png_ptr || info_ptr)
		png_destroy_read_struct(png_ptr ? &png_ptr : NULL, info_ptr ? &info_ptr : NULL, NULL);
	return 0;
}

int splash_png_try_load_nearest_scaled_rgba8_from_file(const char *path, uint32_t dst_w, uint32_t dst_h,
						       struct splash_image *out)
{
	FILE *fp;
	int decoded;

	if (!out || dst_w == 0u || dst_h == 0u)
		return 0;
	*out = (struct splash_image){ 0 };

	fp = fopen(path, "rb");
	if (!fp)
		return 0;

	decoded = decode_nearest_scaled_rgba8_from_stream(fp, dst_w, dst_h, out);
	fclose(fp);

	return decoded;
}

struct splash_image splash_png_load_rgba8_from_memory(const void *data, size_t size, const char *source_name)
{
	struct splash_image out = { 0 };
	const char *label = source_name ? source_name : "memory PNG";
	png_image img;
	size_t pixels;
	size_t rgba_size;
	uint8_t *buf = NULL;

	memset(&img, 0, sizeof(img));
	img.version = PNG_IMAGE_VERSION;

	if (!png_image_begin_read_from_memory(&img, data, size)) {
		fprintf(stderr, "libpng (%s): %s\n", label, img.message);
		goto fail;
	}

	if (img.width == 0u || img.height == 0u) {
		fprintf(stderr, "libpng (%s): zero-sized image\n", label);
		goto fail;
	}

	img.format = PNG_FORMAT_RGBA;

	/* rgba_size = width * height * 4, with overflow check */
	if (splash_mul_overflow_size_t((size_t)img.width, (size_t)img.height, &pixels) ||
		splash_mul_overflow_size_t(pixels, 4u, &rgba_size)) {
		fprintf(stderr, "libpng (%s): image too large\n", label);
		goto fail;
	}

	buf = (uint8_t *)malloc(rgba_size);
	if (!buf) {
		splash_report_error("out of memory");
		goto fail;
	}

	if (!png_image_finish_read(&img, NULL, buf, 0, NULL)) {
		fprintf(stderr, "libpng (%s): %s\n", label, img.message);
		goto fail;
	}

	out.w = img.width;
	out.h = img.height;
	out.rgba = buf;

	png_image_free(&img);
	return out;

fail:
	free(buf);
	png_image_free(&img);
	return (struct splash_image){ 0 };
}

void splash_image_free(struct splash_image *img)
{
	if (!img)
		return;
	free(img->rgba);
	img->rgba = NULL;
	img->w = 0;
	img->h = 0;
}

int splash_composite_onto_black_inplace(struct splash_image *img)
{
	size_t n;

	if (!img || !img->rgba)
		return 0;

	if (splash_mul_overflow_size_t((size_t)img->w, (size_t)img->h, &n)) {
		splash_report_error("image too large");
		return 0;
	}

	for (size_t i = 0; i < n; i++) {
		uint8_t *p = img->rgba + i * 4u;
		premultiply_over_black_to_opaque_rgb(p, p);
	}
	return 1;
}

struct splash_image splash_rotate_rgba(const struct splash_image *src, uint32_t rotation_degrees)
{
	struct splash_image dst = { 0 };
	size_t pixels;
	size_t size;

	if (!src || !src->rgba || src->w == 0u || src->h == 0u)
		return image_error("rotate: invalid source image");

	switch (rotation_degrees) {
	case 0u:
	case 180u:
		dst.w = src->w;
		dst.h = src->h;
		break;
	case 90u:
	case 270u:
		dst.w = src->h;
		dst.h = src->w;
		break;
	default:
		return image_error("rotate: invalid rotation");
	}

	if (splash_mul_overflow_size_t((size_t)dst.w, (size_t)dst.h, &pixels) ||
	    splash_mul_overflow_size_t(pixels, 4u, &size))
		return image_error("rotate: image too large");

	dst.rgba = (uint8_t *)malloc(size);
	if (!dst.rgba)
		return image_error("out of memory");

	for (uint32_t y = 0; y < src->h; y++) {
		for (uint32_t x = 0; x < src->w; x++) {
			uint32_t dst_x;
			uint32_t dst_y;
			const uint8_t *src_px = src->rgba + (((size_t)y * (size_t)src->w + (size_t)x) * 4u);
			uint8_t *dst_px;

			switch (rotation_degrees) {
			case 0u:
				dst_x = x;
				dst_y = y;
				break;
			case 90u:
				dst_x = src->h - 1u - y;
				dst_y = x;
				break;
			case 180u:
				dst_x = src->w - 1u - x;
				dst_y = src->h - 1u - y;
				break;
			case 270u:
				dst_x = y;
				dst_y = src->w - 1u - x;
				break;
			default:
				dst_x = 0u;
				dst_y = 0u;
				break;
			}

			dst_px = dst.rgba + (((size_t)dst_y * (size_t)dst.w + (size_t)dst_x) * 4u);
			memcpy(dst_px, src_px, 4u);
		}
	}

	return dst;
}

struct bilinear_axis_map {
	uint32_t i0;
	uint32_t i1;
	uint16_t w;
};

static struct bilinear_axis_map *build_bilinear_axis_map(uint32_t src_n, uint32_t dst_n)
{
	size_t bytes;
	struct bilinear_axis_map *map;

	if (dst_n == 0u) {
		splash_report_error("scale: invalid destination size");
		return NULL;
	}

	if (splash_mul_overflow_size_t((size_t)dst_n, sizeof(*map), &bytes)) {
		splash_report_error("scale: mapping too large");
		return NULL;
	}

	map = (struct bilinear_axis_map *)malloc(bytes);
	if (!map) {
		splash_report_error("out of memory");
		return NULL;
	}

	if (dst_n <= 1u || src_n <= 1u) {
		for (uint32_t i = 0; i < dst_n; i++) {
			map[i].i0 = 0u;
			map[i].i1 = 0u;
			map[i].w = 0u;
		}
		return map;
	}

	for (uint32_t i = 0; i < dst_n; i++) {
		uint64_t num = (uint64_t)i * (uint64_t)(src_n - 1u);
		uint32_t i0 = (uint32_t)(num / (dst_n - 1u));
		uint32_t i1 = (i0 + 1u < src_n) ? (i0 + 1u) : i0;

		map[i].i0 = i0;
		map[i].i1 = i1;
		map[i].w = (uint16_t)((num % (dst_n - 1u)) * 65536ull / (dst_n - 1u));
	}

	return map;
}

static void scale_bilinear_all_rows(const uint8_t *src_rgba, uint8_t *dst_rgba,
				    const struct bilinear_axis_map *x_map,
				    const struct bilinear_axis_map *y_map,
				    size_t src_stride, size_t dst_stride,
				    uint32_t dst_w, uint32_t dst_h)
{
	for (uint32_t y = 0; y < dst_h; y++) {
		const struct bilinear_axis_map ym = y_map[y];
		const uint32_t wy = ym.w;
		const uint32_t inv_wy = 65536u - wy;

		const uint8_t *row0 = src_rgba + (size_t)ym.i0 * src_stride;
		const uint8_t *row1 = src_rgba + (size_t)ym.i1 * src_stride;
		uint8_t *out_px = dst_rgba + (size_t)y * dst_stride;

		for (uint32_t x = 0; x < dst_w; x++) {
			const struct bilinear_axis_map xm = x_map[x];
			const uint32_t wx = xm.w;
			const uint32_t inv_wx = 65536u - wx;

			const uint8_t *p00 = row0 + ((size_t)xm.i0 << 2);
			const uint8_t *p10 = row0 + ((size_t)xm.i1 << 2);
			const uint8_t *p01 = row1 + ((size_t)xm.i0 << 2);
			const uint8_t *p11 = row1 + ((size_t)xm.i1 << 2);

			for (uint32_t c = 0; c < 3u; c++) {
				uint32_t top = (p00[c] * inv_wx + p10[c] * wx) >> 16;
				uint32_t bot = (p01[c] * inv_wx + p11[c] * wx) >> 16;
				out_px[c] = (uint8_t)((top * inv_wy + bot * wy) >> 16);
			}

			out_px[3] = 255u;
			out_px += 4u;
		}
	}
}

struct splash_image splash_scale_bilinear_rgba(const struct splash_image *src, uint32_t dst_w, uint32_t dst_h)
{
	struct splash_image dst = { 0 };
	size_t pixels;
	size_t size;
	size_t src_stride;
	size_t dst_stride;
	struct bilinear_axis_map *x_map = NULL;
	struct bilinear_axis_map *y_map = NULL;

	if (!src || !src->rgba || src->w == 0u || src->h == 0u)
		return image_error("scale: invalid source image");
	if (dst_w == 0u || dst_h == 0u)
		return image_error("scale: invalid destination size");

	if (splash_mul_overflow_size_t((size_t)dst_w, (size_t)dst_h, &pixels) ||
	    splash_mul_overflow_size_t(pixels, 4u, &size))
		return image_error("scale: image too large");

	dst.w = dst_w;
	dst.h = dst_h;
	dst.rgba = (uint8_t *)malloc(size);
	if (!dst.rgba)
		return image_error("out of memory");

	src_stride = (size_t)src->w * 4u;
	dst_stride = (size_t)dst_w * 4u;

	x_map = build_bilinear_axis_map(src->w, dst_w);
	if (!x_map)
		goto fail;
	y_map = build_bilinear_axis_map(src->h, dst_h);
	if (!y_map)
		goto fail;

	scale_bilinear_all_rows(src->rgba, dst.rgba, x_map, y_map, src_stride, dst_stride, dst_w, dst_h);

	free(y_map);
	free(x_map);
	return dst;

fail:
	free(y_map);
	free(x_map);
	splash_image_free(&dst);
	return (struct splash_image){ 0 };
}

struct splash_image splash_load_logo_image(const char *png_path, int *used_builtin)
{
	struct splash_image img = { 0 };
	int builtin = 0;

	if (png_path && splash_png_try_load_rgba8_from_file(png_path, &img)) {
		builtin = 0;
	} else {
		img = splash_png_load_rgba8_from_memory(hex_splash_default_logo_png,
							hex_splash_default_logo_png_len,
							"built-in logo");
		builtin = 1;
	}

	if (used_builtin)
		*used_builtin = builtin;

	if (!img.rgba || img.w == 0u || img.h == 0u) {
		splash_image_free(&img);
		return image_error("could not load logo image");
	}

	/* Important: do this before scaling to avoid edge halos on transparent PNGs. */
	if (!splash_composite_onto_black_inplace(&img)) {
		splash_image_free(&img);
		return (struct splash_image){ 0 };
	}
	return img;
}

void splash_compute_fit_dimensions(uint32_t src_w, uint32_t src_h, uint32_t max_w, uint32_t max_h,
				   uint32_t *dst_w, uint32_t *dst_h)
{
	*dst_w = src_w;
	*dst_h = src_h;

	if (*dst_w > max_w) {
		*dst_h = (uint32_t)((uint64_t)*dst_h * (uint64_t)max_w / (uint64_t)*dst_w);
		*dst_w = max_w;
	}
	if (*dst_h > max_h) {
		*dst_w = (uint32_t)((uint64_t)*dst_w * (uint64_t)max_h / (uint64_t)*dst_h);
		*dst_h = max_h;
	}

	if (*dst_w == 0u)
		*dst_w = 1u;
	if (*dst_h == 0u)
		*dst_h = 1u;
}

int splash_should_use_fast_decode(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h)
{
	uint64_t src_pixels;
	uint64_t dst_pixels;
	uint32_t mode = splash_parse_env_u32("HEX_SPLASH_FAST_DECODE", 2u);

	if (mode == 0u)
		return 0;
	if (mode == 1u)
		return 1;

	if (src_w == 0u || src_h == 0u || dst_w == 0u || dst_h == 0u)
		return 0;

	src_pixels = (uint64_t)src_w * (uint64_t)src_h;
	dst_pixels = (uint64_t)dst_w * (uint64_t)dst_h;

	if (dst_pixels == 0u)
		return 0;
	if (src_pixels < k_fast_decode_min_src_pixels)
		return 0;

	return src_pixels >= dst_pixels * k_fast_decode_min_ratio;
}
