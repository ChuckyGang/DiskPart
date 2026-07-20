/*
 * clib.c - Minimal C library for DiskPart (-nostdlib, Bartman ELF toolchain).
 *
 * Provides: sprintf, strcmp, strncpy, strtoul, strtol.
 * memset/memcpy/memmove/strlen are in support/gcc8_c_support.c.
 */

#include <stdarg.h>
#include <stddef.h>
#include "clib.h"

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    while (n--) {
        if (*pa != *pb) return (int)*pa - (int)*pb;
        pa++; pb++;
    }
    return 0;
}

int strcmp(const char *a, const char *b)
{
    while (*a && (*a == *b)) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    char *d = dst;
    while (n > 0 && *src) { *d++ = *src++; n--; }
    while (n > 0) { *d++ = '\0'; n--; }
    return dst;
}

unsigned long strtoul(const char *s, char **end, int base)
{
    unsigned long val = 0;
    const char   *p   = s;

    while (*p == ' ' || *p == '\t') p++;

    if (base == 0) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
        else if (p[0] == '0')                              { base = 8;  p++;   }
        else                                                  base = 10;
    } else if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }

    while (*p) {
        int digit;
        char c = *p;
        if      (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'A' && c <= 'Z') digit = c - 'A' + 10;
        else if (c >= 'a' && c <= 'z') digit = c - 'a' + 10;
        else break;
        if (digit >= base) break;
        val = val * (unsigned long)base + (unsigned long)digit;
        p++;
    }
    if (end) *end = (char *)p;
    return val;
}

long strtol(const char *s, char **end, int base)
{
    const char *p = s;
    int neg = 0;
    while (*p == ' ' || *p == '\t') p++;
    if      (*p == '-') { neg = 1; p++; }
    else if (*p == '+') {          p++; }
    unsigned long uval = strtoul(p, end, base);
    return neg ? -(long)uval : (long)uval;
}

/* ------------------------------------------------------------------ */
/* sprintf                                                              */
/* Supports: %c %s %d %u %ld %lu %x %X %lx %lX,                      */
/*           width, zero-pad, left-justify (-)                         */
/* ------------------------------------------------------------------ */

static void put_char(char **buf, char c) { **buf = c; (*buf)++; }

static int fmt_ulong(char *tmp, unsigned long val, int base, int upper)
{
    const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;
    if (val == 0) { tmp[i++] = '0'; }
    else { while (val) { tmp[i++] = digs[val % (unsigned long)base]; val /= (unsigned long)base; } }
    int len = i;
    for (int a = 0, b = i - 1; a < b; a++, b--) { char t = tmp[a]; tmp[a] = tmp[b]; tmp[b] = t; }
    tmp[len] = '\0';
    return len;
}

/* Internal bounded formatter shared by sprintf and snprintf.
   size == 0 means unbounded (sprintf legacy behaviour). */
static int do_fmt(char *buf, size_t size, const char *fmt, va_list ap)
{
    char *out  = buf;
    char *end  = (size > 0) ? buf + size - 1 : (char *)~(size_t)0;
    const char *f = fmt;

#define PUT(c) do { if (out < end) *out++ = (c); } while(0)

    while (*f) {
        /* Advance f UNCONDITIONALLY: PUT() only writes when out < end, so
           putting *f++ directly inside PUT would stop advancing f once the
           buffer fills (the increment lives inside PUT's `if`), leaving
           `while (*f)` spinning forever whenever the output exceeds the
           buffer.  Latent since this formatter was written - never hit
           because every caller sized its buffer to fit until now. */
        if (*f != '%') { char litc = *f++; PUT(litc); continue; }
        f++;

        int left = 0;
        char fill = ' ';
        if (*f == '-') { left = 1; f++; }
        if (*f == '0' && !left) { fill = '0'; f++; }

        int width = 0;
        while (*f >= '0' && *f <= '9') { width = width * 10 + (*f - '0'); f++; }

        int is_long = 0;
        if (*f == 'l') { is_long = 1; f++; }

        char spec = *f++;

        if (spec == '%') { PUT('%'); continue; }
        if (spec == 'c') { PUT((char)va_arg(ap, int)); continue; }

        if (spec == 's') {
            const char *sv = va_arg(ap, const char *);
            int slen, i;
            if (!sv) sv = "(null)";
            slen = 0; { const char *p = sv; while (*p++) slen++; }
            if (!left) for (i = slen; i < width; i++) PUT(' ');
            while (*sv) PUT(*sv++);
            if ( left) for (i = slen; i < width; i++) PUT(' ');
            continue;
        }

        char tmp[32];
        int len = 0, neg = 0;

        if (spec == 'd') {
            long sv = is_long ? va_arg(ap, long) : (long)va_arg(ap, int);
            if (sv < 0) { neg = 1; sv = -sv; }
            len = fmt_ulong(tmp, (unsigned long)sv, 10, 0);
        } else if (spec == 'u') {
            unsigned long uv = is_long ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
            len = fmt_ulong(tmp, uv, 10, 0);
        } else if (spec == 'x') {
            unsigned long uv = is_long ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
            len = fmt_ulong(tmp, uv, 16, 0);
        } else if (spec == 'X') {
            unsigned long uv = is_long ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
            len = fmt_ulong(tmp, uv, 16, 1);
        } else {
            PUT('%'); PUT(spec); continue;
        }

        {
            int total = len + (neg ? 1 : 0);
            int i;
            if (!left) {
                if (neg && fill == '0') PUT('-');
                for (i = total; i < width; i++) PUT(fill);
                if (neg && fill != '0') PUT('-');
            } else {
                if (neg) PUT('-');
            }
            for (i = 0; i < len; i++) PUT(tmp[i]);
            if (left) for (i = total; i < width; i++) PUT(' ');
        }
    }

#undef PUT
    *out = '\0';
    return (int)(out - buf);
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = do_fmt(buf, 0, fmt, ap);
    va_end(ap);
    return n;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = do_fmt(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
