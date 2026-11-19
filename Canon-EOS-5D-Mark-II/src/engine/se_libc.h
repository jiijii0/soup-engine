/*
 * se_libc.h - the entire C library surface the engine uses.
 *
 * Deliberately tiny. On the host these resolve to the system libc; in the
 * camera build they resolve to Magic Lantern's own implementations, which
 * is why the engine must never reach for anything beyond this list.
 *
 * Define SE_HOSTED for the host build.
 */
#ifndef SE_LIBC_H
#define SE_LIBC_H

#include <stddef.h>
#include <stdarg.h>   /* a compiler header, available freestanding */

#ifdef SE_HOSTED
#  include <string.h>
#  include <stdio.h>
#else
void  *memcpy (void *dst, const void *src, size_t n);
void  *memset (void *dst, int c, size_t n);
size_t strlen (const char *s);
int    strcmp (const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
#endif

#endif /* SE_LIBC_H */
