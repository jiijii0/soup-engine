/*
 * fe.h - Soup Engine common types.
 *
 * The engine is freestanding C99: no stdio, no malloc, no host libc beyond
 * a handful of mem* routines. Everything allocates from a caller-supplied
 * arena so the same code runs under macOS and under VxWorks on the 5D.
 */
#ifndef SE_H
#define SE_H

#include <stdint.h>
#include <stddef.h>

typedef int se_err;

#define SE_OK                0
#define SE_E_BADFILE        -1   /* not a file we recognise            */
#define SE_E_UNSUPPORTED    -2   /* recognised, but a variant we can't */
#define SE_E_TRUNCATED      -3   /* ran off the end of the buffer      */
#define SE_E_NOMEM          -4   /* arena exhausted                    */
#define SE_E_CORRUPT        -5   /* internal inconsistency in the data */
#define SE_E_IO             -6   /* host/camera I/O failed             */

const char *se_strerror(se_err e);

/* ---------------------------------------------------------------- arena */
/*
 * Bump allocator. No free(); the whole arena is reset per image. On the
 * camera this will be carved out of a single large AllocateMemory() block,
 * which is the only way to stay sane with ~20 MB of raw in flight.
 */
typedef struct {
    uint8_t *base;
    size_t   size;
    size_t   used;
    size_t   peak;
} se_arena;

void  se_arena_init(se_arena *a, void *mem, size_t size);
void *se_arena_alloc(se_arena *a, size_t n, size_t align);
void  se_arena_reset(se_arena *a);

#define SE_NEW(a, type, count) \
    ((type *)se_arena_alloc((a), sizeof(type) * (size_t)(count), sizeof(type) > 8 ? 8 : sizeof(type)))

/* ----------------------------------------------------------- byte reader */
typedef struct {
    const uint8_t *p;      /* start of buffer   */
    size_t         len;    /* total length      */
    size_t         pos;    /* cursor            */
    int            be;     /* 1 = big endian    */
    int            err;    /* sticky overrun    */
} se_rd;

void     se_rd_init(se_rd *r, const void *buf, size_t len, int big_endian);
void     se_rd_seek(se_rd *r, size_t off);
uint8_t  se_rd_u8 (se_rd *r);
uint16_t se_rd_u16(se_rd *r);
uint32_t se_rd_u32(se_rd *r);

/* --------------------------------------------------------------- logging */
/*
 * The engine never prints. It calls this hook, which the host CLI wires to
 * stderr and the camera build wires to the ML console / bmp_printf.
 */
typedef void (*se_log_fn)(void *ctx, const char *msg);
void se_set_log(se_log_fn fn, void *ctx);
void se_log(const char *fmt, ...);

/* Progress reporting, 0..100. Camera build draws a bar; host prints dots. */
typedef void (*se_progress_fn)(void *ctx, const char *stage, int percent);
void se_set_progress(se_progress_fn fn, void *ctx);
void se_progress(const char *stage, int percent);

/* Same callback, value is hundredths of a percent: 4371 means 43.71%. */
void se_set_progress_fine(se_progress_fn fn, void *ctx);
void se_progress_fine(const char *stage, int hundredths);

/* ----------------------------------------------------------------- utils */
#define SE_MIN(a, b) ((a) < (b) ? (a) : (b))
#define SE_MAX(a, b) ((a) > (b) ? (a) : (b))
#define SE_CLAMP(x, lo, hi) SE_MIN(SE_MAX((x), (lo)), (hi))
#define SE_ARRAY_LEN(x) (sizeof(x) / sizeof((x)[0]))

#endif /* SE_H */
