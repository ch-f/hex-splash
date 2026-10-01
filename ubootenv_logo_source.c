// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

/*
 * U-Boot logo selection logic:
 *
 * +-----------------+----------------------+---------------------+----------------------------------------------+
 * | ttm.logo-custom | ttm.logo-partuuid    | ttm.logo-path       | Result                                       |
 * +-----------------+----------------------+---------------------+----------------------------------------------+
 * | "0"             | any                  | any                 | Disabled: return fallback immediately        |
 * | set (!= "0")    | any                  | any                 | Enabled: use defaults + overrides            |
 * | unset           | set                  | unset               | Enabled: use defaults + overrides            |
 * | unset           | unset                | set                 | Enabled: use defaults + overrides            |
 * | unset           | set                  | set                 | Enabled: use defaults + overrides            |
 * | unset           | unset                | unset               | Disabled: return fallback immediately        |
 * +-----------------+----------------------+---------------------+----------------------------------------------+
 *
 * Defaults used when enabled:
 * - ttm.logo-partuuid: f87c6ad8-eeef-4499-b837-55ec7a7d0489
 * - ttm.logo-path: ttm.logo/logo.png
 */

#include "ubootenv_logo_source.h"
#include "splash_util.h"

#include <ctype.h>
#include <errno.h>
#include <libuboot.h>
#include <limits.h>
#include <mntent.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX UBOOTENV_LOGO_SOURCE_PATH_MAX
#endif

#define HEX_SPLASH_FW_ENV_CONFIG "/etc/fw_env.config"
#define HEX_SPLASH_ENV_BROWSER "ttm.browser"
#define HEX_SPLASH_ENV_LOGO_ENABLE "ttm.logo-custom"
#define HEX_SPLASH_ENV_LOGO_PARTUUID "ttm.logo-partuuid"
#define HEX_SPLASH_ENV_LOGO_PATH "ttm.logo-path"
#define HEX_SPLASH_DEFAULT_LOGO_PARTUUID "f87c6ad8-eeef-4499-b837-55ec7a7d0489"
#define HEX_SPLASH_DEFAULT_LOGO_PATH "ttm.logo/logo.png"

static bool copy_string_checked(char *dst, size_t dst_size, const char *src)
{
	size_t len;

	if (!dst || !src || dst_size == 0)
		return false;

	len = strlen(src);
	if (len >= dst_size)
		return false;

	memcpy(dst, src, len + 1u);
	return true;
}

static void trim_ascii_whitespace_inplace(char *s)
{
	char *start;
	size_t len;

	if (!s)
		return;

	/* leading */
	start = s;
	while (*start != '\0' && isspace((unsigned char)*start))
		start++;
	if (start != s)
		memmove(s, start, strlen(start) + 1u);

	/* trailing */
	len = strlen(s);
	while (len > 0 && isspace((unsigned char)s[len - 1]))
		s[--len] = '\0';
}

static char *env_get_trim(struct uboot_ctx *ctx, const char *key)
{
	char *v = libuboot_get_env(ctx, key);
	if (v)
		trim_ascii_whitespace_inplace(v);
	return v;
}

static bool open_env_context(struct uboot_ctx **ctx_out, bool verbose)
{
	struct uboot_ctx *ctx = NULL;

	if (!ctx_out)
		return false;

	*ctx_out = NULL;

	if (libuboot_initialize(&ctx, NULL) != 0 || !ctx) {
		if (verbose)
			fprintf(stderr, "warning: libubootenv initialize failed; custom logo disabled\n");
		if (ctx)
			libuboot_exit(ctx);
		return false;
	}

	if (libuboot_read_config(ctx, HEX_SPLASH_FW_ENV_CONFIG) != 0) {
		if (verbose)
			fprintf(stderr, "warning: cannot read %s; custom logo disabled\n", HEX_SPLASH_FW_ENV_CONFIG);
		libuboot_exit(ctx);
		return false;
	}

	if (libuboot_open(ctx) != 0) {
		if (verbose)
			fprintf(stderr, "warning: cannot open U-Boot environment; custom logo disabled\n");
		libuboot_exit(ctx);
		return false;
	}

	*ctx_out = ctx;
	return true;
}

static void close_env_context(struct uboot_ctx *ctx)
{
	if (!ctx)
		return;

	libuboot_close(ctx);
	libuboot_exit(ctx);
}

static const char *skip_partuuid_prefix(const char *s)
{
	if (!s)
		return NULL;
	if (strncasecmp(s, "PARTUUID=", 9) == 0)
		return s + 9;
	return s;
}

static bool partuuid_is_sane(const char *s)
{
	const unsigned char *p;

	if (!s || *s == '\0')
		return false;

	for (p = (const unsigned char *)s; *p != '\0'; p++) {
		if (*p == '/')
			return false;
		if (isspace(*p))
			return false;
		if (*p < 0x20 || *p == 0x7f)
			return false;
	}
	return true;
}

static bool build_partuuid_device_path(const char *partuuid, char *out, size_t out_size)
{
	int n;

	if (!partuuid || !out || out_size == 0)
		return false;
	if (!partuuid_is_sane(partuuid))
		return false;

	n = snprintf(out, out_size, "/dev/disk/by-partuuid/%s", partuuid);
	return n >= 0 && (size_t)n < out_size;
}

static bool is_logo_switch_disabled(const char *value)
{
	char *endptr;
	long parsed;

	if (!value)
		return false;

	errno = 0;
	parsed = strtol(value, &endptr, 0);
	if (value == endptr || errno != 0)
		return false;

	while (*endptr != '\0' && isspace((unsigned char)*endptr))
		endptr++;
	if (*endptr != '\0')
		return false;

	return parsed == 0;
}

static unsigned normalize_screen_rotation(long value)
{
	switch (value) {
	case 0:
	case 90:
	case 180:
	case 270:
		return (unsigned)value;
	default:
		return 0u;
	}
}

static unsigned parse_screen_rotation_from_browser_json(const char *json)
{
	static const char needle[] = "\"ScreenRotation\"";
	const char *pos = json;

	if (!json)
		return 0u;

	while ((pos = strcasestr(pos, needle)) != NULL) {
		char *endptr;
		long value;
		const char *cursor = pos + sizeof(needle) - 1u;

		while (*cursor != '\0' && isspace((unsigned char)*cursor))
			cursor++;
		if (*cursor != ':') {
			pos++;
			continue;
		}

		cursor++;
		while (*cursor != '\0' && isspace((unsigned char)*cursor))
			cursor++;

		errno = 0;
		value = strtol(cursor, &endptr, 10);
		if (cursor == endptr || errno != 0) {
			pos++;
			continue;
		}

		while (*endptr != '\0' && isspace((unsigned char)*endptr))
			endptr++;
		if (*endptr != '\0' && *endptr != ',' && *endptr != '}' && *endptr != ']') {
			pos++;
			continue;
		}

		return normalize_screen_rotation(value);
	}

	return 0u;
}

static bool get_block_device_rdev(const char *path, dev_t *out_rdev)
{
	struct stat st;

	if (!path)
		return false;
	if (stat(path, &st) != 0)
		return false;
	if (!S_ISBLK(st.st_mode))
		return false;

	if (out_rdev)
		*out_rdev = st.st_rdev;
	return true;
}

static bool relpath_is_safe(const char *rel)
{
	const char *s;
	const char *seg;

	if (!rel || *rel == '\0')
		return false;

	for (s = rel; *s != '\0'; s++) {
		unsigned char ch = (unsigned char)*s;
		if (ch < 0x20 || ch == 0x7f)
			return false;
		if (ch == '\\')
			return false;
	}

	seg = rel;
	while (*seg != '\0') {
		const char *end;
		size_t len;

		while (*seg == '/')
			seg++;
		if (*seg == '\0')
			break;

		end = seg;
		while (*end != '\0' && *end != '/')
			end++;

		len = (size_t)(end - seg);
		if (len == 1 && seg[0] == '.')
			return false;
		if (len == 2 && seg[0] == '.' && seg[1] == '.')
			return false;

		seg = end;
	}

	return true;
}

static bool resolve_logo_file_path_secure(const char *mount_dir, const char *logo_path,
					 char *out, size_t out_size)
{
	char resolved_mount[PATH_MAX];
	char candidate[UBOOTENV_LOGO_SOURCE_PATH_MAX];
	char resolved_candidate[PATH_MAX];

	const char *rel;
	size_t mlen;
	struct stat st;
	int n;

	if (!mount_dir || !logo_path || !out || out_size == 0)
		return false;

	if (!realpath(mount_dir, resolved_mount))
		return false;

	rel = logo_path;
	while (*rel == '/')
		rel++;
	if (*rel == '\0')
		return false;

	if (!relpath_is_safe(rel))
		return false;

	n = snprintf(candidate, sizeof(candidate), "%s/%s", resolved_mount, rel);
	if (n < 0 || (size_t)n >= sizeof(candidate))
		return false;

	/* realpath() requires existence: missing => fallback */
	if (!realpath(candidate, resolved_candidate))
		return false;

	/* Must stay within mount dir (also blocks symlink escapes) */
	mlen = strlen(resolved_mount);
	if (mlen == 0)
		return false;

	if (strncmp(resolved_candidate, resolved_mount, mlen) != 0)
		return false;

	if (mlen > 1 && resolved_mount[mlen - 1] != '/') {
		char next = resolved_candidate[mlen];
		if (next != '/' && next != '\0')
			return false;
	}

	if (stat(resolved_candidate, &st) != 0)
		return false;
	if (!S_ISREG(st.st_mode))
		return false;

	return copy_string_checked(out, out_size, resolved_candidate);
}

static void load_logo_env_values(char *partuuid, size_t partuuid_size,
				 char *logo_path, size_t logo_path_size,
				 bool *enabled_out)
{
	struct uboot_ctx *ctx = NULL;
	bool seen_enable = false;
	bool seen_partuuid = false;
	bool seen_path = false;

	if (!enabled_out)
		return;

	*enabled_out = false;

	if (!partuuid || partuuid_size == 0 || !logo_path || logo_path_size == 0) {
		*enabled_out = false;
		return;
	}

	if (!copy_string_checked(partuuid, partuuid_size, HEX_SPLASH_DEFAULT_LOGO_PARTUUID) ||
	    !copy_string_checked(logo_path, logo_path_size, HEX_SPLASH_DEFAULT_LOGO_PATH)) {
		partuuid[0] = '\0';
		logo_path[0] = '\0';
		*enabled_out = false;
		return;
	}

	/* If libubootenv isn't usable, disable custom logo and use built-in fallback. */
	if (!open_env_context(&ctx, true))
		return;

	/* ttm.logo-custom */
	{
		char *v = env_get_trim(ctx, HEX_SPLASH_ENV_LOGO_ENABLE);
		if (v) {
			seen_enable = true;
			if (is_logo_switch_disabled(v)) {
				*enabled_out = false;
				free(v);
				goto out;
			}
			*enabled_out = true;
			free(v);
		}
	}

	/* ttm.logo-partuuid */
	{
		char *v = env_get_trim(ctx, HEX_SPLASH_ENV_LOGO_PARTUUID);
		if (v) {
			if (v[0] != '\0') {
				const char *p = skip_partuuid_prefix(v);
				if (p && p[0] != '\0') {
					seen_partuuid = true;
					if (!partuuid_is_sane(p)) {
						fprintf(stderr, "warning: %s contains invalid characters; using default value\n",
							HEX_SPLASH_ENV_LOGO_PARTUUID);
					} else if (!copy_string_checked(partuuid, partuuid_size, p)) {
						fprintf(stderr, "warning: %s too long; using default value\n",
							HEX_SPLASH_ENV_LOGO_PARTUUID);
					}
				}
			}
			free(v);
		}
	}

	/* ttm.logo-path */
	{
		char *v = env_get_trim(ctx, HEX_SPLASH_ENV_LOGO_PATH);
		if (v) {
			if (v[0] != '\0') {
				seen_path = true;
				if (!copy_string_checked(logo_path, logo_path_size, v))
					fprintf(stderr, "warning: %s too long; using default value\n",
						HEX_SPLASH_ENV_LOGO_PATH);
			}
			free(v);
		}
	}

	/* If nothing is set at all, treat as disabled (table behavior). */
	if (!seen_enable && !seen_partuuid && !seen_path) {
		*enabled_out = false;
	} else if (!seen_enable) {
		/* partuuid/path alone implies enabled mode (using defaults + overrides). */
		*enabled_out = true;
	}

out:
	close_env_context(ctx);
}

static bool mount_source_matches_partuuid(const char *source, const char *partuuid, dev_t resolved_rdev)
{
	char resolved_source[PATH_MAX];
	const char *p;
	struct stat st;

	if (!source || !partuuid || partuuid[0] == '\0')
		return false;

	if (strncmp(source, "PARTUUID=", 9) == 0 && strcasecmp(source + 9, partuuid) == 0)
		return true;

	if (source[0] != '/')
		return false;

	p = source;
	if (realpath(source, resolved_source))
		p = resolved_source;

	if (stat(p, &st) == 0 && S_ISBLK(st.st_mode) && st.st_rdev == resolved_rdev)
		return true;

	return false;
}

static bool find_existing_mountpoint_for_partuuid(const char *partuuid, dev_t resolved_rdev,
						  char *mount_dir, size_t mount_dir_size)
{
	FILE *mounts = setmntent("/proc/mounts", "r");
	struct mntent *entry;

	if (!mounts)
		return false;

	while ((entry = getmntent(mounts)) != NULL) {
		if (!mount_source_matches_partuuid(entry->mnt_fsname, partuuid, resolved_rdev))
			continue;
		if (!entry->mnt_dir)
			continue;

		if (!copy_string_checked(mount_dir, mount_dir_size, entry->mnt_dir)) {
			fprintf(stderr, "warning: mountpoint path too long; falling back to built-in logo\n");
			endmntent(mounts);
			return false;
		}

		endmntent(mounts);
		return true;
	}

	endmntent(mounts);
	return false;
}

static bool mount_partuuid_temporary(const char *resolved_device, char *mount_dir, size_t mount_dir_size)
{
	static const char *const fs_types[] = { "ext4", "ext3", "ext2", "f2fs", "vfat", "btrfs", "xfs", "squashfs", "erofs" };
	char mount_template[] = "/tmp/hex-splash-logo-XXXXXX";
	const unsigned long flags = MS_RDONLY | MS_NOATIME | MS_NODEV | MS_NOSUID | MS_NOEXEC;
	char *created_dir;
	int saved_errno = EINVAL;

	if (!resolved_device || !mount_dir || mount_dir_size == 0)
		return false;

	/* Never let a splash failure leave a mount in the parent initramfs.
	 * Making the copied mount tree private also prevents propagation back
	 * to the parent's namespace. Process exit tears down this namespace,
	 * including on fatal signals or an OOM kill. */
	if (unshare(CLONE_NEWNS) != 0 ||
	    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
		fprintf(stderr, "warning: cannot isolate logo mounts: %s; falling back to built-in logo\n",
			strerror(errno));
		return false;
	}

	created_dir = mkdtemp(mount_template);
	if (!created_dir) {
		fprintf(stderr, "warning: mkdtemp failed: %s; falling back to built-in logo\n", strerror(errno));
		return false;
	}

	if (!copy_string_checked(mount_dir, mount_dir_size, created_dir)) {
		rmdir(created_dir);
		fprintf(stderr, "warning: temporary mount path too long; falling back to built-in logo\n");
		return false;
	}

	for (size_t i = 0; i < sizeof(fs_types) / sizeof(fs_types[0]); i++) {
		if (mount(resolved_device, mount_dir, fs_types[i], flags, NULL) == 0)
			return true;
		saved_errno = errno;
	}

	fprintf(stderr, "warning: mount failed for %s: %s; falling back to built-in logo\n",
		resolved_device, strerror(saved_errno));

	rmdir(mount_dir);
	mount_dir[0] = '\0';
	return false;
}

bool ubootenv_logo_source_cleanup(struct ubootenv_logo_source *src)
{
	if (!src)
		return true;

	if (src->is_mounted_by_us && src->mount_dir[0] != '\0') {
		if (umount(src->mount_dir) != 0) {
			fprintf(stderr, "warning: umount(%s) failed: %s\n", src->mount_dir, strerror(errno));
			/* Only our temporary mounts enter this path, and they are private.
			 * Detach a busy mount rather than let it delay the next boot stage. */
			if (umount2(src->mount_dir, MNT_DETACH) != 0) {
				fprintf(stderr, "warning: detach(%s) failed: %s\n", src->mount_dir, strerror(errno));
				return false;
			}
		}
		src->is_mounted_by_us = false;
	}
	if (src->mount_dir[0] != '\0') {
		if (rmdir(src->mount_dir) != 0 && errno != ENOENT) {
			fprintf(stderr, "warning: rmdir(%s) failed: %s\n", src->mount_dir, strerror(errno));
			return false;
		}
	}

	memset(src, 0, sizeof(*src));
	return true;
}

bool ubootenv_logo_source_resolve(struct ubootenv_logo_source *src, char *logo_path, size_t logo_path_size)
{
	char partuuid[128];
	char env_logo_path[UBOOTENV_LOGO_SOURCE_PATH_MAX];
	char partuuid_device[UBOOTENV_LOGO_SOURCE_PATH_MAX];
	char resolved_device[UBOOTENV_LOGO_SOURCE_PATH_MAX];
	char mount_dir[UBOOTENV_LOGO_SOURCE_PATH_MAX];
	bool enabled = true;
	dev_t resolved_rdev;

	if (!src || !logo_path || logo_path_size == 0)
		return false;

	/* Resolve is reuse-safe: clean up any previous temporary mount. */
	logo_path[0] = '\0';
	if (!ubootenv_logo_source_cleanup(src))
		return false;

	load_logo_env_values(partuuid, sizeof(partuuid), env_logo_path, sizeof(env_logo_path), &enabled);
	if (!enabled)
		return false;

	if (partuuid[0] == '\0' || env_logo_path[0] == '\0')
		return false;

	if (!build_partuuid_device_path(partuuid, partuuid_device, sizeof(partuuid_device))) {
		fprintf(stderr, "warning: %s is invalid; falling back to built-in logo\n", HEX_SPLASH_ENV_LOGO_PARTUUID);
		return false;
	}

	if (!realpath(partuuid_device, resolved_device)) {
		fprintf(stderr, "warning: cannot resolve %s: %s; falling back to built-in logo\n",
			partuuid_device, strerror(errno));
		return false;
	}

	if (!get_block_device_rdev(resolved_device, &resolved_rdev)) {
		fprintf(stderr, "warning: resolved logo device is not a block device (%s); falling back to built-in logo\n",
			resolved_device);
		return false;
	}

	if (!find_existing_mountpoint_for_partuuid(partuuid, resolved_rdev, mount_dir, sizeof(mount_dir))) {
		if (!mount_partuuid_temporary(resolved_device, mount_dir, sizeof(mount_dir)))
			return false;

		if (!copy_string_checked(src->mount_dir, sizeof(src->mount_dir), mount_dir)) {
			(void)umount(mount_dir);
			(void)rmdir(mount_dir);
			memset(src, 0, sizeof(*src));
			fprintf(stderr, "warning: mount path too long; falling back to built-in logo\n");
			return false;
		}
		src->is_mounted_by_us = true;
	}

	if (!resolve_logo_file_path_secure(mount_dir, env_logo_path, logo_path, logo_path_size)) {
		fprintf(stderr, "warning: %s did not resolve to a safe existing file; falling back to built-in logo\n",
			HEX_SPLASH_ENV_LOGO_PATH);
		ubootenv_logo_source_cleanup(src);
		logo_path[0] = '\0';
		return false;
	}

	return true;
}

unsigned ubootenv_logo_source_read_screen_rotation(void)
{
	struct uboot_ctx *ctx = NULL;
	char *encoded = NULL;
	char *decoded = NULL;
	unsigned rotation = 0u;

	if (!open_env_context(&ctx, false))
		return 0u;

	encoded = env_get_trim(ctx, HEX_SPLASH_ENV_BROWSER);
	if (!encoded || encoded[0] == '\0')
		goto out;

	if (!splash_decode_base64_string(encoded, &decoded))
		goto out;

	rotation = parse_screen_rotation_from_browser_json(decoded);

out:
	free(decoded);
	free(encoded);
	close_env_context(ctx);
	return rotation;
}
