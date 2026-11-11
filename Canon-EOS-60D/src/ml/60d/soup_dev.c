/** \file
 * soup_dev.c - develop one frame into a film JPEG.
 *
 * Kept apart from the menu and the watcher so the same file builds on the
 * desktop (with SOUP_HOST, against soup_port.h's stdio stand-ins) and is
 * tested there before it goes near a camera. See host_test.c beside it.
 *
 * GPL v2.
 */
#include "soup_port.h"
#include "soup_dev.h"

#include "se.h"
#include "cr2.h"
#include "stock.h"
#include "look.h"
#include "jpeg_dec.h"
#include "jpeg_enc.h"

/* Read [off, off+n) of an open file into `dst` (cacheable), through a DMA
 * bounce buffer. */
static int soup_read_at(FILE *h, uint32_t off, uint8_t *dst, uint32_t n,
                        uint8_t *dma, uint32_t dmacap)
{
    uint32_t done = 0;
    if (FIO_SeekSkipFile(h, off, SEEK_SET) < 0) return -1;
    while (done < n) {
        uint32_t want = n - done;
        int k;
        if (want > dmacap) want = dmacap;
        k = FIO_ReadFile(h, dma, want);
        if (k <= 0) return -1;
        memcpy(dst + done, dma, (size_t)k);
        done += (uint32_t)k;
    }
    return 0;
}

/* ---------------------------------------------------------- develop ---- */

/* Feeds the streaming decoder: offsets are relative to the JPEG start. */
typedef struct { FILE *h; uint32_t base; uint8_t *dma; uint32_t dmacap; } soup_stream;

static int soup_stream_read(void *ctx, uint32_t off, void *dst, uint32_t n)
{
    soup_stream *s = (soup_stream *)ctx;
    return soup_read_at(s->h, s->base + off, (uint8_t *)dst, n, s->dma, s->dmacap);
}

/* Can this JPEG be streamed? Needs no DRI (restart markers), and every
 * marker up to SOS inside the bytes we already hold. */
static int soup_streamable(const uint8_t *j, uint32_t len)
{
    uint32_t p = 2;
    if (len < 4 || j[0] != 0xff || j[1] != 0xd8) return 0;
    while (p + 4 <= len) {
        int m, l;
        if (j[p] != 0xff) return 0;
        while (p < len && j[p] == 0xff) p++;
        if (p + 3 > len) return 0;
        m = j[p++];
        if (m == 0xd8 || m == 0x01 || (m >= 0xd0 && m <= 0xd7)) continue;
        l = (j[p] << 8) | j[p + 1];
        if (m == 0xdd) return 0;             /* DRI: restart markers */
        if (m == 0xda) return p + (uint32_t)l <= len;   /* SOS reached */
        if (l < 2) return 0;
        p += (uint32_t)l;
    }
    return 0;
}

/* Output is written to the card in chunks as the encoder produces it, from
 * two alternating buffers so a write never reads a buffer being refilled. */
typedef struct {
    FILE    *h;
    uint8_t *buf[2];
    int      cur;
    int      len;
    int      bytes;
    int      err;
} soup_out;

static int soup_out_flush(soup_out *o)
{
    int n = o->len;
    if (!n || o->err) return o->err ? -1 : 0;
    if (FIO_WriteFile(o->h, o->buf[o->cur], (size_t)n) != n) {
        o->err = 1;
        return -1;
    }
    o->bytes += n;
    o->len = 0;
    o->cur ^= 1;
    return 0;
}

static int soup_out_sink(void *ctx, const uint8_t *data, size_t len)
{
    soup_out *o = (soup_out *)ctx;
    size_t done = 0;
    if (o->err) return -1;
    while (done < len) {
        int room = FILM_OUTCHUNK - o->len;
        int take = (int)(len - done);
        if (take > room) take = room;
        memcpy(o->buf[o->cur] + o->len, data + done, (size_t)take);
        o->len += take;
        done += (size_t)take;
        if (o->len == FILM_OUTCHUNK && soup_out_flush(o) != 0) return -1;
    }
    return 0;
}

typedef struct {
    se_jpeg     *enc;
    se_look_run *run;
    int          ow;
    int          err;
} soup_look;

static int soup_emit(void *ctx, const uint8_t *row, int width)
{
    soup_look *L = (soup_look *)ctx;
    if (width != L->ow) { L->err = 1; return -1; }
    if (se_jpeg_write_rows(L->enc, row, 1) != SE_OK) { L->err = 1; return -1; }
    return 0;
}

static int soup_rows(void *ctx, const uint8_t *rgb, int nrows, int width)
{
    soup_look *L = (soup_look *)ctx;
    if (width != L->ow) { L->err = 1; return -1; }
    if (se_look_rows(L->run, rgb, nrows, soup_emit, L) != SE_OK) {
        L->err = 1;
        return -1;
    }
    return 0;
}

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* The 160x120 JPEG thumbnail Canon stores in CR2 IFD1. It is the
 * un-filmed picture, but it beats a grey square in the playback index. */
/* Where a JPEG's EXIF TIFF block starts (after APP1 "Exif\0\0"), or 0. */
static uint32_t soup_jpeg_exif(const uint8_t *j, int got, uint32_t *len)
{
    int p = 2;
    *len = 0;
    if (got < 4 || j[0] != 0xFF || j[1] != 0xD8) return 0;
    while (p + 4 <= got && j[p] == 0xFF) {
        int m = j[p + 1];
        int l = (j[p + 2] << 8) | j[p + 3];
        if (m == 0xDA || m == 0xD9 || l < 2) break;
        if (m == 0xE1 && l >= 8 && p + 10 <= got &&
            !memcmp(j + p + 4, "Exif\0\0", 6)) {
            uint32_t t = (uint32_t)p + 10;
            uint32_t n = (uint32_t)l - 8;
            if (t + n > (uint32_t)got) n = (uint32_t)got - t;
            *len = n;
            return t;
        }
        p += 2 + l;
    }
    return 0;
}

static int soup_cr2_thumb(const uint8_t *hdr, int got, uint32_t *off, uint32_t *len)
{
    uint32_t ifd0, ifd1;
    int n, i;
    *off = *len = 0;
    if (got < 16 || hdr[0] != 'I' || hdr[1] != 'I') return -1;
    ifd0 = rd32le(hdr + 4);
    if (ifd0 + 2 > (uint32_t)got) return -1;
    n = hdr[ifd0] | (hdr[ifd0 + 1] << 8);
    if (ifd0 + 2 + (uint32_t)n * 12 + 4 > (uint32_t)got) return -1;
    ifd1 = rd32le(hdr + ifd0 + 2 + n * 12);
    if (!ifd1 || ifd1 + 2 > (uint32_t)got) return -1;
    n = hdr[ifd1] | (hdr[ifd1 + 1] << 8);
    if (n < 1 || n > 64 || ifd1 + 2 + (uint32_t)n * 12 > (uint32_t)got) return -1;
    for (i = 0; i < n; i++) {
        const uint8_t *e = hdr + ifd1 + 2 + i * 12;
        int tag = e[0] | (e[1] << 8);
        if (tag == 0x201) *off = rd32le(e + 8);
        else if (tag == 0x202) *len = rd32le(e + 8);
    }
    if (!*off || !*len || *len > 32 * 1024) return -1;
    if (*off + *len > (uint32_t)got) return -1;
    return 0;
}

/*
 * Develop one frame. `src` is a CR2 (is_cr2 = 1) or a plain JPEG. The
 * result is written to `tmp`; the caller renames it into place.
 */
int soup_develop(const char *src, int is_cr2, const char *tmp,
                 const se_stock *stock, se_progress_fn progress)
{
    uint8_t *hdr = 0, *dma = 0, *block = 0, *thumb = 0;
    se_arena arena, lookar;
    se_look lk;
    se_cr2_info info;
    se_jdec_info ji;
    se_jpeg_exif ex;
    se_jpeg *enc;
    se_look_run *run;
    soup_look L;
    soup_out out;
    FILE *h = 0;
    size_t lookneed;
    uint32_t poff = 0, plen = 0, toff = 0, tlen = 0, fsize = 0;
    int pw = 0, ph = 0, ow, oh, got = 0, stream;
    se_err rc = SE_E_IO;

    memset(&out, 0, sizeof out);
    memset(&L, 0, sizeof L);
    memset(&info, 0, sizeof info);

    /* --- find the JPEG to develop ------------------------------------- */
    if (FIO_GetFileSize(src, &fsize) != 0 || fsize == 0) return SE_E_IO;
    dma = fio_malloc(FILM_HDR);
    hdr = malloc(FILM_HDR);
    if (!dma || !hdr) { rc = SE_E_NOMEM; goto done; }
    h = FIO_OpenFile(src, O_RDONLY);
    if (!h) { rc = SE_E_IO; goto done; }
    got = fsize < FILM_HDR ? (int)fsize : FILM_HDR;
    if (soup_read_at(h, 0, hdr, (uint32_t)got, dma, FILM_HDR) != 0) {
        rc = SE_E_IO; goto done;
    }

    if (is_cr2) {
        rc = se_cr2_find_preview(hdr, (size_t)got, &poff, &plen, &pw, &ph);
        if (rc != SE_OK) { soup_log("%s: no preview (rc=%d)", src, rc); goto done; }
        if (se_cr2_parse(hdr, (size_t)got, &info) != SE_OK)
            memset(&info, 0, sizeof info);
        if (soup_cr2_thumb(hdr, got, &toff, &tlen) == 0) {
            thumb = malloc(tlen);
            if (thumb) memcpy(thumb, hdr + toff, tlen);
        }
    } else {
        /* A camera JPEG: carry its shot details and thumbnail across. */
        uint32_t elen = 0, et = soup_jpeg_exif(hdr, got, &elen);
        poff = 0;
        plen = fsize;
        if (et && se_exif_parse(hdr + et, elen, &info) != SE_OK)
            memset(&info, 0, sizeof info);
        if (et && soup_cr2_thumb(hdr + et, (int)elen, &toff, &tlen) == 0) {
            thumb = malloc(tlen);
            if (thumb) memcpy(thumb, hdr + et + toff, tlen);
        }
    }
    if (poff >= (uint32_t)got || poff + plen > fsize) { rc = SE_E_BADFILE; goto done; }
    if (se_jpeg_dec_info(hdr + poff, (size_t)got - poff, &ji) != SE_OK) {
        soup_log("%s: preview header not understood", src);
        rc = SE_E_BADFILE; goto done;
    }

    /* --- streamed, or whole ------------------------------------------- */
    /*
     * Streamed when we can: the decoder pulls the JPEG through an 8 KB
     * window, so the biggest allocation stays around a megabyte whatever
     * the file size. Canon's CR2 previews carry no restart markers, which
     * is what streaming needs. A JPEG that has them (or whose headers do
     * not fit in the first 256 KB) is read whole into memory instead.
     */
    stream = soup_streamable(hdr + poff, (uint32_t)got - poff);
    if (!stream) {
        uint8_t *whole = malloc(plen);
        if (!whole) {
            soup_log("%s: no memory for %u byte JPEG", src, (unsigned)plen);
            rc = SE_E_NOMEM; goto done;
        }
        if (soup_read_at(h, poff, whole, plen, dma, FILM_HDR) != 0) {
            free(whole); rc = SE_E_IO; goto done;
        }
        free(hdr); hdr = whole; poff = 0;
        FIO_CloseFile(h); h = 0;
        fio_free(dma); dma = 0;
    }

    /* --- working memory ------------------------------------------------ */
    se_jpeg_scaled_size(ji.width, ji.height, FILM_SCALE, &ow, &oh);
    se_look_from_stock(stock, FILM_SCALE, &lk);
    lookneed = se_look_fit(&lk, ow, FILM_LOOK_MAX);

    block = malloc(FILM_SCRATCH + lookneed + 2 * FILM_OUTCHUNK + 64);
    if (!block) {
        soup_log("%s: no memory for working buffers", src);
        rc = SE_E_NOMEM; goto done;
    }
    out.buf[0] = block;
    out.buf[1] = block + FILM_OUTCHUNK;
    se_arena_init(&arena, block + 2 * FILM_OUTCHUNK, FILM_SCRATCH);
    se_arena_init(&lookar, block + 2 * FILM_OUTCHUNK + FILM_SCRATCH, lookneed);

    enc = SE_NEW(&arena, se_jpeg, 1);
    if (!enc) { rc = SE_E_NOMEM; goto done; }
    rc = se_look_begin(&run, &lk, ow, &lookar);
    if (rc != SE_OK) goto done;
    L.run = run;
    L.ow  = ow;

    memset(&ex, 0, sizeof ex);
    ex.make          = info.make[0] ? info.make : "Canon";
    ex.model         = info.model[0] ? info.model : "Canon EOS 60D";
    ex.software      = stock->name;
    ex.datetime      = info.datetime[0] ? info.datetime : 0;
    ex.exposure_time = info.exposure_time;
    ex.aperture      = info.aperture;
    ex.iso           = info.iso;
    if (thumb) { ex.thumb = thumb; ex.thumb_len = (int)tlen; }

    /* --- develop ------------------------------------------------------- */
    out.h = FIO_CreateFile(tmp);
    if (!out.h) { soup_log("cannot create %s", tmp); rc = SE_E_IO; goto done; }
    {
        uint8_t *strip = SE_NEW(&arena, uint8_t, (size_t)8 * ow * 3);
        if (!strip) { rc = SE_E_NOMEM; goto done; }
        rc = se_jpeg_begin(enc, ow, oh, FILM_QUALITY, strip, &ex, soup_out_sink, &out);
        if (rc != SE_OK) goto done;
    }
    L.enc = enc;

    se_set_progress(progress, 0);
    if (stream) {
        soup_stream sr;
        sr.h = h; sr.base = poff; sr.dma = dma; sr.dmacap = FILM_HDR;
        rc = se_jpeg_decode_stream_scaled(hdr + poff, (size_t)got - poff, plen,
                                          soup_stream_read, &sr, FILM_SCALE,
                                          &arena, soup_rows, &L, &ji);
    } else {
        rc = se_jpeg_decode_scaled(hdr, plen, FILM_SCALE, &arena, soup_rows, &L, &ji);
    }
    if (rc == SE_OK && !L.err) rc = se_look_end(L.run, soup_emit, &L);
    if (rc == SE_OK && !L.err) rc = se_jpeg_end(enc);
    if (rc == SE_OK && L.err) rc = SE_E_CORRUPT;
    if (rc == SE_OK && soup_out_flush(&out) != 0) rc = SE_E_IO;
    if (out.err) rc = SE_E_IO;
    if (rc == SE_OK)
        soup_log("%s -> %dx%d, %d KB, %s, arena peak %d KB", src, ow, oh,
                 out.bytes >> 10, stream ? "streamed" : "whole",
                 (int)(arena.peak >> 10));
    else
        soup_log("%s: develop failed rc=%d (%s)", src, rc, se_strerror(rc));

done:
    se_set_progress(0, 0);
    if (out.h) FIO_CloseFile(out.h);
    if (h) FIO_CloseFile(h);
    if (dma) fio_free(dma);
    if (hdr) free(hdr);
    if (block) free(block);
    if (thumb) free(thumb);
    return rc;
}


/* ------------------------------------------------------------- view ---- */

/*
 * Paint a film JPEG into a 4:2:2 picture buffer (the camera's playback
 * image, 720x480 UYVY on the 60D), fitted and centred, for the playback
 * toggle. Half-size decode, then nearest-neighbour down to the screen.
 * Chroma is signed (DIGIC 4), Rec.709 like the rest of the 60D display.
 */
typedef struct {
    uint8_t *buf; int pitch, W, H;      /* destination                    */
    int sw, sh;                         /* delivered source size          */
    int ox, oy, dw, dh;                 /* where it lands                 */
    int srow, trow;                     /* next source row, target row    */
    volatile int *abort;
} soup_view;

static inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static inline int clamps8(int v)  { return v < -128 ? -128 : v > 127 ? 127 : v; }

static void view_put_row(soup_view *v, const uint8_t *rgb, int ty)
{
    uint32_t *d = (uint32_t *)(v->buf + (v->oy + ty) * v->pitch + v->ox * 2);
    int tx;
    for (tx = 0; tx + 1 < v->dw; tx += 2) {
        const uint8_t *p0 = rgb + 3 * ((tx * v->sw) / v->dw);
        const uint8_t *p1 = rgb + 3 * (((tx + 1) * v->sw) / v->dw);
        int r = (p0[0] + p1[0]) >> 1, g = (p0[1] + p1[1]) >> 1, b = (p0[2] + p1[2]) >> 1;
        int y0 = clamp255((217 * p0[0] + 732 * p0[1] + 73 * p0[2]) >> 10);
        int y1 = clamp255((217 * p1[0] + 732 * p1[1] + 73 * p1[2]) >> 10);
        int u  = clamps8((-117 * r - 394 * g + 512 * b) / 1024);
        int vv = clamps8((512 * r - 465 * g - 46 * b) / 1024);
        *d++ = (uint32_t)(u & 0xFF) | ((uint32_t)y0 << 8) |
               ((uint32_t)(vv & 0xFF) << 16) | ((uint32_t)y1 << 24);
    }
}

static int view_rows(void *ctx, const uint8_t *rgb, int nrows, int width)
{
    soup_view *v = (soup_view *)ctx;
    int i;
    if (v->abort && *v->abort) return -1;
    if (width != v->sw) return -1;
    for (i = 0; i < nrows; i++, v->srow++) {
        const uint8_t *row = rgb + (size_t)i * width * 3;
        /* every target row whose source is this row */
        while (v->trow < v->dh && (v->trow * v->sh) / v->dh == v->srow) {
            view_put_row(v, row, v->trow);
            v->trow++;
        }
    }
    return 0;
}

int soup_view_flm(const char *path, uint8_t *buf, int pitch, int W, int H,
                  volatile int *abort)
{
    uint8_t *hdr = 0, *dma = 0, *block = 0;
    se_arena arena;
    se_jdec_info ji;
    soup_view v;
    FILE *h = 0;
    uint32_t fsize = 0;
    int got, scale, ow, oh, y;
    se_err rc = SE_E_IO;

    if (FIO_GetFileSize(path, &fsize) != 0 || fsize == 0) return SE_E_IO;
    dma = fio_malloc(FILM_HDR);
    hdr = malloc(FILM_HDR);
    if (!dma || !hdr) { rc = SE_E_NOMEM; goto done; }
    h = FIO_OpenFile(path, O_RDONLY);
    if (!h) goto done;
    got = fsize < FILM_HDR ? (int)fsize : FILM_HDR;
    if (soup_read_at(h, 0, hdr, (uint32_t)got, dma, FILM_HDR) != 0) goto done;
    if (se_jpeg_dec_info(hdr, (size_t)got, &ji) != SE_OK) { rc = SE_E_BADFILE; goto done; }
    if (!soup_streamable(hdr, (uint32_t)got)) { rc = SE_E_UNSUPPORTED; goto done; }

    scale = ji.width >= 2 * W ? 2 : 1;
    se_jpeg_scaled_size(ji.width, ji.height, scale, &ow, &oh);

    memset(&v, 0, sizeof v);
    v.buf = buf; v.pitch = pitch; v.W = W; v.H = H;
    v.sw = ow; v.sh = oh; v.abort = abort;
    /* fit inside W x H, keep the aspect */
    if ((long)ow * H >= (long)oh * W) { v.dw = W; v.dh = (int)((long)oh * W / ow); }
    else                              { v.dh = H; v.dw = (int)((long)ow * H / oh); }
    v.dw &= ~1;
    v.ox = ((W - v.dw) / 2) & ~1;
    v.oy = (H - v.dh) / 2;

    /* black surround (Y 0, no colour) */
    for (y = 0; y < H; y++) {
        if (y >= v.oy && y < v.oy + v.dh) {
            memset(buf + y * pitch, 0, (size_t)v.ox * 2);
            memset(buf + y * pitch + (v.ox + v.dw) * 2, 0, (size_t)(W - v.ox - v.dw) * 2);
        } else {
            memset(buf + y * pitch, 0, (size_t)W * 2);
        }
    }

    block = malloc(FILM_SCRATCH);
    if (!block) { rc = SE_E_NOMEM; goto done; }
    se_arena_init(&arena, block, FILM_SCRATCH);
    {
        soup_stream sr;
        sr.h = h; sr.base = 0; sr.dma = dma; sr.dmacap = FILM_HDR;
        rc = se_jpeg_decode_stream_scaled(hdr, (size_t)got, fsize, soup_stream_read,
                                          &sr, scale, &arena, view_rows, &v, &ji);
    }
    if (rc != SE_OK && abort && *abort) rc = SOUP_VIEW_CANCELLED;

done:
    if (h) FIO_CloseFile(h);
    if (block) free(block);
    if (hdr) free(hdr);
    if (dma) fio_free(dma);
    return rc;
}
