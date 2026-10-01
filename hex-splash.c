// SPDX-License-Identifier: GPL-2.0-only
// hex-splash.c
// Main flow:
// - parse CLI
// - open framebuffer
// - optionally resolve external logo via U-Boot env
// - clear
// - load image (fast-decode when useful), scale if needed, blit centered

#include "splash_framebuffer.h"
#include "splash_image.h"
#include "splash_util.h"
#include "ubootenv_logo_source.h"
#include "config-cmake.h"

#include <getopt.h>
#include <stdio.h>

#if defined(__OPTIMIZE__)
#define HEXSPL_COMPILED_OPTIMIZED 1
#else
#define HEXSPL_COMPILED_OPTIMIZED 0
#endif

#if defined(__OPTIMIZE_SIZE__)
#define HEXSPL_COMPILED_OPT_SIZE 1
#else
#define HEXSPL_COMPILED_OPT_SIZE 0
#endif

#if defined(NDEBUG)
#define HEXSPL_COMPILED_NDEBUG 1
#else
#define HEXSPL_COMPILED_NDEBUG 0
#endif

struct cli_options {
	const char *fb_path;
	const char *png_path;
};

static void usage(FILE *out, const char *argv0)
{
	fprintf(out,
		"%s v%s\n"
		"Usage: %s [-f /dev/fb0] [logo.png]\n"
		"\n"
		"Displays a PNG on the Linux framebuffer, centered, scaled down to fit,\n"
		"with black background. Exits after drawing.\n"
		"\n"
		"If no logo path is provided, the program tries U-Boot env variables:\n"
		"  - ttm.logo-partuuid\n"
		"  - ttm.logo-path\n"
		"and falls back to the built-in logo.\n"
		"Custom logos also honor ttm.browser.ScreenRotation (0/90/180/270).\n"
		"\n"
		"Set ttm.logo-custom=0 to force built-in fallback.\n"
		"\n"
		"Profiling:\n"
		"  HEX_SPLASH_PROFILE=1 prints simple stage timings to stderr.\n"
		"\n"
		"Build:\n"
		"  type=%s target=%s host=%s ptr_size=%d\n"
		"  compile_macros: optimize=%d opt_size=%d ndebug=%d\n",
		HEXSPL_NAME, HEXSPL_VERSION, argv0,
		HEXSPL_BUILD_TYPE, TARGET_SYSTEM, HOST_SYSTEM, HEXSPL_SIZEOF_VOID_P,
		HEXSPL_COMPILED_OPTIMIZED, HEXSPL_COMPILED_OPT_SIZE, HEXSPL_COMPILED_NDEBUG);
}

static int parse_cli(int argc, char **argv, struct cli_options *opts, int *exit_code)
{
	int opt;

	opts->fb_path = "/dev/fb0";
	opts->png_path = NULL;

	while ((opt = getopt(argc, argv, "f:h")) != -1) {
		switch (opt) {
		case 'f':
			opts->fb_path = optarg;
			break;
		case 'h':
			usage(stdout, argv[0]);
			*exit_code = 0;
			return 0;
		default:
			usage(stderr, argv[0]);
			*exit_code = 2;
			return 0;
		}
	}

	if (optind < argc)
		opts->png_path = argv[optind++];

	if (optind < argc) {
		usage(stderr, argv[0]);
		*exit_code = 2;
		return 0;
	}

	*exit_code = 0;
	return 1;
}

static int try_load_fast_logo(const struct splash_framebuffer *fb, const char *png_path,
			      unsigned rotation_degrees, struct splash_image *img)
{
	uint32_t src_w = 0, src_h = 0;
	uint32_t dst_w, dst_h;

	if (!png_path)
		return 0;
	if (rotation_degrees != 0u)
		return 0;
	if (!splash_png_try_probe_dimensions_from_file(png_path, &src_w, &src_h))
		return 0;

	dst_w = src_w;
	dst_h = src_h;
	splash_compute_fit_dimensions(src_w, src_h, fb->vinfo.xres, fb->vinfo.yres, &dst_w, &dst_h);

	/* If no scaling is needed, the normal path is fine. */
	if (dst_w == src_w && dst_h == src_h)
		return 0;

	if (!splash_should_use_fast_decode(src_w, src_h, dst_w, dst_h))
		return 0;

	return splash_png_try_load_nearest_scaled_rgba8_from_file(png_path, dst_w, dst_h, img);
}

static void render_logo(struct splash_framebuffer *fb, const char *png_path, unsigned rotation_degrees)
{
	const int do_profile = splash_profile_enabled();

	uint64_t t0 = 0, t_load = 0, t_scale = 0, t_blit = 0;
	struct splash_image img = { 0 };
	struct splash_image rotated = { 0 };
	struct splash_image scaled = { 0 };
	uint32_t src_w, src_h;
	uint32_t dst_w, dst_h;
	int fast_decode = 0;
	int used_builtin = 0;
	int rotated_alloc = 0;
	int scaled_alloc = 0;

	if (do_profile)
		t0 = splash_monotonic_millis();

	/* Try fast decode only when it will actually help. */
	fast_decode = try_load_fast_logo(fb, png_path, rotation_degrees, &img);

	/* Normal path: load fully (file or built-in), and composite onto black. */
	if (!img.rgba)
		img = splash_load_logo_image(png_path, &used_builtin);

	if (do_profile)
		t_load = splash_monotonic_millis();

	if (!used_builtin && rotation_degrees != 0u) {
		rotated = splash_rotate_rgba(&img, rotation_degrees);
		rotated_alloc = 1;
	} else {
		rotated = img;
	}

	src_w = rotated.w;
	src_h = rotated.h;

	scaled = rotated;
	dst_w = rotated.w;
	dst_h = rotated.h;

	/* If fast decode succeeded, img is already scaled-to-fit. */
	if (!fast_decode) {
		splash_compute_fit_dimensions(rotated.w, rotated.h, fb->vinfo.xres, fb->vinfo.yres, &dst_w, &dst_h);
		if (dst_w != rotated.w || dst_h != rotated.h) {
			scaled = splash_scale_bilinear_rgba(&rotated, dst_w, dst_h);
			scaled_alloc = 1;
		}
	}

	if (do_profile)
		t_scale = splash_monotonic_millis();

	splash_fb_blit_centered(fb->base, fb->vinfo.xres, fb->vinfo.yres, fb->line_length,
				fb->bytes_per_pixel, &fb->vinfo, &scaled);

	if (do_profile)
		t_blit = splash_monotonic_millis();

	if (scaled_alloc)
		splash_image_free(&scaled);
	if (rotated_alloc)
		splash_image_free(&rotated);
	splash_image_free(&img);

	if (do_profile) {
		const unsigned long long load_ms = (unsigned long long)(t_load - t0);
		const unsigned long long scale_ms = (unsigned long long)(t_scale - t_load);
		const unsigned long long blit_ms = (unsigned long long)(t_blit - t_scale);
		const unsigned long long total_ms = (unsigned long long)(t_blit - t0);

		const char *source = used_builtin ? "builtin" : (png_path ? "file" : "builtin");

		fprintf(stderr,
			"profile-render: load=%llums scale=%llums blit=%llums total=%llums "
			"src=%ux%u dst=%ux%u fast=%d source=%s rotation=%u\n",
			load_ms, scale_ms, blit_ms, total_ms,
			src_w, src_h, dst_w, dst_h, fast_decode, source, used_builtin ? 0u : rotation_degrees);
	}
}

int main(int argc, char **argv)
{
	struct cli_options opts;
	struct ubootenv_logo_source external_logo = { 0 };
	struct splash_framebuffer fb = { .fd = -1 };
	char resolved_logo_path[UBOOTENV_LOGO_SOURCE_PATH_MAX];
	const char *selected_png_path = NULL;
	unsigned rotation_degrees = 0u;
	int exit_code = 0;

	const int do_profile = splash_profile_enabled();
	uint64_t t0 = 0, t_resolve = 0, t_fb = 0, t_clear = 0, t_render = 0;

	if (!parse_cli(argc, argv, &opts, &exit_code))
		goto out;

	if (do_profile)
		t0 = splash_monotonic_millis();

	/* No framebuffer means there is no reason to access or mount a logo. */
	exit_code = splash_framebuffer_open(&fb, opts.fb_path);
	if (exit_code)
		goto out;

	if (do_profile)
		t_fb = splash_monotonic_millis();

	/* Select logo: CLI path wins, else try U-Boot env, else built-in (handled in loader). */
	selected_png_path = opts.png_path;
	if (!selected_png_path &&
	    ubootenv_logo_source_resolve(&external_logo, resolved_logo_path, sizeof(resolved_logo_path))) {
		selected_png_path = resolved_logo_path;
	}

	if (selected_png_path)
		rotation_degrees = ubootenv_logo_source_read_screen_rotation();

	if (do_profile)
		t_resolve = splash_monotonic_millis();

	splash_fb_clear_black(fb.base, fb.vinfo.xres, fb.vinfo.yres, fb.line_length, fb.bytes_per_pixel);

	if (do_profile)
		t_clear = splash_monotonic_millis();

	render_logo(&fb, selected_png_path, rotation_degrees);

	if (do_profile) {
		t_render = splash_monotonic_millis();
		fprintf(stderr,
			"profile-main: fb_open=%llums resolve=%llums clear=%llums render=%llums total=%llums\n",
			(unsigned long long)(t_fb - t0),
			(unsigned long long)(t_resolve - t_fb),
			(unsigned long long)(t_clear - t_resolve),
			(unsigned long long)(t_render - t_clear),
			(unsigned long long)(t_render - t0));
	}

out:
	splash_framebuffer_release(&fb);
	if (!ubootenv_logo_source_cleanup(&external_logo))
		exit_code = 1;
	return exit_code;
}
