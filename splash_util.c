// SPDX-License-Identifier: GPL-2.0-only
#include "splash_util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

void splash_die_errno(const char *what)
{
	fprintf(stderr, "%s: %s\n", what, strerror(errno));
	exit(1);
}

void splash_die_msg(const char *what)
{
	fprintf(stderr, "%s\n", what);
	exit(1);
}

int splash_mul_overflow_size_t(size_t a, size_t b, size_t *out)
{
#if defined(__has_builtin)
#if __has_builtin(__builtin_mul_overflow)
	return __builtin_mul_overflow(a, b, out);
#endif
#endif
#if defined(__GNUC__)
	return __builtin_mul_overflow(a, b, out);
#else
	if (a == 0 || b == 0) {
		*out = 0;
		return 0;
	}
	if (a > SIZE_MAX / b)
		return 1;
	*out = a * b;
	return 0;
#endif
}

uint64_t splash_monotonic_millis(void)
{
#if defined(CLOCK_MONOTONIC)
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0u;
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#else
	struct timeval tv;
	if (gettimeofday(&tv, NULL) != 0)
		return 0u;
	return (uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u;
#endif
}

int splash_profile_enabled(void)
{
	const char *v = getenv("HEX_SPLASH_PROFILE");
	return v && v[0] != '\0' && strcmp(v, "0") != 0;
}

uint32_t splash_parse_env_u32(const char *name, uint32_t fallback)
{
	const char *v = getenv(name);
	char *endptr = NULL;
	unsigned long parsed;

	if (!v || *v == '\0')
		return fallback;

	errno = 0;
	parsed = strtoul(v, &endptr, 10);
	if (errno != 0 || endptr == v || *endptr != '\0')
		return fallback;

	if (parsed > 1024ul)
		parsed = 1024ul;

	return (uint32_t)parsed;
}
