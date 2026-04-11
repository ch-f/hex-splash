// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifndef UBOOTENV_LOGO_SOURCE_PATH_MAX
#define UBOOTENV_LOGO_SOURCE_PATH_MAX 4096
#endif

/*
 * NOTE: Initialize this struct to zero before first use.
 * Example: struct ubootenv_logo_source src = {0};
 */
struct ubootenv_logo_source {
	bool is_mounted_by_us;
	char mount_dir[UBOOTENV_LOGO_SOURCE_PATH_MAX];
};

bool ubootenv_logo_source_resolve(struct ubootenv_logo_source *src, char *logo_path, size_t logo_path_size);
unsigned ubootenv_logo_source_read_screen_rotation(void);
void ubootenv_logo_source_cleanup(struct ubootenv_logo_source *src);