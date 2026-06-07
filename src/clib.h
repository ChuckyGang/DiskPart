/*
 * clib.h - Minimal C library declarations for DiskPart.
 *
 * -nostdlib build: this replaces <string.h>, <stdio.h>, <stdlib.h>.
 *
 * strlen/memset/memcpy/memmove: support/gcc8_c_support.c
 * sprintf/snprintf/strcmp/strncpy/strtoul/strtol: src/clib.c
 */

#ifndef CLIB_H
#define CLIB_H

#include <stddef.h>   /* size_t */

extern size_t strlen(const char *s);
extern void  *memset(void *dst, int c, size_t n);
extern void  *memcpy(void *dst, const void *src, size_t n);
extern void  *memmove(void *dst, const void *src, size_t n);
extern int    memcmp (const void *a, const void *b, size_t n);

int   sprintf (char *buf,             const char *fmt, ...);
int   snprintf(char *buf, size_t size, const char *fmt, ...);

/* DP_SNPRINTF(arr, fmt, ...) - snprintf bounded by sizeof(arr).
 * Compile-time guard: if 'arr' is a pointer (not a real array) sizeof would
 * silently be the pointer size and truncate, so DP_IS_ARRAY forces a build
 * error instead.  Use only with in-scope fixed char arrays; for caller-owned
 * pointer buffers keep plain snprintf with the explicit size. */
#define DP_IS_ARRAY(a) \
    (!__builtin_types_compatible_p(__typeof__(a), __typeof__(&(a)[0])))
#define DP_BUILD_BUG_IF(e) \
    ((int)(sizeof(struct { int _dpchk : (1 - 2 * !!(e)); })) * 0)
#define DP_SNPRINTF(arr, ...) \
    snprintf((arr), sizeof(arr) + DP_BUILD_BUG_IF(!DP_IS_ARRAY(arr)), __VA_ARGS__)
int   strcmp(const char *a, const char *b);
char *strncpy(char *dst, const char *src, size_t n);

unsigned long strtoul(const char *s, char **end, int base);
long          strtol (const char *s, char **end, int base);

#endif /* CLIB_H */
