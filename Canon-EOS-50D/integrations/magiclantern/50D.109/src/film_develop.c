/** \file
 * Develop CR2s into FLM_nnnn.JPG while Film is on.
 *
 * ML/SE.NEXT holds the last finished number. The next numbers are opened
 * by name, with no directory walk, until one is missing. A failed develop
 * is not recorded as finished. The JPEG is held in memory and written once.
 */
#include "dryos.h"
#include "mem.h"
#include "fio-ml.h"
#include "fileprefix.h"
#include "menu.h"
#include "notify_box.h"

#include "se.h"
#include "cr2.h"
#include "stock.h"
#include "look.h"
#include "jpeg_dec.h"
#include "jpeg_enc.h"

#define FILM_HDR     (256 * 1024)
#define FILM_SCRATCH (768 * 1024)
#define FILM_JPEG    (2 * 1024 * 1024)
#define FILM_SCALE   2
/* The 50D only plays back its own Small JPEG size. */
#define FILM_PLAY_W  2352
#define FILM_PLAY_H  1568
#define FILM_MIN_CR2 (6u * 1024 * 1024)
#define FILM_LOOKBACK 20

extern int ml_started;
extern int film_is_on(void);
extern int film_wants_delete(void);

typedef struct {
    uint8_t *data;
    int      cap;
    int      len;
    int      err;
} film_mem;

static char film_job[32];
static int film_shown = -1;
static int film_shown_ms;

/* hundredths is 4371 for 43.71%. Redraw when the shown number changes,
 * and at most ten times a second. */
static void film_show(int hundredths)
{
    int now = get_ms_clock();
    if (hundredths < 0) hundredths = 0;
    if (hundredths > 10000) hundredths = 10000;
    if (hundredths == film_shown) return;
    if (film_shown_ms && hundredths < 10000 && (now - film_shown_ms) < 100) return;
    film_shown = hundredths;
    film_shown_ms = now;
    NotifyBoxAt(16, 480 - 48, 600000, "FILM %s %d,%02d%%",
                film_job, hundredths / 100, hundredths % 100);
}
static se_stock film_cached;
static char film_cached_name[32];
static int film_cached_ok;

static int film_sink(void *ctx, const uint8_t *data, size_t len)
{
    film_mem *o = (film_mem *)ctx;
    if (o->err) return -1;
    if (len > (size_t)(o->cap - o->len)) { o->err = 1; return -1; }
    memcpy(o->data + o->len, data, len);
    o->len += (int)len;
    return 0;
}

static void film_progress(void *ctx, const char *stage, int percent)
{
    (void)ctx;
    (void)stage;
    film_show(percent);
}

static int film_read_at(FILE *h, int off, void *dst, int n)
{
    int done = 0;
    if (FIO_SeekSkipFile(h, off, 0) < 0) return -1;
    while (done < n) {
        int k = FIO_ReadFile(h, (char *)dst + done, (size_t)(n - done));
        if (k <= 0) return -1;
        done += k;
    }
    return 0;
}

static int film_write_at(FILE *h, int off, const void *src, int n)
{
    int done = 0;
    if (FIO_SeekSkipFile(h, off, 0) < 0) return -1;
    while (done < n) {
        int chunk = n - done;
        int k;
        if (chunk > 256 * 1024) chunk = 256 * 1024;
        k = FIO_WriteFile(h, (char *)src + done, (size_t)chunk);
        if (k != chunk) return -1;
        done += k;
    }
    return 0;
}

/* The playback list already contains the CR2. Its embedded preview is what
 * that entry displays, so replace the preview with the film JPEG. A new
 * name is only picked up when the camera builds the list again. */
static void film_embed_preview(const char *cr2, const uint8_t *jpg, int jlen,
                               int width, int height)
{
    uint8_t *hdr;
    uint8_t b[4];
    FILE *h;
    int got, n, i, ifd;
    int off_w = -1, off_h = -1, off_cnt = -1;
    uint32_t strip = 0, bytes = 0;

    if (!jpg || jlen < 4 || jpg[0] != 0xff || jpg[1] != 0xd8) return;
    /* The encoder may have shortened the frame so the width divides by 16.
     * The TIFF tags have to name that size, not the one the caller asked for. */
    {
        int k = 2;
        while (k + 9 < jlen) {
            int m, seglen;
            if (jpg[k] != 0xff) { k++; continue; }
            while (k < jlen && jpg[k] == 0xff) k++;
            if (k >= jlen) break;
            m = jpg[k++];
            if (m == 0xd8 || m == 0xd9 || (m >= 0xd0 && m <= 0xd7)) continue;
            if (k + 2 > jlen) break;
            seglen = (jpg[k] << 8) | jpg[k + 1];
            if (m == 0xc0) {
                height = (jpg[k + 3] << 8) | jpg[k + 4];
                width  = (jpg[k + 5] << 8) | jpg[k + 6];
                break;
            }
            k += seglen;
        }
    }
    hdr = fio_malloc(FILM_HDR);
    if (!hdr) return;
    got = (int)read_file(cr2, hdr, FILM_HDR);
    if (got < 16 || hdr[0] != 'I' || hdr[1] != 'I') { fio_free(hdr); return; }
    ifd = (int)(hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | (hdr[7] << 24));
    if (ifd < 8 || ifd + 2 > got) { fio_free(hdr); return; }
    n = hdr[ifd] | (hdr[ifd + 1] << 8);
    if (n < 1 || n > 64 || ifd + 2 + n * 12 > got) { fio_free(hdr); return; }
    for (i = 0; i < n; i++) {
        int e = ifd + 2 + i * 12;
        int tag = hdr[e] | (hdr[e + 1] << 8);
        int typ = hdr[e + 2] | (hdr[e + 3] << 8);
        uint32_t cnt = (uint32_t)hdr[e + 4] | ((uint32_t)hdr[e + 5] << 8) |
                       ((uint32_t)hdr[e + 6] << 16) | ((uint32_t)hdr[e + 7] << 24);
        uint32_t val = (uint32_t)hdr[e + 8] | ((uint32_t)hdr[e + 9] << 8) |
                       ((uint32_t)hdr[e + 10] << 16) | ((uint32_t)hdr[e + 11] << 24);
        if (cnt != 1) continue;
        if (tag == 0x100 && typ == 3) off_w = e + 8;
        else if (tag == 0x101 && typ == 3) off_h = e + 8;
        else if (tag == 0x111 && typ == 4) strip = val;
        else if (tag == 0x117 && typ == 4) { bytes = val; off_cnt = e + 8; }
    }
    fio_free(hdr);
    if (off_w < 0 || off_h < 0 || off_cnt < 0 || !strip || !bytes) return;
    if (jlen > (int)bytes || width <= 0 || height <= 0) return;

    h = FIO_OpenFile(cr2, O_RDWR);
    if (!h) return;
    if (film_write_at(h, (int)strip, jpg, jlen) != 0) {
        FIO_CloseFile(h);
        return;
    }
    b[0] = (uint8_t)width; b[1] = (uint8_t)(width >> 8); b[2] = 0; b[3] = 0;
    film_write_at(h, off_w, b, 4);
    b[0] = (uint8_t)height; b[1] = (uint8_t)(height >> 8);
    film_write_at(h, off_h, b, 4);
    b[0] = (uint8_t)jlen; b[1] = (uint8_t)(jlen >> 8);
    b[2] = (uint8_t)(jlen >> 16); b[3] = (uint8_t)(jlen >> 24);
    film_write_at(h, off_cnt, b, 4);
    FIO_CloseFile(h);
}

static int film_emit(void *ctx, const uint8_t *row, int width)
{
    struct {
        se_jpeg *enc;
        se_look_run *run;
        int ow;
        int err;
    } *L = ctx;
    if (width != L->ow) { L->err = 1; return -1; }
    if (se_jpeg_write_rows(L->enc, row, 1) != SE_OK) { L->err = 1; return -1; }
    return 0;
}

static int film_rows(void *ctx, const uint8_t *rgb, int nrows, int width)
{
    struct {
        se_jpeg *enc;
        se_look_run *run;
        int ow;
        int err;
    } *L = ctx;
    if (width != L->ow) { L->err = 1; return -1; }
    if (se_look_rows(L->run, rgb, nrows, film_emit, L) != SE_OK) {
        L->err = 1;
        return -1;
    }
    return 0;
}

/* DMA buffer. FIO_ReadFile asserts if a cacheable pointer is larger than 8 KB. */
static int film_read_file(const char *path, void *dma, int cap)
{
    int n = (int)read_file(path, dma, (size_t)cap);
    if (n < 0) return -1;
    if (n >= cap) n = cap - 1;
    ((char *)dma)[n] = 0;
    return n;
}

/* Read STOCK from SE.CFG and the matching ML/FILM file. */
static int film_load_stock(se_stock *s)
{
    char *cfg, *index, *text;
    char name[32];
    char path[64];
    int n, i, line0, got;
    int ok = 0;

    cfg = fio_malloc(256);
    index = fio_malloc(4096);
    text = fio_malloc(16384);
    if (!cfg || !index || !text) goto out;

    name[0] = 0;
    n = film_read_file("ML/SE.CFG", cfg, 256);
    if (n <= 0) n = film_read_file("A:/ML/SE.CFG", cfg, 256);
    if (n > 0) {
        line0 = 0;
        for (i = 0; i <= n; i++) {
            if (cfg[i] != '\n' && cfg[i] != '\r' && cfg[i] != 0) continue;
            cfg[i] = 0;
            if (!strncmp(cfg + line0, "STOCK ", 6)) {
                snprintf(name, sizeof(name), "%s", cfg + line0 + 6);
                break;
            }
            line0 = i + 1;
        }
    }
    if (film_cached_ok && name[0] && !strncmp(name, film_cached_name, 31)) {
        *s = film_cached;
        ok = 1;
        goto out;
    }

    n = film_read_file("ML/FILM/INDEX.TXT", index, 4096);
    if (n <= 0) n = film_read_file("A:/ML/FILM/INDEX.TXT", index, 4096);
    if (n <= 0) goto out;
    line0 = 0;
    for (i = 0; i <= n; i++) {
        char *line;
        if (index[i] != '\n' && index[i] != '\r' && index[i] != 0) continue;
        index[i] = 0;
        line = index + line0;
        line0 = i + 1;
        if (!line[0] || line[0] == '#') continue;
        snprintf(path, sizeof(path), "ML/FILM/%s", line);
        got = film_read_file(path, text, 16384);
        if (got <= 0) continue;
        se_stock_init(s);
        if (se_stock_parse(text, (size_t)got, s) != SE_OK) continue;
        if (!name[0] || !strncmp(s->name, name, sizeof(name))) {
            film_cached = *s;
            snprintf(film_cached_name, sizeof(film_cached_name), "%s", s->name);
            film_cached_ok = 1;
            ok = 1;
            break;
        }
    }
out:
    fio_free(cfg);
    fio_free(index);
    fio_free(text);
    return ok ? 0 : -1;
}

static int film_develop(const char *cr2, const char *jpg, se_stock *stock)
{
    uint8_t *block, *scratch, *pv, *jbuf;
    se_arena arena, lookar;
    se_look lk;
    se_cr2_info info;
    se_jdec_info ji;
    se_jpeg_exif ex;
    se_jpeg *enc;
    se_look_run *run;
    film_mem jpeg;
    FILE *out;
    struct {
        se_jpeg *enc;
        se_look_run *run;
        int ow;
        int err;
    } L;
    size_t lookneed, need;
    uint32_t poff = 0, plen = 0;
    int pw = 0, ph = 0, ow, oh, got, jcap;
    se_err rc = SE_E_IO;
    void *hdr;

    hdr = fio_malloc(FILM_HDR);
    if (!hdr) return -1;
    got = (int)read_file(cr2, hdr, FILM_HDR);
    if (got <= 0) { fio_free(hdr); return -1; }
    rc = se_cr2_find_preview(hdr, (size_t)got, &poff, &plen, &pw, &ph);
    if (rc != SE_OK) { fio_free(hdr); return -1; }
    if (se_cr2_parse(hdr, (size_t)got, &info) != SE_OK)
        memset(&info, 0, sizeof info);
    if (poff >= (uint32_t)got) { fio_free(hdr); return -1; }
    pv = (uint8_t *)hdr + poff;
    if (se_jpeg_dec_info(pv, (size_t)got - poff, &ji) != SE_OK) {
        fio_free(hdr); return -1;
    }
    /* Half the coded preview: 4752x3168 becomes 2376x1584. */
    se_jpeg_scaled_size(ji.width, ji.height, FILM_SCALE, &ow, &oh);
    se_look_from_stock(stock, FILM_SCALE, &lk);
    lookneed = se_look_fit(&lk, ow, SE_LOOK_BUDGET);
    need = FILM_SCRATCH + lookneed + 64;
    jcap = FILM_JPEG;
    fio_free(hdr);
    hdr = 0;
    block = malloc(need);
    pv = malloc(plen);
    jbuf = malloc((size_t)jcap);
    if (!block || !pv || !jbuf) {
        free(block);
        free(pv);
        free(jbuf);
        return -1;
    }

    /* DMA only for the read. The decode runs from the cached copy. */
    {
        void *dma = fio_malloc(plen);
        FILE *h;
        int rd;
        if (!dma) {
            free(block);
            free(pv);
            free(jbuf);
            return -1;
        }
        h = FIO_OpenFile(cr2, 0);
        rd = !h || film_read_at(h, (int)poff, dma, (int)plen) != 0;
        if (h) FIO_CloseFile(h);
        if (!rd) memcpy(pv, dma, plen);
        fio_free(dma);
        if (rd) {
            free(block);
            free(pv);
            free(jbuf);
            return -1;
        }
    }

    memset(&jpeg, 0, sizeof jpeg);
    memset(&L, 0, sizeof L);
    jpeg.data = jbuf;
    jpeg.cap = jcap;
    scratch = block;
    se_arena_init(&arena, scratch, FILM_SCRATCH);
    se_arena_init(&lookar, scratch + FILM_SCRATCH, lookneed);

    snprintf(film_job, sizeof(film_job), "%s", stock->name[0] ? stock->name : "Film");
    se_set_progress_fine(film_progress, 0);

    enc = SE_NEW(&arena, se_jpeg, 1);
    if (!enc) { rc = SE_E_NOMEM; goto done; }
    rc = se_look_begin(&run, &lk, ow, &lookar);
    if (rc != SE_OK) goto done;
    L.run = run;
    L.ow = ow;

    memset(&ex, 0, sizeof ex);
    ex.make = info.make[0] ? info.make : "Canon";
    ex.model = info.model[0] ? info.model : "Canon EOS 50D";
    ex.software = stock->name;
    ex.datetime = info.datetime;
    ex.exposure_time = info.exposure_time;
    ex.aperture = info.aperture;
    ex.iso = info.iso;
    {
        uint8_t *strip = SE_NEW(&arena, uint8_t, (size_t)8 * ow * 3);
        uint8_t *trow, *tstrip, *tbuf;
        se_jpeg *th;
        film_mem tmem;
        int play_w = ow, play_h = oh, ty;
        if (!strip) { rc = SE_E_NOMEM; goto done; }
        if (ow >= FILM_PLAY_W && oh >= FILM_PLAY_H) {
            play_w = FILM_PLAY_W;
            play_h = FILM_PLAY_H;
        }
        tbuf = SE_NEW(&arena, uint8_t, 8192);
        trow = SE_NEW(&arena, uint8_t, 160 * 3);
        tstrip = SE_NEW(&arena, uint8_t, 8 * 160 * 3);
        th = SE_NEW(&arena, se_jpeg, 1);
        if (tbuf && trow && tstrip && th) {
            memset(trow, 128, 160 * 3);
            memset(&tmem, 0, sizeof tmem);
            tmem.data = tbuf;
            tmem.cap = 8192;
            if (se_jpeg_begin(th, 160, 120, 60, tstrip, NULL, film_sink, &tmem) == SE_OK) {
                for (ty = 0; ty < 120; ty++)
                    se_jpeg_write_rows(th, trow, 1);
                if (se_jpeg_end(th) == SE_OK && tmem.len > 0) {
                    ex.thumb = tbuf;
                    ex.thumb_len = tmem.len;
                }
            }
        }
        rc = se_jpeg_begin(enc, play_w, play_h, 88, strip, &ex, film_sink, &jpeg);
        if (rc != SE_OK) goto done;
        enc->stride = ow;
    }
    L.enc = enc;
    rc = se_jpeg_decode_scaled(pv, plen, FILM_SCALE, &arena, film_rows, &L, &ji);
    if (rc == SE_OK && !L.err) rc = se_look_end(L.run, film_emit, &L);
    if (rc == SE_OK && !L.err) rc = se_jpeg_end(enc);
    if (jpeg.err || jpeg.len <= 0) rc = SE_E_IO;
    out = 0;
    if (rc == SE_OK) {
        int left = jpeg.len;
        int off = 0;
        out = FIO_CreateFile(jpg);
        while (out && left > 0) {
            int chunk = left > (256 * 1024) ? (256 * 1024) : left;
            int w = FIO_WriteFile(out, jpeg.data + off, (size_t)chunk);
            if (w != chunk) { rc = SE_E_IO; break; }
            off += w;
            left -= w;
        }
        if (!out) rc = SE_E_IO;
        if (out) FIO_CloseFile(out);
        if (rc == SE_OK)
            film_embed_preview(cr2, jpeg.data, jpeg.len, ow, oh);
    }

done:
    se_set_progress(0, 0);
    se_set_progress_fine(0, 0);
    free(block);
    free(pv);
    free(jbuf);
    fio_free(hdr);
    return rc == SE_OK ? 0 : -1;
}

static void film_paths(int num, char *cr2, char *jpg, char *tmp, int n)
{
    const char *dir = get_dcim_dir();
    const char *prefix = get_file_prefix();
    /* Eight characters, four of them digits: the playback list accepts this. */
    snprintf(cr2, n, "%s/%s%04d.CR2", dir, prefix, num);
    snprintf(jpg, n, "%s/FLM_%04d.JPG", dir, num);
    snprintf(tmp, n, "%s/FLM_%04d.TMP", dir, num);
}

/* Earlier builds wrote IMG_nnnn.FLM.JPG. That file already finished the frame. */
static int film_old_jpg(int num)
{
    char old[64];
    const char *dir = get_dcim_dir();
    const char *prefix = get_file_prefix();
    snprintf(old, sizeof(old), "%s/%s%04d.FLM.JPG", dir, prefix, num);
    return is_file(old);
}

static int film_next_load(void)
{
    char *b = fio_malloc(32);
    int n, v = 0;
    if (!b) return 0;
    n = film_read_file("ML/SE.NEXT", b, 32);
    if (n <= 0) n = film_read_file("A:/ML/SE.NEXT", b, 32);
    if (n > 0) v = atoi(b);
    fio_free(b);
    return v;
}

static void film_next_save(int num)
{
    char *b = fio_malloc(32);
    int n;
    FILE *f;
    if (!b) return;
    n = snprintf(b, 32, "%d\n", num);
    f = FIO_CreateFile("ML/SE.NEXT");
    if (!f) f = FIO_CreateFile("A:/ML/SE.NEXT");
    if (f && n > 0) {
        FIO_WriteFile(f, b, (size_t)n);
        FIO_CloseFile(f);
    }
    fio_free(b);
}

/* 1 = this number is settled, 0 = not ready yet, -1 = failed, try again. */
static int film_one(int num, int current)
{
    char cr2[64], jpg[64], tmp[64];
    se_stock stock;
    uint32_t s1 = 0, s2 = 0;

    film_paths(num, cr2, jpg, tmp, 64);
    if (is_file(jpg) || film_old_jpg(num)) return 1;
    if (FIO_GetFileSize(cr2, &s1) != 0 || s1 == 0) {
        if (is_file(tmp)) FIO_RemoveFile(tmp);
        return num < current ? 1 : 0;
    }
    if (s1 < FILM_MIN_CR2) {
        msleep(300);
        if (FIO_GetFileSize(cr2, &s2) != 0 || s2 != s1) return 0;
        return num < current ? 1 : 0;
    }
    msleep(400);
    if (FIO_GetFileSize(cr2, &s2) != 0 || s2 != s1) return 0;
    if (film_load_stock(&stock) != 0) return -1;
    if (is_file(tmp)) FIO_RemoveFile(tmp);
    snprintf(film_job, sizeof(film_job), "%s", stock.name[0] ? stock.name : "Film");
    film_shown = -1;
    film_shown_ms = 0;
    film_show(0);
    if (film_develop(cr2, tmp, &stock) == 0 && is_file(tmp)) {
        FIO_RenameFile(tmp, jpg);
        if (is_file(jpg)) {
            if (is_file(tmp)) FIO_RemoveFile(tmp);
            if (film_wants_delete()) FIO_RemoveFile(cr2);
            NotifyBoxHide();
            return 1;
        }
    }
    if (is_file(tmp)) FIO_RemoveFile(tmp);
    NotifyBoxHide();
    return -1;
}

static int film_bootstrap(int current)
{
    int saved = film_next_load();
    int n;
    char cr2[64], jpg[64], tmp[64];
    uint32_t sz;
    if (saved > 0) return saved;
    for (n = current - FILM_LOOKBACK; n < current; n++) {
        if (n < 1) continue;
        film_paths(n, cr2, jpg, tmp, 64);
        if (is_file(jpg) || film_old_jpg(n)) continue;
        sz = 0;
        if (is_file(tmp) || (FIO_GetFileSize(cr2, &sz) == 0 && sz >= FILM_MIN_CR2))
            return n - 1;
    }
    return current > 0 ? current : 0;
}

static void film_dev_task(void *unused)
{
    int cursor = -1;
    (void)unused;
    while (!ml_started) msleep(100);
    msleep(2000);
    for (;;) {
        struct card_info *card;
        int current, rc;
        if (!film_is_on()) {
            cursor = -1;
            msleep(500);
            continue;
        }
        card = get_shooting_card();
        current = card ? card->file_number : 0;
        if (current <= 0) { msleep(500); continue; }
        if (cursor < 0) cursor = film_bootstrap(current);
        if (cursor >= current) { msleep(500); continue; }
        rc = film_one(cursor + 1, current);
        if (rc > 0) {
            cursor++;
            film_next_save(cursor);
        } else if (rc < 0) {
            msleep(2000);
        } else {
            msleep(500);
        }
    }
}

void film_develop_start(void)
{
    task_create("film_dev", 0x1f, 0x10000, film_dev_task, 0);
}
