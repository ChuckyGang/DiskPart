/*
 * clib.h — Minimal C library declarations for DiskPart.
 *
 * -nostdlib build: this replaces <string.h>, <stdio.h>, <stdlib.h>.
 *
 * strlen/memset/memcpy/memmove: support/gcc8_c_support.c
 * sprintf/strcmp/strncpy/strtoul/strtol: src/clib.c
 */

#ifndef CLIB_H
#define CLIB_H

#include <stddef.h>   /* size_t */

extern size_t strlen(const char *s);
extern void  *memset(void *dst, int c, size_t n);
extern void  *memcpy(void *dst, const void *src, size_t n);
extern void  *memmove(void *dst, const void *src, size_t n);
extern int    memcmp (const void *a, const void *b, size_t n);

int   sprintf(char *buf, const char *fmt, ...);
int   strcmp(const char *a, const char *b);
char *strncpy(char *dst, const char *src, size_t n);

unsigned long strtoul(const char *s, char **end, int base);
long          strtol (const char *s, char **end, int base);

#endif /* CLIB_H */
