/*
 * develop.c - one frame, from the preview Canon embedded in the raw.
 *
 * Find the full-size JPEG in the CR2, decode it at the asked-for scale,
 * run the look over it, encode beside the raw. It is the 5D's
 * develop_preview with every Canon call replaced by Magic Lantern's, and
 * with the preview held whole rather than streamed, because this body
 * can afford the 3 MB and the 5D could not. The FNV hash of every row
 * that reaches the encoder is logged; tools/previewcheck prints the same
 * number from the same file, and tools/verify-700d.py compares the files.
 */
#include "soupeng.h"

const char *se_stage = "";
int         se_pct;
int         se_waiting;

/*
 * The highest-numbered .CR2 in the shooting folder, for "Develop last
 * shot". The watcher does not use this: it knows the number of each new
 * frame from Canon's counter and never walks the directory.
 */
int se_newest_cr2(char *out, size_t cap)
{
    const char *dir = get_dcim_dir();
    struct fio_file *file = alloc_fio_file();
    struct fio_dirent *d;
    char best[FIO_MAX_PATH_LENGTH];

    best[0] = 0;
    if (!file) return -1;
    d = FIO_FindFirstEx(dir, file);
    if (IS_ERROR(d)) { free(file); return -1; }
    do {
        struct file_info fi = convert_fio_file_info(file);
        if (!fi.name[0] || (fi.mode & ATTR_DIRECTORY)) continue;
        if (!ends_with_ci(fi.name, ".CR2")) continue;
        if (strcmp(fi.name, best) > 0)
            snprintf(best, sizeof best, "%s", fi.name);
    } while (FIO_FindNextEx(d, file) == 0);
    FIO_FindClose(d);
    free(file);

    if (!best[0]) return -1;
    snprintf(out, cap, "%s/%s", dir, best);
    return 0;
}

/*
 * The output name: FLM_1234.JPG in the raw's folder.
 *
 * The 5D writes IMG_1234.FLM.JPG, and so did the first build of this,
 * which the camera refused: Canon's filesystem is 8.3 and DryOS will not
 * create a name that is not. FLM_ keeps the number and sorts the films
 * together, after the raws.
 *
 * Built by hand, not with snprintf: the snprintf here is Canon's, and it
 * does not know %.*s. The build before that one asked it for exactly
 * that and tried to create a file called ".*s.FLM.JPG".
 */
void se_out_name(const char *cr2, char *out, size_t cap)
{
    size_t l = strlen(cr2);                 /* ".../IMG_1234.CR2" */
    size_t dir = l;                         /* length including the '/' */
    const char *digits;
    while (dir > 0 && cr2[dir - 1] != '/') dir--;
    digits = cr2 + l - 4;                   /* the four before ".CR2" */
    if (digits - 4 >= cr2 + dir) digits -= 4; else digits = cr2 + dir;

    if (dir + 13 >= cap) { out[0] = 0; return; }
    memcpy(out, cr2, dir);
    strcpy(out + dir, "FLM_");
    memcpy(out + dir + 4, digits, 4);
    strcpy(out + dir + 8, ".JPG");
}

/* Read exactly n bytes at off, in as many calls as the card wants. */
static int read_at(FILE *f, uint32_t off, uint8_t *dst, uint32_t n)
{
    uint32_t done = 0;
    if (FIO_SeekSkipFile(f, off, SEEK_SET) != (int64_t)off) return -1;
    while (done < n) {
        int got = FIO_ReadFile(f, dst + done, n - done);
        if (got <= 0) return -1;
        done += (uint32_t)got;
    }
    return 0;
}

/*
 * The CR2 header. The IFD0 entries that locate the preview, the MakerNote
 * with the exposure data, and the start of the preview itself all sit in
 * the first 100 KB of every 700D frame seen so far; 256 KB is ample.
 */
#define SE_HDR_BYTES  (256 * 1024)
#define SE_OUTBUF     (256 * 1024)
#define SE_ARENA      (1536 * 1024)

typedef struct {
    FILE    *f;
    uint8_t *buf;          /* uncacheable, so FIO needs no cache sync   */
    int      len;
    int      bytes;
    int      err;
} se_out;

static int out_flush(se_out *o)
{
    if (o->len) {
        if (FIO_WriteFile(o->f, o->buf, (size_t)o->len) != o->len) o->err = 1;
        o->len = 0;
    }
    return o->err ? -1 : 0;
}

static int out_sink(void *ctx, const uint8_t *d, size_t n)
{
    se_out *o = (se_out *)ctx;
    while (n) {
        size_t room = SE_OUTBUF - (size_t)o->len;
        size_t k = n < room ? n : room;
        memcpy(o->buf + o->len, d, k);
        o->len += (int)k; o->bytes += (int)k; d += k; n -= k;
        if (o->len == SE_OUTBUF && out_flush(o) != 0) return -1;
    }
    return 0;
}

typedef struct {
    se_jpeg     *enc;
    se_look_run *run;
    int          ow;
    uint32_t     hash;
    int          err;
    int          aborted;     /* power-off during the frame */
    int          preview;     /* the rows are also going to prints.c */
} look_ctx;

/* One finished row, on its way out of the look stage. The hash is taken
 * here, after the look, because with halation on rows leave `radius`
 * rows after they went in -- hashing the input would hash the wrong
 * pixels in the wrong order. tools/previewcheck.c hashes the same rows. */
static int look_emit(void *ctx, const uint8_t *row, int width)
{
    look_ctx *L = (look_ctx *)ctx;
    L->hash = se_hash_bytes(L->hash, row, width * 3);
    if (L->preview) se_pv_row(row);
    if (se_jpeg_write_rows(L->enc, row, 1) != SE_OK) { L->err = 1; return -1; }
    return 0;
}

static int look_rows(void *ctx, const uint8_t *rgb, int nrows, int width)
{
    look_ctx *L = (look_ctx *)ctx;
    /* The one place to stop a frame in flight: once a row batch, which
     * is a few milliseconds. The partial file is removed by the caller. */
    if (ml_shutdown_requested) { L->aborted = 1; L->err = 1; return -1; }
    if (width != L->ow) {
        if (!L->err) se_logf("decoder gave %d columns, expected %d", width, L->ow);
        L->err = 1; return -1;
    }
    if (se_look_rows(L->run, rgb, nrows, look_emit, L) != SE_OK) { L->err = 1; return -1; }
    return 0;
}

/* Progress, drawn by the worker itself. Skipped while Magic Lantern's
 * menu is up: it owns the screen then and would paint over it. */

/*
 * "stream 45%, 3 waiting": the progress of the frame in hand and the
 * count behind it, for the shooting screen and the menu. The count is
 * what says whether the camera may be switched off yet, and roughly how
 * long until it may: about ten seconds a frame at quarter size, forty at
 * half, three minutes at full.
 *
 * Built in pieces. The first attempt was one format, "%d%%  %d waiting",
 * and the count came out 0 on the camera every time while the log, with
 * no %% in its line, had it right: Canon's printf, which is what Magic
 * Lantern's snprintf and bmp_printf resolve to, does not treat %% the
 * way C does, and the conversion after it got the wrong argument. So no
 * format here holds a %% at all; the sign is a character written by hand.
 */
int se_progress_text(char *buf, size_t cap)
{
    int n = snprintf(buf, cap, "%s %d", se_stage, se_pct);
    if (n < 0 || n + 1 >= (int)cap) return n;
    buf[n++] = '%';
    buf[n] = 0;
    if (se_waiting)
        n += snprintf(buf + n, cap - (size_t)n, ", %d waiting", se_waiting);
    return n;
}

/* Read live, not once at the start of the frame: shots taken while this
 * one develops are the ones the count is for. */
void se_count_waiting(void)
{
    se_waiting = se_q_count() > 1 ? se_q_count() - 1 : 0;
}

/* From the engine, per strip; and from se_develop_preview at its start,
 * so the line changes the moment a frame begins. The screen is redrawn
 * at every fifth percent here and once a second by the watcher. */
void se_progress_hook(void *ctx, const char *stage, int pct)
{
    se_count_waiting();
    if (pct == se_pct && stage == se_stage) return;
    se_stage = stage; se_pct = pct;
    if (ml_shutdown_requested) return;
    if (pct % 5 == 0) se_draw_status();
}

/*
 * The preview is streamed off the card as the decoder asks for it, not
 * held. It was held, for a while: this body has 3 MB of DMA memory and
 * every preview seen on it was under 2.2 MB. Then the simulator ran the
 * one sample it had not seen, an ISO 6400 frame whose preview is 2.9 MB,
 * and with the header window and the output buffer beside it that is
 * more than the pool. The 5D streams for the same reason and the engine
 * has the decoder for it; the cost is a few hundred 8 KB reads, which
 * this card does in well under a second.
 *
 * The decoder's window lives in the arena, which is cached, and Magic
 * Lantern's FIO_ReadFile will not read into cached memory above 8 KB (it
 * copies through a temporary and complains on the console each time).
 * So the card is read into an uncached window and the decoder's 8 KB
 * requests are served out of it. The window is 256 KB, not the 8 KB the
 * decoder asks for: served piecewise straight off the card, a frame
 * cost about 250 FIO calls and two and a half seconds more than it had
 * when the preview was held whole. Eight calls a frame costs nothing
 * measurable.
 */
#define SE_BOUNCE (256 * 1024)

typedef struct {
    FILE    *f;
    uint8_t *bounce;        /* fio_malloc'd, SE_BOUNCE                   */
    uint32_t base;          /* where the JPEG starts in the file         */
    uint32_t total;         /* the JPEG's length                         */
    uint32_t wbase, wlen;   /* what the window holds, JPEG-relative      */
} se_pvrd;

static int pv_read(void *ctx, uint32_t off, void *dst, uint32_t n)
{
    se_pvrd *r = (se_pvrd *)ctx;
    uint8_t *d = (uint8_t *)dst;
    if (ml_shutdown_requested) return -1;
    while (n) {
        uint32_t k;
        if (off < r->wbase || off >= r->wbase + r->wlen) {
            uint32_t want = SE_BOUNCE;
            if (off >= r->total) return -1;
            if (off + want > r->total) want = r->total - off;
            if (read_at(r->f, r->base + off, r->bounce, want) != 0) return -1;
            r->wbase = off; r->wlen = want;
        }
        k = r->wbase + r->wlen - off;
        if (k > n) k = n;
        memcpy(d, r->bounce + (off - r->wbase), k);
        d += k; off += k; n -= k;
    }
    return 0;
}

/* The rows of a print being read back, for a preview it never had. */
static int view_rows(void *ctx, const uint8_t *rgb, int nrows, int width)
{
    int i;
    if (ml_shutdown_requested) return -1;
    for (i = 0; i < nrows; i++, rgb += width * 3) se_pv_row(rgb);
    return 0;
}

/*
 * A print older than previews: decode it and keep the small copy. The film's
 * name is in the JPEG's EXIF as Software, and looking for each known name in
 * the header is less code than walking the EXIF.
 */
int se_make_preview(int no)
{
    char path[FIO_MAX_PATH_LENGTH], film[32];
    uint8_t *hdr = 0, *amem = 0;
    FILE *in = 0;
    se_pvrd rd;
    se_arena arena;
    se_jdec_info ji;
    uint32_t size, got;
    int scale, ow, oh, i, rc = -1;

    se_flm_path(no, path, sizeof path);
    size = file_size(path);
    if (!size) return -1;
    memset(&rd, 0, sizeof rd);
    hdr = fio_malloc(SE_HDR_BYTES);
    rd.bounce = fio_malloc(SE_BOUNCE);
    amem = __mem_malloc(SE_ARENA, MEM_TEMPORARY, __FILE__, __LINE__);
    in = FIO_OpenFile(path, O_RDONLY | O_SYNC);
    if (!hdr || !rd.bounce || !amem || !in) goto done;
    got = size < SE_HDR_BYTES ? size : SE_HDR_BYTES;
    if (read_at(in, 0, hdr, got) != 0) goto done;
    if (se_jpeg_dec_info(hdr, got, &ji) != SE_OK) goto done;

    scale = ji.width >= 2592 ? 4 : ji.width >= 1296 ? 2 : 1;
    se_jpeg_scaled_size(ji.width, ji.height, scale, &ow, &oh);
    if (se_pv_begin(ow) != 0) goto done;

    film[0] = 0;
    for (i = 0; i < se_nstocks && !film[0]; i++) {
        int n = (int)strlen(se_stocks[i].name), p, lim = (got < 4096 ? (int)got : 4096) - n;
        for (p = 0; p < lim; p++)
            if (hdr[p] == (uint8_t)se_stocks[i].name[0] && memcmp(hdr + p, se_stocks[i].name, (size_t)n) == 0) {
                snprintf(film, sizeof film, "%s", se_stocks[i].name);
                break;
            }
    }

    rd.f = in; rd.base = 0; rd.total = size;
    se_arena_init(&arena, amem, SE_ARENA);
    if (se_jpeg_decode_stream_scaled(hdr, got, size, pv_read, &rd, scale, &arena, view_rows, 0, &ji) == SE_OK) {
        se_pv_save(no, film, ji.width >= 5184 ? 1 : ji.width >= 2592 ? 0 : 2, size);
        rc = 0;
    }
done:
    se_pv_abort();
    if (in) FIO_CloseFile(in);
    if (amem) free(amem);
    if (rd.bounce) fio_free(rd.bounce);
    if (hdr) fio_free(hdr);
    return rc;
}

/*
 * Develop one frame from the preview Canon embedded in it.
 *
 * Memory: the header window, the read window and the output staging
 * buffer come from fio_malloc, uncacheable, because the card reads and
 * writes them by DMA -- 768 KB together, whatever the preview's size. The arena -- the decoder's MCU rows, the look's delay line, the
 * encoder and its strip -- is ordinary cacheable memory, because the
 * processor works in it and uncached memory would make every access a
 * bus cycle. The camera measured the arena's peak at 511 KB for a
 * full-size frame.
 */
se_err se_develop_preview(const char *cr2_path, const char *out_path,
                                 const se_stock *st, int scale, se_result *R)
{
    uint8_t *hdr = 0, *amem = 0;
    FILE    *in = 0;
    se_pvrd  rd;
    se_out   out;
    se_arena arena;
    se_look  lk;
    look_ctx L;
    se_cr2_info  info;
    se_jdec_info ji;
    se_jpeg_exif ex;
    se_jpeg *enc;
    uint32_t poff = 0, plen = 0, got;
    int pw, ph, t0, t1, t2, t3;
    se_err rc;

    memset(&out, 0, sizeof out);
    memset(&L, 0, sizeof L);
    memset(&rd, 0, sizeof rd);
    memset(R, 0, sizeof *R);
    t0 = get_ms_clock();
    se_progress_hook(0, "reading", 0);

    /* --- locate the preview and read the exposure data ----------------- */
    hdr = fio_malloc(SE_HDR_BYTES);
    if (!hdr) { se_logf("no memory for the header window"); return SE_E_NOMEM; }
    in = FIO_OpenFile(cr2_path, O_RDONLY | O_SYNC);
    if (!in) { rc = SE_E_IO; se_logf("cannot open %s", cr2_path); goto done; }
    {
        uint32_t size = FIO_GetFileSize_direct(cr2_path);
        got = size < SE_HDR_BYTES ? size : SE_HDR_BYTES;
    }
    if (read_at(in, 0, hdr, got) != 0) { rc = SE_E_IO; se_logf("header read failed"); goto done; }
    t1 = get_ms_clock();

    rc = se_cr2_find_preview(hdr, got, &poff, &plen, &pw, &ph);
    if (rc != SE_OK) { se_logf("no preview in the raw (rc=%d)", rc); goto done; }
    if (se_cr2_parse(hdr, got, &info) != SE_OK) memset(&info, 0, sizeof info);
    if (poff >= got) { rc = SE_E_UNSUPPORTED; se_logf("preview starts past the header window"); goto done; }
    if (se_jpeg_dec_info(hdr + poff, got - poff, &ji) != SE_OK) {
        rc = SE_E_BADFILE; se_logf("preview header unreadable"); goto done;
    }

    /* --- the preview is read as the decoder goes; just the bounce now -- */
    rd.f = in; rd.base = poff; rd.total = plen;
    rd.bounce = fio_malloc(SE_BOUNCE);
    if (!rd.bounce) { rc = SE_E_NOMEM; se_logf("no memory for the read bounce"); goto done; }
    t2 = get_ms_clock();

    /* --- the look and the working memory ------------------------------- */
    se_jpeg_scaled_size(ji.width, ji.height, scale, &R->ow, &R->oh);
    R->scale = scale;
    se_look_from_stock(st, scale, &lk);
    {
        int r0 = lk.hal_radius, a0 = lk.hal_amount;
        /* Same budget as tools/previewcheck.c, or the two grade differently
         * and the hash proves nothing. */
        se_look_fit(&lk, R->ow, SE_LOOK_BUDGET);
        if (a0 > 0 && lk.hal_amount == 0)
            se_logf("halation off at %d wide: does not fit %d KB", R->ow, SE_LOOK_BUDGET >> 10);
        else if (lk.hal_radius != r0)
            se_logf("halation radius %d -> %d to fit", r0, lk.hal_radius);
    }
    amem = __mem_malloc(SE_ARENA, MEM_TEMPORARY, __FILE__, __LINE__);
    out.buf = fio_malloc(SE_OUTBUF);
    if (!amem || !out.buf) {
        rc = SE_E_NOMEM;
        se_logf("no memory for the develop (%d KB free)", GetFreeMemForMalloc() >> 10);
        goto done;
    }
    se_arena_init(&arena, amem, SE_ARENA);

    enc = SE_NEW(&arena, se_jpeg, 1);
    if (!enc) { rc = SE_E_NOMEM; goto done; }
    rc = se_look_begin(&L.run, &lk, R->ow, &arena);
    if (rc != SE_OK) { se_logf("look stage refused at %d wide", R->ow); goto done; }
    L.ow = R->ow; L.hash = FNV_INIT; L.enc = enc;
    L.preview = se_pv_begin(R->ow) == 0;

    /* --- encode -------------------------------------------------------- */
    out.f = FIO_CreateFile(out_path);
    if (!out.f) { rc = SE_E_IO; se_logf("cannot create %s", out_path); goto done; }
    memset(&ex, 0, sizeof ex);
    ex.make          = info.make[0]  ? info.make  : "Canon";
    ex.model         = info.model[0] ? info.model : "Canon EOS 700D";
    ex.software      = st->name;
    ex.datetime      = info.datetime;
    ex.exposure_time = info.exposure_time;
    ex.aperture      = info.aperture;
    ex.iso           = info.iso;
    {
        /* 4:2:0, as every camera JPEG is: half the encoder's blocks and a
         * third off the file. tools/previewcheck takes "420" to match. */
        uint8_t *strip = SE_NEW(&arena, uint8_t, SE_JPEG_STRIP_BYTES(R->ow, 1));
        if (!strip) { rc = SE_E_NOMEM; goto done; }
        rc = se_jpeg_begin2(enc, R->ow, R->oh, 88, strip, &ex, out_sink, &out, 1);
        if (rc != SE_OK) goto done;
    }

    rc = se_jpeg_decode_stream_scaled(hdr + poff, got - poff, plen, pv_read, &rd,
                                      scale, &arena, look_rows, &L, &ji);
    if (rc != SE_OK && !L.aborted) se_logf("preview decode rc=%d", rc);
    /* the delay line still holds `radius` rows */
    if (rc == SE_OK && !L.err) rc = se_look_end(L.run, look_emit, &L);
    if (rc == SE_OK && !L.err) rc = se_jpeg_end(enc);
    if (rc == SE_OK && out_flush(&out) != 0) rc = SE_E_IO;
    if (L.err && rc == SE_OK) rc = SE_E_IO;
    t3 = get_ms_clock();

    R->hash = L.hash;
    R->bytes = out.bytes;
    R->arena_peak = arena.peak;
    R->aborted = L.aborted;
    R->ms_hdr = t1 - t0; R->ms_read = t2 - t1; R->ms_stream = t3 - t2;
    R->ms_total = t3 - t0;

done:
    if (out.f) FIO_CloseFile(out.f);
    if (rc != SE_OK && out.f) FIO_RemoveFile(out_path);
    if (rc == SE_OK && L.preview)
        se_pv_save(se_file_no(out_path), st->name, scale == 1 ? 1 : scale == 4 ? 2 : 0, (uint32_t)out.bytes);
    else se_pv_abort();
    if (in) FIO_CloseFile(in);
    if (out.buf) fio_free(out.buf);
    if (amem) free(amem);
    if (rd.bounce) fio_free(rd.bounce);
    if (hdr) fio_free(hdr);
    return rc;
}
