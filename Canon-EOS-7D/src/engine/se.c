#include "se.h"
#include "se_libc.h"

const char *se_strerror(se_err e)
{
    switch (e) {
    case SE_OK:             return "ok";
    case SE_E_BADFILE:      return "unrecognised file";
    case SE_E_UNSUPPORTED:  return "unsupported variant";
    case SE_E_TRUNCATED:    return "truncated data";
    case SE_E_NOMEM:        return "out of arena memory";
    case SE_E_CORRUPT:      return "corrupt data";
    case SE_E_IO:           return "I/O error";
    default:                return "unknown error";
    }
}

/* ---------------------------------------------------------------- arena */

void se_arena_init(se_arena *a, void *mem, size_t size)
{
    a->base = (uint8_t *)mem;
    a->size = size;
    a->used = 0;
    a->peak = 0;
}

void *se_arena_alloc(se_arena *a, size_t n, size_t align)
{
    size_t off;
    if (align < 1) align = 1;
    off = (a->used + (align - 1)) & ~(align - 1);
    if (n > a->size || off > a->size - n)
        return NULL;
    a->used = off + n;
    if (a->used > a->peak) a->peak = a->used;
    return a->base + off;
}

void se_arena_reset(se_arena *a)
{
    a->used = 0;
}

/* ----------------------------------------------------------- byte reader */

void se_rd_init(se_rd *r, const void *buf, size_t len, int big_endian)
{
    r->p   = (const uint8_t *)buf;
    r->len = len;
    r->pos = 0;
    r->be  = big_endian;
    r->err = 0;
}

void se_rd_seek(se_rd *r, size_t off)
{
    if (off > r->len) { r->err = 1; return; }
    r->pos = off;
}

uint8_t se_rd_u8(se_rd *r)
{
    if (r->pos + 1 > r->len) { r->err = 1; return 0; }
    return r->p[r->pos++];
}

uint16_t se_rd_u16(se_rd *r)
{
    const uint8_t *q;
    if (r->pos + 2 > r->len) { r->err = 1; return 0; }
    q = r->p + r->pos;
    r->pos += 2;
    return r->be ? (uint16_t)((q[0] << 8) | q[1])
                 : (uint16_t)((q[1] << 8) | q[0]);
}

uint32_t se_rd_u32(se_rd *r)
{
    const uint8_t *q;
    if (r->pos + 4 > r->len) { r->err = 1; return 0; }
    q = r->p + r->pos;
    r->pos += 4;
    return r->be ? ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3]
                 : ((uint32_t)q[3] << 24) | ((uint32_t)q[2] << 16) | ((uint32_t)q[1] << 8) | q[0];
}

/* --------------------------------------------------------------- logging */

static se_log_fn      g_log;
static void          *g_log_ctx;
static se_progress_fn g_prog;
static void          *g_prog_ctx;
static se_progress_fn g_fine;
static void          *g_fine_ctx;

void se_set_log(se_log_fn fn, void *ctx) { g_log = fn; g_log_ctx = ctx; }

void se_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    if (!g_log) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    g_log(g_log_ctx, buf);
}

void se_set_progress(se_progress_fn fn, void *ctx) { g_prog = fn; g_prog_ctx = ctx; }

void se_progress(const char *stage, int percent)
{
    if (g_prog) g_prog(g_prog_ctx, stage, percent);
}

void se_set_progress_fine(se_progress_fn fn, void *ctx) { g_fine = fn; g_fine_ctx = ctx; }

void se_progress_fine(const char *stage, int hundredths)
{
    if (g_fine) g_fine(g_fine_ctx, stage, hundredths);
}
