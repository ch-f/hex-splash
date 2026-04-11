// SPDX-License-Identifier: GPL-2.0-only
#include "splash_util.h"

#include <ctype.h>
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

static int base64_decode_value(unsigned char ch)
{
	if (ch >= 'A' && ch <= 'Z')
		return (int)(ch - 'A');
	if (ch >= 'a' && ch <= 'z')
		return (int)(ch - 'a') + 26;
	if (ch >= '0' && ch <= '9')
		return (int)(ch - '0') + 52;
	if (ch == '+')
		return 62;
	if (ch == '/')
		return 63;
	return -1;
}

bool splash_decode_base64_string(const char *src, char **decoded_out)
{
	size_t src_len;
	size_t capacity;
	size_t out_len = 0;
	unsigned char quartet[4];
	size_t quartet_len = 0;
	bool finished = false;
	char *decoded;

	if (!src || !decoded_out)
		return false;

	*decoded_out = NULL;
	src_len = strlen(src);
	capacity = (src_len / 4u) * 3u + 3u;

	decoded = (char *)malloc(capacity + 1u);
	if (!decoded)
		return false;

	for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; p++) {
		int value;

		if (isspace(*p))
			continue;

		if (finished)
			goto fail;

		if (*p == '=') {
			quartet[quartet_len++] = 0xFFu;
		} else {
			value = base64_decode_value(*p);
			if (value < 0)
				goto fail;
			quartet[quartet_len++] = (unsigned char)value;
		}

		if (quartet_len != 4u)
			continue;

		if (quartet[0] == 0xFFu || quartet[1] == 0xFFu)
			goto fail;

		decoded[out_len++] = (char)((quartet[0] << 2) | (quartet[1] >> 4));

		if (quartet[2] == 0xFFu) {
			if (quartet[3] != 0xFFu)
				goto fail;
			finished = true;
		} else {
			decoded[out_len++] = (char)((quartet[1] << 4) | (quartet[2] >> 2));
			if (quartet[3] == 0xFFu) {
				finished = true;
			} else {
				decoded[out_len++] = (char)((quartet[2] << 6) | quartet[3]);
			}
		}

		quartet_len = 0u;
	}

	if (quartet_len != 0u) {
		if (finished || quartet_len == 1u || quartet[0] == 0xFFu || quartet[1] == 0xFFu)
			goto fail;

		decoded[out_len++] = (char)((quartet[0] << 2) | (quartet[1] >> 4));
		if (quartet_len == 3u) {
			if (quartet[2] == 0xFFu)
				goto fail;
			decoded[out_len++] = (char)((quartet[1] << 4) | (quartet[2] >> 2));
		} else if (quartet_len != 2u) {
			goto fail;
		}
	}

	decoded[out_len] = '\0';
	*decoded_out = decoded;
	return true;

fail:
	free(decoded);
	return false;
}
