// SPDX-License-Identifier: GPL-2.0-only
#ifndef SPLASH_UTIL_H
#define SPLASH_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void splash_die_errno(const char *what);
void splash_die_msg(const char *what);

int splash_mul_overflow_size_t(size_t a, size_t b, size_t *out);

/* Monotonic milliseconds (for profiling). */
uint64_t splash_monotonic_millis(void);

/* Non-zero if HEX_SPLASH_PROFILE is set and not "0". */
int splash_profile_enabled(void);

/* Parse env var as u32 (base-10). Returns fallback on errors. */
uint32_t splash_parse_env_u32(const char *name, uint32_t fallback);

/* Decodes a base64 string into a newly allocated NUL-terminated buffer. */
bool splash_decode_base64_string(const char *src, char **decoded_out);

#endif