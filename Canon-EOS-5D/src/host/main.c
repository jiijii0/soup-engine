/*
 * filmdev - host-side driver for the Soup Engine.
 *
 * Runs the exact same engine sources that the camera build uses, so a bug
 * found here is a bug fixed there. The camera has no console and no live
 * view, so this is where all the real debugging happens.
 */
#include "../engine/se.h"
#include "../engine/cr2.h"
#include "../engine/lut.h"
#include "../engine/pipeline.h"
#include "../engine/debayer.h"
#include "../engine/xmp.h"
#include "../engine/ui.h"
#include "../engine/stock.h"
#include "../engine/jpeg_dec.h"
#include "../engine/look.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <errno.h>

static void log_cb(void *ctx, const char *msg)
{
    (void)ctx;
    fprintf(stderr, "  %s\n", msg);
}

static int g_quiet_progress;
static void progress_cb(void *ctx, const char *stage, int pct)
{
    static char   last_stage[32];
    static int    last_pct = -1;
    (void)ctx;
    if (g_quiet_progress) return;
    if (strcmp(last_stage, stage) != 0) {
        if (last_pct >= 0) fprintf(stderr, "\n");
        snprintf(last_stage, sizeof last_stage, "%s", stage);
        last_pct = -1;
    }
    if (pct / 5 != last_pct / 5) {
        fprintf(stderr, "\r  %-10s %3d%%", stage, pct);
        fflush(stderr);
        last_pct = pct;
    }
    if (pct >= 100) { fprintf(stderr, "\r  %-10s done\n", stage); last_pct = -1; }
}

static void *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    long  sz;
    void *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *out_len = (size_t)sz;
    return buf;
}

static const char *cfa_name(int phase)
{
    static const char *n[4] = { "RGGB", "GRBG", "GBRG", "BGGR" };
    return n[phase & 3];
}

static void dump_info(const se_cr2_info *i)
{
    printf("make/model     : %s / %s\n", i->make, i->model);
    printf("exposure       : %g s  f/%.1f  ISO %d\n",
           i->exposure_time, i->aperture, i->iso);
    printf("raw frame      : %d x %d\n", i->raw_width, i->raw_height);
    printf("active area    : %d x %d at (%d,%d)..(%d,%d)\n",
           i->active_width, i->active_height,
           i->crop_left, i->crop_top, i->crop_right, i->crop_bottom);
    printf("cfa phase      : %s\n", cfa_name(i->cfa_phase));
    printf("bit depth      : %d\n", i->bits);
    printf("levels         : black %d%s  white %d\n",
           i->black, i->black_measured ? " (measured)" : " (from database)",
           i->white);
    printf("black per site : %d %d %d %d\n",
           i->black_cfa[0], i->black_cfa[1], i->black_cfa[2], i->black_cfa[3]);
    printf("as-shot WB     : R %.0f  G1 %.0f  B %.0f  G2 %.0f%s\n",
           i->wb[0], i->wb[1], i->wb[2], i->wb[3],
           i->wb_present ? "" : " (absent, assumed neutral)");
    printf("wb multipliers : R %.4f  G 1.0000  B %.4f\n",
           i->wb[1] > 0 ? i->wb[0] / i->wb[1] : 1.0,
           i->wb[1] > 0 ? i->wb[2] / i->wb[1] : 1.0);
    printf("strip          : offset %u, %u bytes\n", i->strip_offset, i->strip_bytes);
    printf("cr2 slices     : count %d, width %d, last %d%s\n",
           i->slice_count, i->slice_width, i->slice_last,
           i->slice_count == 0 ? "  <- unsliced, streamable" : "  <- sliced, needs a full frame buffer");
    if (i->cam_xyz_present) {
        printf("cam_xyz        : % .4f % .4f % .4f\n"
               "                 % .4f % .4f % .4f\n"
               "                 % .4f % .4f % .4f\n",
               i->cam_xyz[0], i->cam_xyz[1], i->cam_xyz[2],
               i->cam_xyz[3], i->cam_xyz[4], i->cam_xyz[5],
               i->cam_xyz[6], i->cam_xyz[7], i->cam_xyz[8]);
    }
}

static int write_pgm16(const char *path, const uint16_t *d, int w, int h)
{
    FILE *f = fopen(path, "wb");
    int    y, x;
    uint8_t *line;
    if (!f) return -1;
    fprintf(f, "P5\n%d %d\n65535\n", w, h);
    line = malloc((size_t)w * 2);
    if (!line) { fclose(f); return -1; }
    for (y = 0; y < h; y++) {
        const uint16_t *s = d + (size_t)y * w;
        for (x = 0; x < w; x++) {            /* PGM is big endian */
            line[x * 2 + 0] = (uint8_t)(s[x] >> 8);
            line[x * 2 + 1] = (uint8_t)(s[x] & 0xff);
        }
        fwrite(line, 1, (size_t)w * 2, f);
    }
    free(line);
    fclose(f);
    return 0;
}

static int jpeg_sink(void *ctx, const uint8_t *data, size_t len)
{
    return fwrite(data, 1, len, (FILE *)ctx) == len ? 0 : -1;
}

static int ppm_row_sink(void *ctx, const uint8_t *rgb8, int nrows, int width)
{
    size_t n = (size_t)nrows * width * 3;
    return fwrite(rgb8, 1, n, (FILE *)ctx) == n ? 0 : -1;
}

/*
 * Create the directory a path lives in. A fresh clone has no out/, and
 * every default output path points there; failing on that is a pointless
 * first impression.
 */
static void ensure_dir_for(const char *path)
{
    char dir[1024];
    char *slash;
    size_t n = strlen(path);
    if (n >= sizeof dir) return;
    memcpy(dir, path, n + 1);
    slash = strrchr(dir, '/');
    if (!slash || slash == dir) return;
    *slash = 0;

    /*
     * Every level, not one. "one level is enough for out/ and friends"
     * stopped being true the moment `filmdev looks` started writing to
     * out/looks/<frame>/ -- it created nothing and reported thirteen
     * "cannot write" lines per frame.
     */
    for (slash = dir + 1; *slash; slash++) {
        if (*slash != '/') continue;
        *slash = 0;
        mkdir(dir, 0755);
        *slash = '/';
    }
    mkdir(dir, 0755);
}

static int ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcmp(s + a - b, suf) == 0;
}

/* ===================================================== stock library == */
/*
 * Stocks are plain files, so the library is just a directory. The camera
 * does the same thing with ML/FILM on the card -- there is no registry
 * and nothing to rebuild when you add one.
 */

static const char *stock_dir(void)
{
    const char *d = getenv("FILMDEV_STOCKS");
    return (d && *d) ? d : "stocks";
}

/* Accepts a path, or a bare name resolved against the stock directory. */
static int resolve_stock(const char *want, char *out, size_t outsz)
{
    FILE *f;
    size_t i;
    char lower[64];

    f = fopen(want, "rb");
    if (f) { fclose(f); snprintf(out, outsz, "%s", want); return 0; }

    for (i = 0; want[i] && i < sizeof lower - 1; i++)
        lower[i] = (char)(want[i] >= 'A' && want[i] <= 'Z' ? want[i] + 32 : want[i]);
    lower[i] = 0;

    snprintf(out, outsz, "%s/%s.fs", stock_dir(), lower);
    f = fopen(out, "rb");
    if (f) { fclose(f); return 0; }

    snprintf(out, outsz, "%s/%s", stock_dir(), want);
    f = fopen(out, "rb");
    if (f) { fclose(f); return 0; }
    return -1;
}

static int load_stock(const char *want, se_stock *st, char *resolved, size_t rsz)
{
    size_t len;
    void  *txt;
    se_err rc;

    if (resolve_stock(want, resolved, rsz) != 0) {
        fprintf(stderr, "filmdev: no stock '%s' (looked in %s/)\n", want, stock_dir());
        return -1;
    }
    txt = read_file(resolved, &len);
    if (!txt) { fprintf(stderr, "filmdev: cannot read %s\n", resolved); return -1; }
    rc = se_stock_parse((const char *)txt, len, st);
    free(txt);
    if (rc != SE_OK) {
        fprintf(stderr, "filmdev: %s: %s\n", resolved, se_strerror(rc));
        return -1;
    }
    return 0;
}

#define MAX_STOCKS 64
static char g_stock_path[MAX_STOCKS][512];
static int  g_stock_n;

static int cmp_path(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static int scan_stocks(void)
{
    DIR *d = opendir(stock_dir());
    struct dirent *e;
    g_stock_n = 0;
    if (!d) return 0;
    while ((e = readdir(d)) != NULL && g_stock_n < MAX_STOCKS) {
        size_t n = strlen(e->d_name);
        if (n > 3 && strcmp(e->d_name + n - 3, ".fs") == 0)
            snprintf(g_stock_path[g_stock_n++], sizeof g_stock_path[0],
                     "%s/%s", stock_dir(), e->d_name);
    }
    closedir(d);
    qsort(g_stock_path, (size_t)g_stock_n, sizeof g_stock_path[0], cmp_path);
    return g_stock_n;
}

static int list_stocks(void)
{
    int i;
    if (!scan_stocks()) {
        fprintf(stderr, "filmdev: no stocks in %s/\n", stock_dir());
        return 1;
    }
    printf("film stocks in %s/\n\n", stock_dir());
    for (i = 0; i < g_stock_n; i++) {
        size_t len;
        void  *txt = read_file(g_stock_path[i], &len);
        se_stock st;
        if (!txt) continue;
        if (se_stock_parse((const char *)txt, len, &st) == SE_OK) {
            const char *slash = strrchr(g_stock_path[i], '/');
            char key[64];
            int  k = 0;
            const char *b = slash ? slash + 1 : g_stock_path[i];
            while (b[k] && b[k] != '.' && k < 60) { key[k] = b[k]; k++; }
            key[k] = 0;
            printf("  %-14s %-44s", key, st.desc);
            if (st.wb_preset > 0) printf(" [%s]", se_wb_name(st.wb_preset));
            if (st.grain > 0)     printf(" grain %.0f", st.grain);
            printf("\n");
        }
        free(txt);
    }
    printf("\nuse: filmdev develop photo.CR2 --stock <name>\n");
    return 0;
}

/* ======================================================= card layout == */

static int copy_file(const char *src, const char *dst)
{
    size_t len;
    void  *d = read_file(src, &len);
    FILE  *f;
    if (!d) return -1;
    f = fopen(dst, "wb");
    if (!f) { free(d); return -1; }
    fwrite(d, 1, len, f);
    fclose(f);
    free(d);
    return 0;
}

static int prepare_card(const char *root)
{
    char path[1024];
    int  i, n = scan_stocks();

    if (!n) { fprintf(stderr, "filmdev: no stocks to copy\n"); return 1; }

    snprintf(path, sizeof path, "%s/ML", root);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/ML/FILM", root);
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "filmdev: cannot create %s\n", path);
        return 1;
    }

    for (i = 0; i < n; i++) {
        const char *slash = strrchr(g_stock_path[i], '/');
        snprintf(path, sizeof path, "%s/ML/FILM/%s", root,
                 slash ? slash + 1 : g_stock_path[i]);
        if (copy_file(g_stock_path[i], path) != 0) {
            fprintf(stderr, "filmdev: cannot write %s\n", path);
            return 1;
        }
        printf("  %s\n", path);
    }
    /*
     * The index the camera reads instead of enumerating the directory.
     * FIO_FindFirstEx is the one call in the payload whose contract is
     * guessed rather than known -- cookie type, failure value and dirent
     * shape all come from a newer Magic Lantern tree than the binary it runs
     * inside -- and the camera's log showed the payload dying there. Naming
     * the files here costs nothing and removes that call from the path.
     */
    snprintf(path, sizeof path, "%s/ML/FILM/INDEX.TXT", root);
    {
        FILE *ix = fopen(path, "wb");
        if (!ix) { fprintf(stderr, "filmdev: cannot write %s\n", path); return 1; }
        for (i = 0; i < n; i++) {
            const char *slash = strrchr(g_stock_path[i], '/');
            fprintf(ix, "%s\n", slash ? slash + 1 : g_stock_path[i]);
        }
        fclose(ix);
        printf("  %s\n", path);
    }

    /*
     * The engine itself travels as ML/SE.BIN, loaded on demand by the stub
     * patched into AUTOEXEC.BIN. Copying it here means card preparation is
     * one command and cannot leave the two halves out of step -- a stale
     * SE.BIN against a newer AUTOEXEC.BIN is exactly the kind of mismatch
     * that is miserable to diagnose on the camera.
     */
    snprintf(path, sizeof path, "%s/ML/SE.BIN", root);
    if (copy_file("ml/build/SE.BIN", path) == 0) {
        printf("  %s\n", path);
    } else {
        fprintf(stderr,
                "filmdev: could not copy ml/build/SE.BIN -- run 'make payload' "
                "first, or copy it to ML/SE.BIN by hand\n");
    }

    printf("\n%d stocks copied. The camera lists whatever it finds in ML/FILM.\n", n);
    printf("Remember AUTOEXEC.BIN: cp ml/build/AUTOEXEC.BIN %s/\n", root);
    return 0;
}


/* ====================================================== contact sheet = */
/*
 * Every stock applied to one frame, in a grid.
 *
 * This renders the PREVIEW path, not the raw one, because that is what the
 * camera does unless told otherwise -- and because saturation, split tone
 * and halation only exist on that path. A sheet built from the raw develop
 * showed the curves and silently dropped everything else, so PORTRA's
 * whole character and SOUP 800's entire reason for existing were missing
 * from the one picture meant for deciding between stocks.
 *
 * It is also cheaper. The raw decode this used to do once was more
 * expensive than decoding the embedded preview once per stock.
 */

static void rgb_text(uint8_t *img, int w, int h, int x, int y,
                     const char *s, int rr, int gg, int bb, int sx, int sy)
{
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        int gi, col, row;
        if (ch >= 'a' && ch <= 'z') ch = (unsigned char)(ch - 'a' + 'A');
        gi = (ch < 0x20 || ch > 0x5F) ? 0 : ch - 0x20;
        for (col = 0; col < SE_FONT_W; col++) {
            uint8_t bits = se_ui_font[gi][col];
            for (row = 0; row < SE_FONT_H; row++) {
                int dx, dy;
                if (!(bits & (1u << row))) continue;
                for (dy = 0; dy < sy; dy++)
                for (dx = 0; dx < sx; dx++) {
                    int px = x + col * sx + dx, py = y + row * sy + dy;
                    if (px < 0 || py < 0 || px >= w || py >= h) continue;
                    img[(py * (size_t)w + px) * 3 + 0] = (uint8_t)rr;
                    img[(py * (size_t)w + px) * 3 + 1] = (uint8_t)gg;
                    img[(py * (size_t)w + px) * 3 + 2] = (uint8_t)bb;
                }
            }
        }
        x += (SE_FONT_W + 1) * sx;
    }
}

typedef struct { uint8_t *buf; int w, h, y; } tile_ctx;

/* The contact sheet's rows come through the look stage, which may hold
 * them back -- halation emits row k only once row k+radius has arrived --
 * so the tile is filled from the look's output, not the decoder's. */
typedef struct { tile_ctx t; se_look_run *run; } sheet_ctx;

static int sheet_emit(void *ctx, const uint8_t *row, int width)
{
    sheet_ctx *s = (sheet_ctx *)ctx;
    if (s->t.y < s->t.h)
        memcpy(s->t.buf + (size_t)s->t.y * s->t.w * 3, row, (size_t)width * 3);
    s->t.y++;
    return 0;
}

/* se_develop hands rows the same way the decoder does. */
static int sheet_raw_rows(void *ctx, const uint8_t *rgb8, int nrows, int width)
{
    sheet_ctx *s = (sheet_ctx *)ctx;
    (void)width;
    return se_look_rows(s->run, rgb8, nrows, sheet_emit, s) == SE_OK ? 0 : -1;
}

static int sheet_rows(void *ctx, const uint8_t *rgb, int nrows, int width)
{
    sheet_ctx *s = (sheet_ctx *)ctx;
    (void)width;
    return se_look_rows(s->run, rgb, nrows, sheet_emit, s) == SE_OK ? 0 : -1;
}

static void box_down(const uint8_t *src, int sw, int sh,
                     uint8_t *dst, int dw, int dh)
{
    int x, y, i, j, c;
    int fx = sw / dw, fy = sh / dh;
    if (fx < 1) fx = 1;
    if (fy < 1) fy = 1;
    for (y = 0; y < dh; y++)
    for (x = 0; x < dw; x++) {
        unsigned acc[3] = { 0, 0, 0 };
        int n = 0;
        for (j = 0; j < fy; j++)
        for (i = 0; i < fx; i++) {
            int sxp = x * fx + i, syp = y * fy + j;
            if (sxp >= sw || syp >= sh) continue;
            for (c = 0; c < 3; c++) acc[c] += src[((size_t)syp * sw + sxp) * 3 + c];
            n++;
        }
        for (c = 0; c < 3; c++)
            dst[((size_t)y * dw + x) * 3 + c] = (uint8_t)(n ? acc[c] / (unsigned)n : 0);
    }
}

/* ============================================================== looks = */
/*
 * One finished JPEG per stock, full size.
 *
 * The contact sheet is for choosing; this is for actually having the
 * pictures. Same two routes: the preview path, which is what the camera
 * does, or --from-raw, which demosaics the sensor data and grades that.
 */
typedef struct { se_jpeg *enc; se_look_run *run; int err; } looks_ctx;

static int looks_emit(void *ctx, const uint8_t *row, int width)
{
    looks_ctx *L = (looks_ctx *)ctx;
    (void)width;
    if (se_jpeg_write_rows(L->enc, row, 1) != SE_OK) { L->err = 1; return -1; }
    return 0;
}

static int looks_rows(void *ctx, const uint8_t *rgb, int nrows, int width)
{
    looks_ctx *L = (looks_ctx *)ctx;
    (void)width;
    return se_look_rows(L->run, rgb, nrows, looks_emit, L) == SE_OK ? 0 : -1;
}

static int looks_cmd(const char *in_path, const char *outdir, int from_raw)
{
    void        *filebuf;
    size_t       filelen;
    se_cr2_info  info;
    se_jdec_info ji;
    se_arena     arena;
    void        *amem;
    uint32_t     poff = 0, plen = 0;
    int          pw = 0, ph = 0, n, i, ow, oh;
    uint16_t    *raw = 0;
    char         path[1024];

    if (!scan_stocks()) { fprintf(stderr, "filmdev: no stocks\n"); return 1; }
    n = g_stock_n;

    filebuf = read_file(in_path, &filelen);
    if (!filebuf) { fprintf(stderr, "filmdev: cannot read %s\n", in_path); return 1; }
    if (se_cr2_parse(filebuf, filelen, &info) != SE_OK) {
        fprintf(stderr, "filmdev: %s is not a CR2 we understand\n", in_path); return 1;
    }
    if (se_cr2_find_preview(filebuf, filelen, &poff, &plen, &pw, &ph) != SE_OK ||
        se_jpeg_dec_info((char *)filebuf + poff, plen, &ji) != SE_OK) {
        fprintf(stderr, "filmdev: no usable embedded preview\n"); return 1;
    }

    amem = malloc(96u << 20);
    if (!amem) { fprintf(stderr, "filmdev: out of memory\n"); return 1; }
    se_arena_init(&arena, amem, 96u << 20);

    if (from_raw) {
        raw = calloc((size_t)info.raw_width * info.raw_height, sizeof(uint16_t));
        if (!raw) { fprintf(stderr, "filmdev: out of memory\n"); return 1; }
        fprintf(stderr, "  decoding the sensor data once...\n");
        if (se_cr2_decode(filebuf, filelen, &info, raw, &arena) != SE_OK) {
            fprintf(stderr, "filmdev: raw decode failed\n"); return 1;
        }
        se_cr2_measure_black(&info, raw);
        se_arena_reset(&arena);
        se_develop_out_size(&info, 1, &ow, &oh);
    } else {
        se_jpeg_scaled_size(ji.width, ji.height, 1, &ow, &oh);
    }

    snprintf(path, sizeof path, "%s/x", outdir);
    ensure_dir_for(path);

    for (i = 0; i < n; i++) {
        size_t    slen;
        void     *stext = read_file(g_stock_path[i], &slen);
        se_stock  st;
        se_look   lk;
        looks_ctx lc;
        se_jpeg_exif ex;
        static se_jpeg enc;
        FILE     *fo;
        char      nm[SE_STOCK_NAME], *q;
        int       k;

        if (!stext) continue;
        if (se_stock_parse((const char *)stext, slen, &st) != SE_OK) { free(stext); continue; }
        free(stext);

        /* the stock's own name, made safe for a filename */
        for (k = 0; st.name[k] && k < (int)sizeof nm - 1; k++)
            nm[k] = (st.name[k] == ' ') ? '_' : st.name[k];
        nm[k] = 0;
        for (q = nm; *q; q++) if (*q == '/' || *q == '\\') *q = '_';
        snprintf(path, sizeof path, "%s/%s.jpg", outdir, nm);

        se_look_from_stock(&st, 1, &lk);
        se_look_fit(&lk, ow, SE_LOOK_BUDGET);
        se_arena_reset(&arena);
        memset(&lc, 0, sizeof lc);
        if (se_look_begin(&lc.run, &lk, ow, &arena) != SE_OK) continue;

        fo = fopen(path, "wb");
        if (!fo) { fprintf(stderr, "filmdev: cannot write %s\n", path); continue; }

        memset(&ex, 0, sizeof ex);
        ex.make = info.make[0] ? info.make : "Canon";
        ex.model = info.model[0] ? info.model : "Canon EOS 5D";
        ex.software = st.name;
        ex.datetime = info.datetime[0] ? info.datetime : NULL;
        ex.exposure_time = info.exposure_time;
        ex.aperture = info.aperture;
        ex.iso = info.iso;
        {
            uint8_t *strip = SE_NEW(&arena, uint8_t, (size_t)8 * ow * 3);
            if (!strip || se_jpeg_begin(&enc, ow, oh, 92, strip, &ex,
                                        jpeg_sink, fo) != SE_OK) {
                fclose(fo); continue;
            }
        }
        lc.enc = &enc;

        if (from_raw) {
            se_dev_opts o; se_raw_src src; se_stock neutral;
            static se_lut1 flat[3];
            se_stock_init(&neutral);
            se_stock_bake(&neutral, flat);
            se_dev_opts_default(&o);
            o.scale = 1; o.tone = flat; o.dither = 1; o.grain_amount = 0;
            o.rows = looks_rows; o.rows_ctx = &lc;
            memset(&src, 0, sizeof src);
            src.mem = raw;
            if (se_develop(&info, &src, NULL, &o, &arena, NULL, NULL) != SE_OK) {
                fclose(fo); continue;
            }
        } else if (se_jpeg_decode_scaled((char *)filebuf + poff, plen, 1,
                                         &arena, looks_rows, &lc, &ji) != SE_OK) {
            fclose(fo); continue;
        }
        se_look_end(lc.run, looks_emit, &lc);
        se_jpeg_end(&enc);
        fclose(fo);
        fprintf(stderr, "  %-16s %s\n", st.name, path);
    }
    fprintf(stderr, "  %d stocks written to %s (%dx%d%s)\n",
            n, outdir, ow, oh, from_raw ? ", from the sensor data" : "");
    return 0;
}

static int contact_sheet(const char *in_path, const char *out_path,
                         int quiet, int from_raw, int cols_opt)
{
    void   *filebuf;
    size_t  filelen;
    se_cr2_info info;
    void   *amem;
    se_arena arena;
    int     n, i, tw, th, cols, rows, W, H;
    uint8_t *sheet, *tile, *small;
    int      qw, qh;
    FILE    *fo;
    uint32_t poff = 0, plen = 0;
    int      pw = 0, ph = 0;
    se_jdec_info ji;
    const int DSCALE = 2;
    se_cr2_info rawinfo;
    uint16_t   *raw = 0;
    int         rawscale = 4;

    if (!scan_stocks()) { fprintf(stderr, "filmdev: no stocks\n"); return 1; }
    n = g_stock_n;

    filebuf = read_file(in_path, &filelen);
    if (!filebuf) { fprintf(stderr, "filmdev: cannot read %s\n", in_path); return 1; }
    if (se_cr2_parse(filebuf, filelen, &info) != SE_OK) {
        fprintf(stderr, "filmdev: %s is not a CR2 we understand\n", in_path);
        return 1;
    }
    if (se_cr2_find_preview(filebuf, filelen, &poff, &plen, &pw, &ph) != SE_OK) {
        fprintf(stderr, "filmdev: %s has no embedded preview\n", in_path);
        return 1;
    }
    if (se_jpeg_dec_info((char *)filebuf + poff, plen, &ji) != SE_OK) {
        fprintf(stderr, "filmdev: the embedded preview will not decode\n");
        return 1;
    }

    amem = malloc(64u << 20);
    if (!amem) { fprintf(stderr, "filmdev: out of memory\n"); return 1; }
    se_arena_init(&arena, amem, 64u << 20);

    se_jpeg_scaled_size(ji.width, ji.height, DSCALE, &qw, &qh);
    tw = qw / 3; th = qh / 3;

    if (from_raw) {
        /* decoded once for all thirteen stocks; it is the expensive part */
        int rw, rh;
        rawinfo = info;
        raw = calloc((size_t)info.raw_width * info.raw_height, sizeof(uint16_t));
        if (!raw) { fprintf(stderr, "filmdev: out of memory\n"); return 1; }
        if (!quiet) fprintf(stderr, "  decoding the sensor data once...\n");
        if (se_cr2_decode(filebuf, filelen, &rawinfo, raw, &arena) != SE_OK) {
            fprintf(stderr, "filmdev: raw decode failed\n"); return 1;
        }
        se_cr2_measure_black(&rawinfo, raw);
        se_arena_reset(&arena);
        /* match the preview route's tile size as closely as the scales allow */
        for (rawscale = 1; rawscale <= 8; rawscale *= 2) {
            se_develop_out_size(&rawinfo, rawscale, &rw, &rh);
            if (rw <= qw) break;
        }
        se_develop_out_size(&rawinfo, rawscale, &qw, &qh);
        tw = qw / 3; th = qh / 3;
        W = 4 * tw;
    }

    /*
     * Pick the grid rather than assuming four across.
     *
     * Thirteen stocks in four columns leaves three black holes in the
     * bottom row, which reads as a rendering fault rather than a
     * sheet. Try the sensible widths and take the one that wastes
     * fewest cells, breaking ties toward the wider sheet because a
     * contact sheet is looked at in a row.
     */
    if (cols_opt > 0) {
        cols = cols_opt;
    } else {
        int best = 4, waste = n, c;
        for (c = 3; c <= 6 && c <= n; c++) {
            int w = (c - (n % c)) % c;
            if (w < waste || (w == waste && c > best)) { waste = w; best = c; }
        }
        cols = best;
    }
    rows = (n + cols - 1) / cols;
#define LABEL_H 20
    W = cols * tw; H = rows * (th + LABEL_H);

    sheet = calloc((size_t)W * H * 3, 1);
    tile  = malloc((size_t)qw * qh * 3);
    small = malloc((size_t)tw * th * 3);
    if (!sheet || !tile || !small) { fprintf(stderr, "filmdev: out of memory\n"); return 1; }

    /*
     * Is there any colour in the preview at all?
     *
     * The preview path grades the JPEG DIGIC already made, and DIGIC makes
     * it through the camera's Picture Style. With that set to Monochrome
     * the embedded preview is black and white -- and then every colour
     * simulation on the card collapses onto the same tonal variation and
     * the sheet is thirteen grey pictures. That is not a bug anywhere in
     * this project and there is nothing it can do about it, so it says so
     * rather than letting someone conclude the looks do not work.
     */
    if (!from_raw) {
        se_arena pa;
        se_look   nl;
        sheet_ctx   nc;
        int y, mx = 0;
        se_stock neutral;

        se_stock_init(&neutral);
        se_look_from_stock(&neutral, DSCALE, &nl);
        se_arena_init(&pa, amem, 64u << 20);
        nc.t.buf = tile; nc.t.w = qw; nc.t.h = qh; nc.t.y = 0; nc.run = 0;
        if (se_look_begin(&nc.run, &nl, qw, &pa) == SE_OK &&
            se_jpeg_decode_scaled((char *)filebuf + poff, plen, DSCALE, &pa,
                                  sheet_rows, &nc, &ji) == SE_OK) {
            se_look_end(nc.run, sheet_emit, &nc);
            for (y = 0; y < qw * qh * 3; y += 3) {
                int r = tile[y], g = tile[y+1], b = tile[y+2];
                int c = (r>g?(r>b?r:b):(g>b?g:b)) - (r<g?(r<b?r:b):(g<b?g:b));
                if (c > mx) mx = c;
            }
        }
        if (mx <= 16) {
            fprintf(stderr,
"\nfilmdev: this frame's embedded preview is black and white (peak chroma\n"
"         %d of 255). The camera's Picture Style was set to Monochrome, so\n"
"         DIGIC wrote a grey JPEG into the CR2 -- and the preview path,\n"
"         which is what the camera uses, has no colour to grade. Every\n"
"         stock will come out as the same picture with a different curve.\n\n"
"         Set the camera's Picture Style to Standard, Neutral or Faithful.\n"
"         The sensor data in this file IS colour; only the preview is not.\n\n"
"         To see the stocks on this frame anyway, add --from-raw. That\n"
"         develops the sensor data and then grades it, which the camera\n"
"         cannot do -- on the raw path it hands mosaiced samples to DIGIC\n"
"         and never sees RGB. Use it to choose a stock, not to predict a\n"
"         camera frame.\n\n", mx);
        }
    }

    for (i = 0; i < n; i++) {
        size_t slen;
        void  *stext = read_file(g_stock_path[i], &slen);
        se_stock st;
        se_look  lk;
        sheet_ctx sc;
        int cx = (i % cols) * tw, cy = (i / cols) * (th + LABEL_H);
        int y;

        if (!stext) continue;
        if (se_stock_parse((const char *)stext, slen, &st) != SE_OK) { free(stext); continue; }
        free(stext);

        /* The same budget the camera applies, so a stock whose halation the
         * camera would narrow is shown narrowed here too. */
        se_look_from_stock(&st, DSCALE, &lk);
        se_look_fit(&lk, qw, SE_LOOK_BUDGET);

        se_arena_reset(&arena);
        sc.t.buf = tile; sc.t.w = qw; sc.t.h = qh; sc.t.y = 0;
        sc.run = NULL;
        if (se_look_begin(&sc.run, &lk, qw, &arena) != SE_OK) continue;

        if (from_raw) {
            /*
             * Demosaic the sensor data with NO stock applied, then grade
             * the result. The curves have to come from exactly one place
             * or they are applied twice -- se_develop would bake them in
             * linear and se_look would apply them again in sRGB -- so the
             * develop runs neutral and the whole look lives in se_look.
             */
            se_dev_opts o;
            se_raw_src  src;
            se_stock    neutral;
            static se_lut1 flat[3];

            se_stock_init(&neutral);
            se_stock_bake(&neutral, flat);
            se_dev_opts_default(&o);
            o.scale = rawscale;
            o.tone  = flat;
            o.dither = 1;
            o.grain_amount = 0;          /* se_look does the grain */
            o.rows = sheet_raw_rows; o.rows_ctx = &sc;
            memset(&src, 0, sizeof src);
            src.mem = raw;
            if (se_develop(&rawinfo, &src, NULL, &o, &arena, NULL, NULL) != SE_OK)
                continue;
        } else {
            if (se_jpeg_decode_scaled((char *)filebuf + poff, plen, DSCALE, &arena,
                                      sheet_rows, &sc, &ji) != SE_OK) continue;
        }
        if (se_look_end(sc.run, sheet_emit, &sc) != SE_OK) continue;

        box_down(tile, qw, qh, small, tw, th);
        for (y = 0; y < th; y++)
            memcpy(sheet + ((size_t)(cy + y) * W + cx) * 3,
                   small + (size_t)y * tw * 3, (size_t)tw * 3);
        rgb_text(sheet, W, H, cx + 5, cy + th + 3, st.name, 240, 240, 240, 2, 2);
        if (!quiet) fprintf(stderr, "\r  %-14s %d/%d", st.name, i + 1, n);
    }
    if (!quiet) fprintf(stderr, "\r  %d stocks rendered        \n", n);

    ensure_dir_for(out_path);
    fo = fopen(out_path, "wb");
    if (!fo) { fprintf(stderr, "filmdev: cannot write %s\n", out_path); return 1; }
    {
        static se_jpeg j;
        uint8_t *strip = malloc((size_t)8 * W * 3);
        int y;
        se_jpeg_exif ex;
        memset(&ex, 0, sizeof ex);
        ex.make = info.make; ex.model = info.model;
        ex.software = "Soup Engine contact sheet";
        ex.datetime = info.datetime[0] ? info.datetime : NULL;
        if (!strip) return 1;
        if (se_jpeg_begin(&j, W, H, 90, strip, &ex, jpeg_sink, fo) != SE_OK) return 1;
        for (y = 0; y < H; y++)
            se_jpeg_write_rows(&j, sheet + (size_t)y * W * 3, 1);
        se_jpeg_end(&j);
        free(strip);
    }
    fclose(fo);
    printf("wrote %s  (%d x %d, %d stocks)\n", out_path, W, H, n);
    return 0;
}


/* ============================================================ batch == */
/*
 * Develop many frames with one stock. The stock is parsed and baked once;
 * each frame then costs a decode plus a develop.
 */
static int batch_develop(char **files, int nfiles, const char *stock_name,
                         const char *outdir, se_dev_opts base, int quiet)
{
    se_stock st;
    char     resolved[512];
    static se_lut1 tone[3];
    int      i, ok = 0, failed = 0;
    clock_t  t0 = clock();

    if (load_stock(stock_name, &st, resolved, sizeof resolved) != 0) return 1;
    se_stock_bake(&st, tone);
    printf("stock: %s - %s\n\n", st.name, st.desc);

    for (i = 0; i < nfiles; i++) {
        void   *filebuf;
        size_t  filelen;
        se_cr2_info info;
        uint16_t *raw;
        void   *amem;
        se_arena arena;
        se_dev_opts o = base;
        se_raw_src src;
        FILE   *fo;
        char    outpath[1024];
        size_t  need;
        const char *slash = strrchr(files[i], '/');
        const char *stem  = slash ? slash + 1 : files[i];
        const char *dot   = strrchr(stem, '.');
        int     sn = dot ? (int)(dot - stem) : (int)strlen(stem);

        filebuf = read_file(files[i], &filelen);
        if (!filebuf) {
            fprintf(stderr, "  %-24s cannot read\n", stem); failed++; continue;
        }
        if (se_cr2_parse(filebuf, filelen, &info) != SE_OK) {
            fprintf(stderr, "  %-24s not a CR2 we understand\n", stem);
            free(filebuf); failed++; continue;
        }

        need = se_develop_scratch_size(&info) + (1 << 20);
        raw  = calloc((size_t)info.raw_width * info.raw_height, sizeof(uint16_t));
        amem = malloc(need);
        if (!raw || !amem) { fprintf(stderr, "  out of memory\n"); return 1; }
        se_arena_init(&arena, amem, need);

        if (se_cr2_decode(filebuf, filelen, &info, raw, &arena) != SE_OK) {
            fprintf(stderr, "  %-24s decode failed\n", stem);
            free(filebuf); free(raw); free(amem); failed++; continue;
        }
        se_cr2_measure_black(&info, raw);
        se_arena_reset(&arena);

        o.tone = tone;
        o.stock_name = st.name;
        o.grain_amount = se_stock_grain(&st, SE_SRC_RAW);
        o.grain_size = 22; o.grain_frequency = 60;
        if (st.wb_preset > 0 && info.wb_presets_present)
            o.wb_override = info.wb_preset[st.wb_preset];

        snprintf(outpath, sizeof outpath, "%s/%.*s_%s.jpg", outdir, sn, stem, st.name);
        mkdir(outdir, 0755);
        fo = fopen(outpath, "wb");
        if (!fo) {
            fprintf(stderr, "  %-24s cannot write %s\n", stem, outpath);
            free(filebuf); free(raw); free(amem); failed++; continue;
        }
        memset(&src, 0, sizeof src);
        src.mem = raw;
        if (se_develop(&info, &src, NULL, &o, &arena, jpeg_sink, fo) != SE_OK) {
            fprintf(stderr, "  %-24s develop failed\n", stem);
            failed++;
        } else {
            int ow, oh;
            se_develop_out_size(&info, o.scale, &ow, &oh);
            if (!quiet) printf("  %-24s -> %s  (%dx%d)\n", stem,
                               strrchr(outpath,'/') ? strrchr(outpath,'/')+1 : outpath, ow, oh);
            ok++;
        }
        fclose(fo);
        free(filebuf); free(raw); free(amem);
    }

    printf("\n%d developed, %d failed, %.1f s total (%.2f s each)\n",
           ok, failed, (double)(clock() - t0) / CLOCKS_PER_SEC,
           ok ? (double)(clock() - t0) / CLOCKS_PER_SEC / ok : 0.0);
    return failed ? 1 : 0;
}



/* ============================================================ checkfir */
/*
 * Validate a Canon firmware file before it goes anywhere near the camera.
 *
 * Flashing is the one operation in this whole project that can genuinely
 * brick the body, and Canon no longer hosts the EOS 5D firmware, so the
 * file has to come from somewhere else. These are the checks that make any
 * source safe to use.
 *
 * The expected size is not a guess: Canon's own Windows installer carries
 * a manifest declaring 5d000111.fir as exactly 5,202,980 bytes. The header
 * layout is taken from the two known-good 5Dc .fir files in vendor/.
 */
#define FIR_5DC_111_SIZE 5202980

static int check_fir(const char *path)
{
    size_t len;
    uint8_t *d = read_file(path, &len);
    char ver[17];
    int  i, problems = 0;

    if (!d) { fprintf(stderr, "filmdev: cannot read %s\n", path); return 1; }

    printf("%s\n", path);
    printf("  size            : %zu bytes\n", len);

    if (len < 0x20) {
        printf("  VERDICT         : far too small to be a firmware file\n");
        free(d);
        return 1;
    }

    /* magic */
    if (d[0] == 0x13 && d[1] == 0x02 && d[2] == 0x00 && d[3] == 0x80) {
        printf("  header magic    : 13 02 00 80  (matches the EOS 5D)\n");
    } else {
        printf("  header magic    : %02x %02x %02x %02x  <- NOT an EOS 5D firmware header\n",
               d[0], d[1], d[2], d[3]);
        problems++;
    }

    /* version string, NUL padded to 16 bytes at offset 0x10 */
    for (i = 0; i < 16; i++) ver[i] = (char)(d[0x10 + i] ? d[0x10 + i] : 0);
    ver[16] = 0;
    printf("  version string  : \"%s\"\n", ver[0] ? ver : "(empty)");
    if (strcmp(ver, "1.1.1") != 0) {
        printf("                    <- expected \"1.1.1\"; Magic Lantern for the\n"
               "                       5D Classic supports no other version\n");
        problems++;
    }

    /* size against Canon's own manifest */
    if (len == FIR_5DC_111_SIZE) {
        printf("  size check      : exactly matches Canon's manifest for 5d000111.fir\n");
    } else {
        long diff = (long)len - (long)FIR_5DC_111_SIZE;
        printf("  size check      : %+ld bytes vs Canon's manifest (%d)\n",
               diff, FIR_5DC_111_SIZE);
        if (len > 100000) {
            printf("                    <- wrong size for the 1.1.1 update\n");
            problems++;
        } else {
            printf("                    <- this is small enough to be a Magic Lantern\n"
                   "                       installer .fir, not the Canon update\n");
            problems++;
        }
    }

    printf("\n");
    if (problems == 0) {
        printf("  VERDICT         : looks like the genuine EOS 5D 1.1.1 update.\n");
        printf("                    Copy it to the ROOT of a freshly formatted CF\n");
        printf("                    card, then update from the camera's menu.\n");
    } else {
        printf("  VERDICT         : %d problem%s found. DO NOT FLASH THIS FILE.\n",
               problems, problems == 1 ? "" : "s");
    }
    free(d);
    return problems ? 1 : 0;
}


/* =========================================================== checkcard */
/*
 * Diagnose "no update found" when a .fir is sitting on the card.
 *
 * The big one on this body is a catch-22: firmware 1.1.1 is the update
 * that ADDS high-capacity CF support. On 1.0.1 the camera wants a small
 * FAT16 card, so a modern card may not be readable -- you need the update
 * to read the card that holds the update.
 */
static int check_card(const char *root)
{
    DIR *d = opendir(root);
    struct dirent *e;
    struct statfs sfs;
    int  nfir = 0, nmeta = 0, problems = 0;
    char path[1024];

    if (!d) { fprintf(stderr, "filmdev: cannot open %s\n", root); return 1; }

    printf("%s\n\n", root);

    if (statfs(root, &sfs) == 0) {
        double gb = (double)sfs.f_blocks * (double)sfs.f_bsize / 1073741824.0;
        printf("  filesystem      : %s\n", sfs.f_fstypename);
        printf("  card size       : %.2f GB\n", gb);
        if (gb > 2.0) {
            printf("                    <- LARGER THAN 2 GB.\n"
                   "                       Firmware 1.1.1 is the update that ADDS\n"
                   "                       high-capacity CF support, so on 1.0.1 the\n"
                   "                       camera may not read this card at all.\n"
                   "                       Use a 2 GB or smaller card for the update.\n");
            problems++;
        }
        if (strstr(sfs.f_fstypename, "msdos") == NULL &&
            strstr(sfs.f_fstypename, "exfat") == NULL) {
            printf("                    <- not a FAT filesystem; the camera cannot read it\n");
            problems++;
        } else if (strstr(sfs.f_fstypename, "exfat") != NULL) {
            printf("                    <- exFAT. This camera needs FAT16/FAT32.\n");
            problems++;
        }
    }
    printf("\n");

    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        int is_fir = (n > 4 &&
                      (strcasecmp(e->d_name + n - 4, ".fir") == 0));
        if (e->d_name[0] == '.') {
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) nmeta++;
            /* a macOS resource fork shadowing a real .fir */
            if (n > 2 && e->d_name[0] == '.' && e->d_name[1] == '_') continue;
            continue;
        }
        if (is_fir) {
            nfir++;
            printf("  found           : %s\n", e->d_name);
            if (strcmp(e->d_name, "5d000111.fir") != 0)
                printf("                    <- the camera expects 5d000111.fir;\n"
                       "                       rename it to be safe\n");
            snprintf(path, sizeof path, "%s/%s", root, e->d_name);
            printf("\n");
            if (check_fir(path) != 0) problems++;
            printf("\n");
        }
    }
    closedir(d);

    if (!nfir) {
        printf("  found           : NO .fir FILE IN THE ROOT OF THIS CARD\n");
        printf("                    It must be at the top level, not in a folder.\n");
        problems++;
    }
    if (nmeta) {
        printf("  macOS metadata  : %d hidden entr%s (.DS_Store, ._ forks, "
               ".Spotlight-V100)\n", nmeta, nmeta == 1 ? "y" : "ies");
        printf("                    Usually harmless, but clean them off and\n"
               "                    format the card IN THE CAMERA to be sure.\n");
    }

    printf("\n  %s\n", problems ? "Fix the items marked above." :
           "Card looks ready. Update from the camera's last menu tab.");
    return problems ? 1 : 0;
}

/* ============================================================ stress == */
/*
 * The CR2 reader is the most complex parser here and the one most likely
 * to meet a bad file: a card pulled mid-write, a bad block, a half-copied
 * frame. It must reject all of that without reading outside the buffer.
 * Run this under AddressSanitizer -- that is the point of it.
 */
static int stress_cr2(const char *path, int rounds)
{
    size_t  len;
    uint8_t *orig = read_file(path, &len);
    uint8_t *work;
    uint16_t *raw;
    void   *amem;
    size_t  arena_sz;
    int     i, parsed = 0, decoded = 0, rejected = 0;
    int     n_trunc, n_hdr, n_ent, total;
    unsigned seed = 1;

    if (rounds < 10) rounds = 10;
    n_trunc = rounds / 2;
    n_hdr   = rounds;
    n_ent   = rounds / 4;
    total   = n_trunc + n_hdr + n_ent;

    if (!orig) { fprintf(stderr, "filmdev: cannot read %s\n", path); return 1; }

    work = malloc(len);
    if (!work) return 1;

    /* size the buffers from the intact file, then abuse copies of it */
    {
        se_cr2_info info;
        if (se_cr2_parse(orig, len, &info) != SE_OK) {
            fprintf(stderr, "filmdev: %s does not parse intact\n", path);
            return 1;
        }
        raw = calloc((size_t)info.raw_width * info.raw_height, sizeof(uint16_t));
        arena_sz = se_develop_scratch_size(&info) + (1 << 20);
        amem = malloc(arena_sz);
        if (!raw || !amem) return 1;
    }

    se_set_log(NULL, NULL);
    printf("stressing the CR2 reader with %s (%.1f MB), %d variants\n",
           path, len / 1048576.0, total);
    fflush(stdout);

    /* --- truncation at every scale ---------------------------------- */
    for (i = 0; i < n_trunc; i++) {
        se_cr2_info info;
        se_arena a;
        size_t cut = (size_t)((double)len * ((double)i / (double)n_trunc));
        memcpy(work, orig, cut);
        if (se_cr2_parse(work, cut, &info) == SE_OK) {
            parsed++;
            se_arena_init(&a, amem, arena_sz);
            if (se_cr2_decode(work, cut, &info, raw, &a) == SE_OK) decoded++;
        } else rejected++;
    }

    /* --- bit rot in the header, where the structure lives ------------ */
    for (i = 0; i < n_hdr; i++) {
        se_cr2_info info;
        se_arena a;
        size_t hdr = len < 65536 ? len : 65536;
        int k, nmut;
        memcpy(work, orig, len);
        seed = seed * 1103515245u + 12345u;
        nmut = 1 + (int)((seed >> 16) % 12);
        for (k = 0; k < nmut; k++) {
            seed = seed * 1103515245u + 12345u;
            work[(seed >> 8) % hdr] = (uint8_t)(seed >> 20);
        }
        if (se_cr2_parse(work, len, &info) == SE_OK) {
            parsed++;
            se_arena_init(&a, amem, arena_sz);
            if (se_cr2_decode(work, len, &info, raw, &a) == SE_OK) decoded++;
        } else rejected++;
    }

    /* --- bit rot in the entropy data -------------------------------- */
    for (i = 0; i < n_ent; i++) {
        se_cr2_info info;
        se_arena a;
        int k;
        memcpy(work, orig, len);
        for (k = 0; k < 20; k++) {
            seed = seed * 1103515245u + 12345u;
            work[len / 2 + (seed >> 8) % (len / 2)] = (uint8_t)(seed >> 20);
        }
        if (se_cr2_parse(work, len, &info) == SE_OK) {
            parsed++;
            se_arena_init(&a, amem, arena_sz);
            if (se_cr2_decode(work, len, &info, raw, &a) == SE_OK) decoded++;
        } else rejected++;
    }

    /* --- an arena far too small must fail cleanly, not overrun ------- */
    {
        se_cr2_info info;
        size_t tiny;
        for (tiny = 0; tiny < 70000; tiny += 997) {
            se_arena a;
            if (se_cr2_parse(orig, len, &info) != SE_OK) break;
            se_arena_init(&a, amem, tiny);
            se_cr2_decode(orig, len, &info, raw, &a);
        }
    }

    se_set_log(log_cb, NULL);
    printf("  %d damaged variants: %d parsed, %d of those decoded, %d rejected\n",
           total, parsed, decoded, rejected);
    printf("  71 undersized arenas handled\n");
    printf("  no crash, no out-of-bounds access\n");
    free(orig); free(work); free(raw); free(amem);
    return 0;
}

/* ------------------------------------------------------------ selftest */
/*
 * Properties the engine must hold regardless of input. These catch the
 * class of bug that is nearly impossible to see by eye on a photograph.
 */

static int fail_count;
static void check(int ok, const char *what)
{
    printf("  [%s] %s\n", ok ? "pass" : "FAIL", what);
    if (!ok) fail_count++;
}

static int test_debayer_flat(void)
{
    /*
     * Every Malvar-He-Cutler kernel has taps summing to 16/16, so a flat
     * CFA field must demosaic to exactly that value in all three channels.
     * Any arithmetic slip in the kernels shows up here immediately.
     */
    enum { W = 64, ROWS = 8, BW = W + 4, BH = ROWS + 2 * SE_DEBAYER_HALO };
    static uint16_t band[BH * BW];
    static uint16_t rgb[ROWS * W * 3];
    int phase, i, worst = 0;

    for (phase = 0; phase < 4; phase++) {
        for (i = 0; i < BH * BW; i++) band[i] = 20000;
        se_debayer_band(band, BW, BW, 2, W, ROWS, phase, rgb);
        for (i = 0; i < ROWS * W * 3; i++) {
            int d = (int)rgb[i] - 20000;
            if (d < 0) d = -d;
            if (d > worst) worst = d;
        }
    }
    printf("        worst flat-field deviation: %d counts of 20000\n", worst);
    return worst == 0;
}

static int test_debayer_phase(void)
{
    /*
     * Put a single bright pixel on a known CFA site and check it lands in
     * the right channel. This is what catches a phase-mapping inversion,
     * which otherwise just looks like "the colours are a bit odd".
     */
    enum { W = 32, ROWS = 8, BW = W + 4, BH = ROWS + 2 * SE_DEBAYER_HALO };
    static uint16_t band[BH * BW];
    static uint16_t rgb[ROWS * W * 3];
    int phase, ok = 1;

    for (phase = 0; phase < 4; phase++) {
        int x, y, i, cx = 10, cy = 4;
        int expect;
        for (i = 0; i < BH * BW; i++) band[i] = 0;
        /* colour of output pixel (cx, cy) under this phase */
        {
            int X = cx + (phase & 1), Y = cy + ((phase >> 1) & 1);
            expect = (Y & 1) ? ((X & 1) ? 2 : 1) : ((X & 1) ? 1 : 0);
        }
        band[(cy + SE_DEBAYER_HALO) * BW + (cx + 2)] = 40000;
        se_debayer_band(band, BW, BW, 2, W, ROWS, phase, rgb);
        x = cx; y = cy;
        {
            const uint16_t *p = rgb + ((size_t)y * W + x) * 3;
            /* the site's own channel gets the full value, unfiltered */
            if (p[expect] != 40000) {
                printf("        phase %d: expected channel %d to be 40000, got %d\n",
                       phase, expect, p[expect]);
                ok = 0;
            }
        }
    }
    return ok;
}

static int test_lut_identity(void)
{
    static uint8_t mem[4 << 20];
    se_arena a;
    se_lut3  lut;
    uint16_t px[3 * 64];
    int i, worst = 0;
    unsigned seed = 12345;

    se_arena_init(&a, mem, sizeof mem);
    if (se_lut3_identity(&lut, 33, &a) != SE_OK) return 0;

    for (i = 0; i < 3 * 64; i++) {
        seed = seed * 1103515245u + 12345u;
        px[i] = (uint16_t)(seed >> 16);
    }
    {
        uint16_t ref[3 * 64];
        memcpy(ref, px, sizeof ref);
        se_lut3_apply_row(&lut, px, 64);
        for (i = 0; i < 3 * 64; i++) {
            int d = (int)px[i] - (int)ref[i];
            if (d < 0) d = -d;
            if (d > worst) worst = d;
        }
    }
    printf("        worst identity-LUT error: %d of 65535\n", worst);
    return worst <= 2;
}

static int test_lut_neutral_axis(void)
{
    /*
     * Tetrahedral interpolation was chosen over trilinear specifically so
     * that grey stays grey. Verify that on a non-trivial grade.
     */
    static uint8_t mem[4 << 20];
    se_arena a;
    se_lut3  lut;
    se_grade g;
    uint16_t px[3 * 256];
    int i, worst = 0;

    se_arena_init(&a, mem, sizeof mem);
    se_grade_init(&g);
    g.contrast = 40.0f; g.shadows = -30.0f; g.saturation = 35.0f;
    g.hue[2] = -30.0f;  g.sat[2] = -10.0f;
    g.master.n = 3;
    g.master.x[0] = 0;   g.master.y[0] = 8;
    g.master.x[1] = 128; g.master.y[1] = 134;
    g.master.x[2] = 255; g.master.y[2] = 250;
    if (se_grade_bake(&g, 33, &lut, &a) != SE_OK) return 0;

    for (i = 0; i < 256; i++) {
        uint16_t v = (uint16_t)(i * 257);
        px[i * 3 + 0] = px[i * 3 + 1] = px[i * 3 + 2] = v;
    }
    se_lut3_apply_row(&lut, px, 256);
    for (i = 0; i < 256; i++) {
        int mx = px[i*3], mn = px[i*3], c;
        for (c = 1; c < 3; c++) {
            if (px[i*3+c] > mx) mx = px[i*3+c];
            if (px[i*3+c] < mn) mn = px[i*3+c];
        }
        if (mx - mn > worst) worst = mx - mn;
    }
    printf("        worst grey-axis channel spread: %d of 65535\n", worst);
    return worst < 600;
}

static int test_srgb_roundtrip(void)
{
    static se_lut1 enc, dec;
    int i, worst = 0;
    se_lut1_srgb_encode(&enc);
    se_lut1_srgb_decode(&dec);
    for (i = 0; i < 65536; i += 7) {
        int e = se_lut1_eval(&enc, (uint16_t)i);
        int b = se_lut1_eval(&dec, (uint16_t)e);
        int d = b - i;
        if (d < 0) d = -d;
        if (d > worst) worst = d;
    }
    printf("        worst sRGB encode/decode round trip: %d of 65535\n", worst);
    return worst < 300;   /* 4096-entry tables, so some slack is expected */
}

static int g_sink_bytes;
static int g_sink_soi, g_sink_eoi, g_sink_last;
static int count_sink(void *ctx, const uint8_t *d, size_t n)
{
    size_t i;
    (void)ctx;
    for (i = 0; i < n; i++) {
        g_sink_last = (g_sink_last << 8) | d[i];
        g_sink_bytes++;
        if (g_sink_bytes == 2 && (g_sink_last & 0xffff) == 0xffd8) g_sink_soi = 1;
    }
    g_sink_eoi = ((g_sink_last & 0xffff) == 0xffd9);
    return 0;
}

static int test_jpeg_structure(void)
{
    enum { W = 61, H = 37 };      /* deliberately not multiples of 8 */
    static uint8_t rgb[H * W * 3];
    static uint8_t strip[8 * W * 3];
    static se_jpeg j;            /* ~5 KB, keep it off the stack */
    se_jpeg_exif   ex;
    int x, y;

    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            rgb[(y * W + x) * 3 + 0] = (uint8_t)(x * 4);
            rgb[(y * W + x) * 3 + 1] = (uint8_t)(y * 6);
            rgb[(y * W + x) * 3 + 2] = (uint8_t)(128);
        }

    memset(&ex, 0, sizeof ex);
    ex.make = "Canon"; ex.model = "Canon EOS 5D";
    ex.software = "Soup Engine TEST";
    ex.datetime = "2026:10:18 12:00:00";
    ex.exposure_time = 1.0f / 250.0f; ex.aperture = 8.0f; ex.iso = 100;

    g_sink_bytes = g_sink_soi = g_sink_eoi = g_sink_last = 0;
    if (se_jpeg_begin(&j, W, H, 90, strip, &ex, count_sink, NULL) != SE_OK) return 0;
    if (se_jpeg_write_rows(&j, rgb, H) != SE_OK) return 0;
    if (se_jpeg_end(&j) != SE_OK) return 0;

    printf("        %dx%d encoded to %d bytes, SOI=%d EOI=%d\n",
           W, H, g_sink_bytes, g_sink_soi, g_sink_eoi);
    return g_sink_soi && g_sink_eoi && g_sink_bytes > 200;
}

static int test_ui_navigation(void)
{
    /*
     * Walk the menu with pseudo-random key presses and check two things
     * the camera cannot tell us about: that no state ever leaves its
     * valid range, and that drawing never writes outside the overlay
     * buffer. A stray write there lands in Canon's memory.
     */
    enum { GUARD = 64 };
    static uint8_t mem[GUARD + SE_UI_FB_SIZE + GUARD];
    se_ui_fb fb;
    se_ui    u;
    unsigned seed = 7;
    int      i, n, ok = 1;

    static const se_ui_stock sl[] = {
        { "A", "SHORT" },
        { "VERYLONGSTOCKNAME", "A DESCRIPTION THAT IS DELIBERATELY QUITE LONG INDEED" },
        { "MATTE", "YOUR LIGHTROOM EDIT, FADED AND COOL" },
    };
    n = (int)(sizeof sl / sizeof sl[0]);
    memset(mem, 0xA5, sizeof mem);
    fb.px = mem + GUARD;

    se_ui_init(&u);
    se_ui_set_stocks(&u, sl, n);
    for (i = 0; i < 20000; i++) {
        se_ui_key k = (se_ui_key)(seed >> 28 & 7);
        seed = seed * 1103515245u + 12345u;
        se_ui_key_press(&u, k);

        if (u.screen < 0 || u.screen >= SE_SCREEN_COUNT) {
            printf("        screen out of range: %d\n", (int)u.screen); ok = 0; break;
        }
        if (u.stock < 0 || u.stock >= n) {
            printf("        stock out of range: %d\n", u.stock); ok = 0; break;
        }
        if (u.setting < 0 || u.setting >= SE_SET_COUNT) {
            printf("        setting out of range: %d\n", u.setting); ok = 0; break;
        }
        {
            int j;
            for (j = 0; j < SE_SET_COUNT; j++)
                if (u.value[j] < 0 || u.value[j] > 16) {
                    printf("        value[%d] out of range: %d\n", j, u.value[j]);
                    ok = 0;
                }
            if (!ok) break;
        }
        se_ui_draw(&u, &fb);
    }

    for (i = 0; i < GUARD; i++) {
        if (mem[i] != 0xA5 || mem[GUARD + SE_UI_FB_SIZE + i] != 0xA5) {
            printf("        overlay buffer overrun at guard byte %d\n", i);
            ok = 0;
            break;
        }
    }
    printf("        walked 20000 key presses across every screen\n");
    return ok;
}

static int test_ui_coverage(void)
{
    /* Every stock and every setting value must render without tripping
     * the guards, including the longest names and values. */
    enum { GUARD = 64 };
    static uint8_t mem[GUARD + SE_UI_FB_SIZE + GUARD];
    se_ui_fb fb;
    se_ui    u;
    int      n, i, j, v, scr;

    static const se_ui_stock sl[] = {
        { "A", "SHORT" },
        { "VERYLONGSTOCKNAME", "A DESCRIPTION THAT IS DELIBERATELY QUITE LONG INDEED" },
    };
    n = (int)(sizeof sl / sizeof sl[0]);
    memset(mem, 0x5A, sizeof mem);
    fb.px = mem + GUARD;

    /* zero stocks must render too: a card with no ML/FILM directory */
    for (scr = 0; scr < SE_SCREEN_COUNT; scr++) {
        se_ui_init(&u);
        u.screen = (se_ui_screen)scr;
        se_ui_draw(&u, &fb);
    }

    for (scr = 0; scr < SE_SCREEN_COUNT; scr++)
    for (i = 0; i < n; i++)
    for (j = 0; j < SE_SET_COUNT; j++)
    for (v = 0; v <= 16; v++) {
        se_ui_init(&u);
        se_ui_set_stocks(&u, sl, n);
        u.screen = (se_ui_screen)scr;
        u.stock = i;
        u.setting = j;
        u.value[j] = v;
        u.percent = (v * 7) % 101;
        u.total = 99; u.done = 98;
        u.last_seconds = 99999;
        snprintf(u.file, sizeof u.file, "IMG_9999.CR2");
        se_ui_draw(&u, &fb);
    }
    for (i = 0; i < GUARD; i++)
        if (mem[i] != 0x5A || mem[GUARD + SE_UI_FB_SIZE + i] != 0x5A) {
            printf("        overlay buffer overrun at guard byte %d\n", i);
            return 0;
        }
    printf("        rendered every stock x setting x value combination\n");
    return 1;
}


/*
 * The text parsers all read files off a card. A truncated write, a bad
 * block or a hand-edited file must produce an error, never a crash and
 * never a read outside the buffer -- on the camera there is no MMU to
 * catch it, so a stray read lands in Canon's memory.
 */
static unsigned g_fuzz_seed = 0x5f3759df;
static unsigned fuzz_rand(void)
{
    g_fuzz_seed = g_fuzz_seed * 1103515245u + 12345u;
    return g_fuzz_seed >> 8;
}

static int fuzz_parsers(void)
{
    /* the parsers log every rejection; 60000 of those is not useful output */
    se_set_log(NULL, NULL);

    static const char *seeds[] = {
        "NAME X\nCURVE 0,0 255,255\n",
        "NAME VELVIA\nDESC D\nWB DAYLIGHT\nEXPOSURE +0.5\nGRAIN 20\n"
        "CURVE 0,0 128,130 255,255\nCURVE_R 0,0 255,255\n",
        "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n",
        "TITLE \"x\"\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\nLUT_3D_SIZE 2\n",
        "<x:xmpmeta><rdf:RDF><rdf:Description crs:Exposure2012=\"+1.00\" "
        "crs:Shadows2012=\"-60\" crs:WhiteBalance=\"Daylight\">"
        "<crs:ToneCurvePV2012><rdf:Seq><rdf:li>0, 11</rdf:li>"
        "<rdf:li>255, 238</rdf:li></rdf:Seq></crs:ToneCurvePV2012>"
        "</rdf:Description></rdf:RDF></x:xmpmeta>",
        "NAME A\nCURVE 9999,-9999 0,0 300,300 1e9,1e9\n",
        "LUT_3D_SIZE 999999\n",
        "LUT_3D_SIZE 0\n",
        "CURVE_R\nCURVE_G\nCURVE_B\nNAME\n",
        "",
    };
    static char buf[8192];
    static uint8_t arena_mem[2 << 20];
    int iter, i;

    for (iter = 0; iter < 60000; iter++) {
        const char *src = seeds[iter % (int)(sizeof seeds / sizeof seeds[0])];
        size_t slen = strlen(src);
        size_t len;
        int    mode = (int)(fuzz_rand() % 4);

        /* truncate, corrupt bytes, or append garbage */
        len = slen ? (fuzz_rand() % (slen + 1)) : 0;
        if (mode == 0 && slen) len = slen;              /* intact */
        if (len > sizeof buf - 1) len = sizeof buf - 1;
        memcpy(buf, src, len);
        if (mode == 2) {
            int k, nmut = 1 + (int)(fuzz_rand() % 8);
            for (k = 0; k < nmut && len; k++)
                buf[fuzz_rand() % len] = (char)(fuzz_rand() & 0xff);
        }
        if (mode == 3) {
            size_t extra = fuzz_rand() % 64;
            while (extra-- && len < sizeof buf - 1)
                buf[len++] = (char)(fuzz_rand() & 0xff);
        }
        buf[len] = 0;

        {
            se_stock st;
            se_stock_parse(buf, len, &st);
            /* whatever comes back, the curves must be sane */
            if (st.master.n < 0 || st.master.n > SE_CURVE_MAX_PTS ||
                st.r.n < 0 || st.r.n > SE_CURVE_MAX_PTS ||
                st.wb_preset < -1 || st.wb_preset >= SE_WB_COUNT)
                { se_set_log(log_cb, NULL); return 0; }
        }
        {
            se_arena a;
            se_lut3  lut;
            se_arena_init(&a, arena_mem, sizeof arena_mem);
            if (se_lut3_parse_cube(buf, len, &lut, &a) == SE_OK) {
                if (lut.size < 2 || lut.size > SE_LUT3_MAX_SIZE || !lut.data)
                    { se_set_log(log_cb, NULL); return 0; }
            }
        }
        {
            se_grade    g;
            se_xmp_info xi;
            if (se_xmp_parse(buf, len, &g, &xi) == SE_OK) {
                if (g.master.n < 0 || g.master.n > SE_CURVE_MAX_PTS ||
                    xi.unsupported_n < 0 || xi.unsupported_n > SE_XMP_UNSUP_MAX)
                    { se_set_log(log_cb, NULL); return 0; }
            }
        }
    }

    /* a parsed stock must always bake to something monotonic and in range */
    for (i = 0; i < (int)(sizeof seeds / sizeof seeds[0]); i++) {
        se_stock st;
        static se_lut1 tone[3];
        int c, k;
        if (se_stock_parse(seeds[i], strlen(seeds[i]), &st) != SE_OK) continue;
        se_stock_bake(&st, tone);
        for (c = 0; c < 3; c++)
            for (k = 1; k < SE_LUT1_SIZE; k++)
                if (tone[c].v[k] + 1 < tone[c].v[k - 1]) {
                    se_set_log(log_cb, NULL);
                    printf("        stock %d channel %d is not monotonic at %d\n", i, c, k);
                    return 0;
                }
    }
    se_set_log(log_cb, NULL);
    printf("        60000 mutated inputs through three parsers, all survived\n");
    return 1;
}

static int test_stock_roundtrip(void)
{
    /* The values a stock file states must be the values that come back. */
    static const char *src =
        "# comment\n"
        "NAME      VELVIA\n"
        "DESC      DENSE BLACKS\n"
        "WB        DAYLIGHT\n"
        "EXPOSURE  +0.50\n"
        "GRAIN     12\n"
        "CURVE     0,0 128,140 255,255\n"
        "CURVE_B   0,10 255,250\n"
        "UNKNOWN_KEY whatever\n";
    se_stock st;
    static se_lut1 tone[3];
    int ok = 1;

    if (se_stock_parse(src, strlen(src), &st) != SE_OK) {
        printf("        parse failed\n"); return 0;
    }
    if (strcmp(st.name, "VELVIA") != 0)  { printf("        name  = '%s'\n", st.name); ok = 0; }
    if (strcmp(st.desc, "DENSE BLACKS") != 0) { printf("        desc  = '%s'\n", st.desc); ok = 0; }
    if (st.wb_preset != SE_WB_DAYLIGHT)  { printf("        wb    = %d\n", st.wb_preset); ok = 0; }
    if (st.exposure < 0.49f || st.exposure > 0.51f) { printf("        exp   = %g\n", st.exposure); ok = 0; }
    if (st.grain < 11.9f || st.grain > 12.1f) { printf("        grain = %g\n", st.grain); ok = 0; }
    if (st.master.n != 3) { printf("        master has %d points\n", st.master.n); ok = 0; }
    if (st.b.n != 2)      { printf("        blue has %d points\n", st.b.n); ok = 0; }
    if (st.r.n != 0 || st.g.n != 0) { printf("        r/g should be identity\n"); ok = 0; }

    /*
     * The [RAW] block: the two paths must be able to disagree, and the
     * things the raw path physically cannot do must be refused there
     * rather than quietly accepted and ignored.
     */
    {
        static const char *rs =
            "NAME      SPLIT\n"
            "CURVE     0,0 128,160 255,255\n"
            "EXPOSURE  +0.50\n"
            "GRAIN     40\n"
            "SAT       150\n"
            "[RAW]\n"
            "CURVE     0,0 128,132 255,255\n"
            "EXPOSURE  -0.25\n"
            "GRAIN     12\n"
            "SAT       200\n"
            "HALATION  50 200 10\n";
        se_stock r2;
        uint8_t  jm[3][256];
        static se_lut1 rt[3];

        if (se_stock_parse(rs, strlen(rs), &r2) != SE_OK) {
            printf("        [RAW] stock failed to parse\n"); ok = 0;
        } else {
            if (se_stock_grain(&r2, SE_SRC_JPEG) != 40.0f ||
                se_stock_grain(&r2, SE_SRC_RAW)  != 12.0f) {
                printf("        grain did not split per source: jpeg %g raw %g\n",
                       se_stock_grain(&r2, SE_SRC_JPEG),
                       se_stock_grain(&r2, SE_SRC_RAW)); ok = 0;
            }
            if (!r2.raw.exposure_set || r2.raw.exposure > -0.24f) {
                printf("        [RAW] EXPOSURE did not take\n"); ok = 0;
            }
            if (r2.sat != 150.0f) {
                printf("        [RAW] SAT leaked into the common block: %g\n",
                       r2.sat); ok = 0;
            }
            if (r2.hal_amount != 0.0f) {
                printf("        [RAW] HALATION leaked into the common block\n");
                ok = 0;
            }
            /* and the two bakes must actually differ */
            se_stock_bake_srgb8(&r2, jm);
            se_stock_bake(&r2, rt);
            if (jm[0][128] < 170) {
                printf("        jpeg bake ignored the common curve: %d\n",
                       jm[0][128]); ok = 0;
            }
        }
    }

    se_stock_bake(&st, tone);
    /* blue lifts its floor, so black must not stay at zero */
    if (tone[2].v[0] == 0) { printf("        blue floor did not lift\n"); ok = 0; }
    /* exposure is +0.5 stop, so mid grey must land brighter than input */
    if (tone[1].v[SE_LUT1_SIZE / 2] <= 32768) {
        printf("        +0.5 EV did not brighten: %d\n", tone[1].v[SE_LUT1_SIZE / 2]);
        ok = 0;
    }
    if (ok) printf("        every stated field round trips, curves bake as declared\n");
    return ok;
}

static int test_curve_monotonic(void)
{
    /*
     * Monotone cubic interpolation is the whole reason for using Hermite
     * with Fritsch-Carlson tangents rather than Catmull-Rom. If a curve
     * ever reverses, a smooth gradient gets a visible band in it.
     */
    se_curve c;
    int iter, worst_rev = 0;

    for (iter = 0; iter < 4000; iter++) {
        int i, n = 2 + (int)(fuzz_rand() % (SE_CURVE_MAX_PTS - 2));
        int prev_x = 0;
        float last = -1.0f;
        c.n = n;
        for (i = 0; i < n; i++) {
            prev_x = prev_x + 1 + (int)(fuzz_rand() % (240 / n));
            c.x[i] = (uint8_t)(prev_x > 255 ? 255 : prev_x);
            c.y[i] = (uint8_t)(fuzz_rand() & 0xff);
        }
        /* a curve with non-increasing x is malformed input; skip those */
        for (i = 1; i < n; i++) if (c.x[i] <= c.x[i-1]) { c.n = 0; break; }
        if (!c.n) continue;
        /* monotone interpolation only guarantees monotonic OUTPUT for
         * monotonic input, so sort y to make the property testable */
        for (i = 1; i < n; i++) {
            int j;
            for (j = i; j > 0 && c.y[j] < c.y[j-1]; j--) {
                uint8_t t = c.y[j]; c.y[j] = c.y[j-1]; c.y[j-1] = t;
            }
        }
        for (i = 0; i <= 512; i++) {
            float x = (float)i / 512.0f;
            float v = se_curve_eval(&c, x);
            if (v < -0.001f || v > 1.001f) {
                printf("        curve left [0,1]: %g at x=%g\n", v, x);
                return 0;
            }
            if (v + 0.002f < last) worst_rev++;
            last = v;
        }
    }
    printf("        4000 random monotonic curves stayed in range, %d reversals\n", worst_rev);
    return worst_rev == 0;
}


/*
 * A neutral subject pushed past the sensor's saturation point must render
 * neutral. It does, without any highlight-recovery stage: a channel that
 * saturates earlier in raw counts carries a proportionally larger white
 * balance gain, and the two cancel. This test exists because that is
 * surprising enough to be worth pinning down.
 */
static se_cr2_info g_hl_info;
static int         g_hl_level;

static int hl_rows(void *ctx, int y, int n, uint16_t *dst)
{
    int i, x;
    (void)ctx;
    for (i = 0; i < n; i++) {
        uint16_t *r = dst + (size_t)i * g_hl_info.raw_width;
        int ry = y + i;
        for (x = 0; x < g_hl_info.raw_width; x++) {
            int isR = !(x & 1) && !(ry & 1);
            int isB =  (x & 1) &&  (ry & 1);
            double g = isR ? (2001.0 / 1024.0) : isB ? (1601.0 / 1024.0) : 1.0;
            double v = (double)g_hl_level / g + g_hl_info.black_cfa[((ry & 1) << 1) | (x & 1)];
            if (v > g_hl_info.white) v = g_hl_info.white;   /* the sensor clips */
            r[x] = (uint16_t)v;
        }
    }
    return 0;
}

static int g_hl_max[3];
static int hl_sink(void *ctx, const uint8_t *rgb8, int nrows, int width)
{
    int i, c;
    (void)ctx;
    for (i = 0; i < nrows * width; i++)
        for (c = 0; c < 3; c++)
            if (rgb8[i * 3 + c] > g_hl_max[c]) g_hl_max[c] = rgb8[i * 3 + c];
    return 0;
}

static int test_highlight_neutral(void)
{
    static uint8_t arena_mem[4 << 20];
    static const float m[9] = { 0.6347f,-0.0479f,-0.0972f, -0.8297f,1.5954f,0.2480f,
                                -0.1968f,0.2131f,0.7649f };
    int lv, worst = 0;

    memset(&g_hl_info, 0, sizeof g_hl_info);
    g_hl_info.raw_width = 128; g_hl_info.raw_height = 32;
    g_hl_info.crop_right = 127; g_hl_info.crop_bottom = 31;
    g_hl_info.active_width = 128; g_hl_info.active_height = 32;
    g_hl_info.bits = 12; g_hl_info.white = 3692; g_hl_info.black = 175;
    g_hl_info.black_cfa[0] = 157; g_hl_info.black_cfa[1] = 187;
    g_hl_info.black_cfa[2] = 187; g_hl_info.black_cfa[3] = 172;
    g_hl_info.black_measured = 1;
    g_hl_info.wb[0] = 2001; g_hl_info.wb[1] = 1024;
    g_hl_info.wb[2] = 1601; g_hl_info.wb[3] = 1024;
    g_hl_info.wb_present = 1;
    memcpy(g_hl_info.cam_xyz, m, sizeof m);
    g_hl_info.cam_xyz_present = 1;

    for (lv = 3000; lv <= 12000; lv += 500) {
        se_arena a;
        se_dev_opts o;
        se_raw_src src;
        int mx, mn;

        g_hl_level = lv;
        se_arena_init(&a, arena_mem, sizeof arena_mem);
        se_dev_opts_default(&o);
        o.rows = hl_sink; o.dither = 0;
        memset(&src, 0, sizeof src);
        src.read_rows = hl_rows;
        g_hl_max[0] = g_hl_max[1] = g_hl_max[2] = 0;
        if (se_develop(&g_hl_info, &src, NULL, &o, &a, NULL, NULL) != SE_OK) {
            printf("        develop failed at level %d\n", lv);
            return 0;
        }
        mx = g_hl_max[0] > g_hl_max[1] ? (g_hl_max[0] > g_hl_max[2] ? g_hl_max[0] : g_hl_max[2])
                                       : (g_hl_max[1] > g_hl_max[2] ? g_hl_max[1] : g_hl_max[2]);
        mn = g_hl_max[0] < g_hl_max[1] ? (g_hl_max[0] < g_hl_max[2] ? g_hl_max[0] : g_hl_max[2])
                                       : (g_hl_max[1] < g_hl_max[2] ? g_hl_max[1] : g_hl_max[2]);
        if (mx - mn > worst) worst = mx - mn;
    }
    printf("        neutral pushed to 3.2 stops over: worst channel spread %d\n", worst);
    return worst == 0;
}

/*
 * Half-size decoding: the 4-point IDCT against the 8-point one.
 *
 * This is the check that matters for the camera's speed, because the
 * capture path asks the decoder for half size rather than decoding whole
 * and box-filtering afterwards. Three things can go wrong and each has its
 * own assertion here:
 *
 *   the scale factor   a flat field must come back at the same level. The
 *                      4-point basis needs a different normalisation from
 *                      the 8-point one and getting it wrong shows up as a
 *                      uniformly darker or brighter frame, which is easy to
 *                      mistake for a look.
 *
 *   the geometry       plane strides, MCU counts and the delivered size all
 *                      change with the block size, and an odd width has to
 *                      round up or the last column is dropped.
 *
 *   the transform      a smooth frame decoded half-size must match the same
 *                      frame decoded whole and averaged 2x2. They are not
 *                      the same operation -- one discards the high
 *                      coefficients, the other reconstructs and then
 *                      averages them -- so they agree closely on smooth
 *                      content and not at a hard edge. Smooth content is
 *                      therefore what this uses: a disagreement then means
 *                      the transform is wrong, not that the two filters
 *                      differ.
 */
static uint8_t *g_jh_buf;
static int      g_jh_len;

static int grab_sink(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    memcpy(g_jh_buf + g_jh_len, d, n);
    g_jh_len += (int)n;
    return 0;
}

static uint8_t *g_jh_out;
static int g_jh_rows, g_jh_w;

static int grab_rows(void *ctx, const uint8_t *rgb, int nrows, int width)
{
    (void)ctx;
    g_jh_w = width;
    memcpy(g_jh_out + (size_t)g_jh_rows * width * 3, rgb,
           (size_t)nrows * width * 3);
    g_jh_rows += nrows;
    return 0;
}

/* Encode `rgb` and leave the stream in g_jh_buf. */
static int jh_encode(const uint8_t *rgb, int w, int h, int q)
{
    static se_jpeg j;
    static uint8_t strip[8 * 256 * 3];
    se_jpeg_exif ex;
    memset(&ex, 0, sizeof ex);
    g_jh_len = 0;
    if (se_jpeg_begin(&j, w, h, q, strip, &ex, grab_sink, NULL) != SE_OK) return 0;
    if (se_jpeg_write_rows(&j, rgb, h) != SE_OK) return 0;
    return se_jpeg_end(&j) == SE_OK;
}

static int jh_decode(int scale, int w, int h, uint8_t *dst, int *ow, int *oh)
{
    static uint8_t arena_mem[4u << 20];
    se_arena ar;
    se_jdec_info info;
    (void)w; (void)h;
    se_arena_init(&ar, arena_mem, sizeof arena_mem);
    g_jh_out = dst; g_jh_rows = 0; g_jh_w = 0;
    if (se_jpeg_decode_scaled(g_jh_buf, (size_t)g_jh_len, scale, &ar,
                              grab_rows, NULL, &info) != SE_OK) return 0;
    *ow = info.width; *oh = info.height;
    return g_jh_rows == info.height && g_jh_w == info.width;
}

static int test_jpeg_half(void)
{
    enum { W = 64, H = 48 };
    static uint8_t rgb[256 * 256 * 3];
    static uint8_t full[256 * 256 * 3], half[256 * 256 * 3];
    int x, y, c, ow, oh, worst = 0, ok = 1;
    long sum = 0, n = 0;

    g_jh_buf = malloc(1u << 20);
    if (!g_jh_buf) return 0;

    /* --- 1. a flat field keeps its level ------------------------------- */
    for (y = 0; y < 32; y++)
        for (x = 0; x < 32; x++)
            for (c = 0; c < 3; c++) rgb[(y * 32 + x) * 3 + c] = 137;
    if (!jh_encode(rgb, 32, 32, 95)) { free(g_jh_buf); return 0; }
    if (!jh_decode(2, 32, 32, half, &ow, &oh) || ow != 16 || oh != 16) {
        printf("        flat field: decoded %dx%d, wanted 16x16\n", ow, oh);
        ok = 0;
    } else {
        int wf = 0;
        for (x = 0; x < ow * oh * 3; x++) {
            int d = half[x] - 137;
            if (d < 0) d = -d;
            if (d > wf) wf = d;
        }
        printf("        flat field at half size: worst error %d of 137\n", wf);
        if (wf > 2) ok = 0;
    }

    /* --- 2. an odd size rounds up rather than dropping a column -------- */
    for (y = 0; y < 37; y++)
        for (x = 0; x < 61; x++)
            for (c = 0; c < 3; c++)
                rgb[(y * 61 + x) * 3 + c] = (uint8_t)(40 + x + y);
    if (!jh_encode(rgb, 61, 37, 90)) { free(g_jh_buf); return 0; }
    if (!jh_decode(2, 61, 37, half, &ow, &oh) || ow != 31 || oh != 19) {
        printf("        61x37 at half size: decoded %dx%d, wanted 31x19\n", ow, oh);
        ok = 0;
    } else {
        printf("        61x37 at half size: %dx%d, all rows delivered\n", ow, oh);
    }

    /* --- 3. smooth content agrees with the full decode, averaged ------- */
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            uint8_t *p = rgb + (size_t)(y * W + x) * 3;
            p[0] = (uint8_t)(30 + x * 3);
            p[1] = (uint8_t)(200 - y * 3);
            p[2] = (uint8_t)(90 + ((x + y) * 2 & 63));
        }
    if (!jh_encode(rgb, W, H, 96)) { free(g_jh_buf); return 0; }
    if (!jh_decode(1, W, H, full, &ow, &oh) || ow != W || oh != H) ok = 0;
    if (!jh_decode(2, W, H, half, &ow, &oh) || ow != W / 2 || oh != H / 2) ok = 0;

    if (ok) {
        for (y = 0; y < oh; y++)
            for (x = 0; x < ow; x++)
                for (c = 0; c < 3; c++) {
                    const uint8_t *f = full + ((size_t)(y * 2) * W + x * 2) * 3 + c;
                    int avg = (f[0] + f[3] + f[W * 3] + f[W * 3 + 3] + 2) / 4;
                    int d = half[((size_t)y * ow + x) * 3 + c] - avg;
                    if (d < 0) d = -d;
                    if (d > worst) worst = d;
                    sum += d; n++;
                }
        printf("        half vs full averaged 2x2: worst %d, mean %.3f\n",
               worst, n ? (double)sum / n : 0.0);
        if (worst > 12 || (n && (double)sum / n > 2.0)) ok = 0;
    }

    free(g_jh_buf);
    g_jh_buf = NULL;
    return ok;
}

static int run_selftest(void)
{
    printf("engine selftest\n");
    check(test_debayer_flat(),      "demosaic reproduces a flat field exactly");
    check(test_debayer_phase(),     "demosaic maps all four CFA phases correctly");
    check(test_lut_identity(),      "identity 3D LUT is a no-op");
    check(test_lut_neutral_axis(),  "tetrahedral interpolation keeps grey near-grey");
    check(test_srgb_roundtrip(),    "sRGB encode/decode round trips");
    check(test_jpeg_structure(),    "JPEG encoder emits a well-formed stream");
    check(test_jpeg_half(),         "half-size decoding matches the full decode");
    check(test_highlight_neutral(), "a blown neutral highlight stays neutral");
    check(test_ui_navigation(),     "menu navigation keeps every value in range");
    check(test_ui_coverage(),       "every menu screen draws inside the overlay buffer");
    check(test_stock_roundtrip(),   "a film stock round trips through its text format");
    check(test_curve_monotonic(),   "point curves stay monotonic and inside [0,1]");
    check(fuzz_parsers(),           "corrupt and truncated files are rejected, never crash");
    printf("%s (%d failure%s)\n", fail_count ? "FAILED" : "all passed",
           fail_count, fail_count == 1 ? "" : "s");
    return fail_count ? 1 : 0;
}

/*
 * Render every menu screen into the camera's real overlay format so the
 * design can be reviewed before any of it runs on hardware.
 */
static int dump_ui(const char *dir)
{
    static uint8_t fbmem[SE_UI_FB_SIZE];
    se_ui_fb fb = { fbmem };
    se_ui    u;
    char     path[512];
    int      i;
    /*
     * The real stocks off the card, not a list written out here.
     *
     * This used to be five names typed into this file, and three of them
     * had been retired -- so the screens it produced for review were of a
     * picker that no longer existed. It reads stocks/ now, and draws the
     * stock screen once per film, which is the only way to actually look
     * at every icon.
     */
    #define UI_DUMP_MAX 32
    static se_ui_stock stocks[UI_DUMP_MAX];
    static char        names[UI_DUMP_MAX][SE_STOCK_NAME];
    static char        descs[UI_DUMP_MAX][SE_STOCK_DESC];
    int nstocks = 0;

    if (scan_stocks()) {
        for (i = 0; i < g_stock_n && nstocks < UI_DUMP_MAX; i++) {
            size_t slen;
            void  *txt = read_file(g_stock_path[i], &slen);
            se_stock st;
            if (!txt) continue;
            if (se_stock_parse((const char *)txt, slen, &st) == SE_OK) {
                snprintf(names[nstocks], sizeof names[0], "%s", st.name);
                snprintf(descs[nstocks], sizeof descs[0], "%s", st.desc);
                stocks[nstocks].name = names[nstocks];
                stocks[nstocks].desc = descs[nstocks];
                nstocks++;
            }
            free(txt);
        }
    }
    if (!nstocks) {
        fprintf(stderr, "filmdev: no stocks found in stocks/\n");
        return 1;
    }
    struct { const char *name; se_ui_screen scr; } shots[] = {
        { "stock",     SE_SCREEN_STOCK    },
        { "settings",  SE_SCREEN_SETTINGS },
        { "working",   SE_SCREEN_WORKING  },
        { "done",      SE_SCREEN_DONE     },
    };

    for (i = 0; i < (int)(sizeof shots / sizeof shots[0]); i++) {
        FILE *f;
        se_ui_init(&u);
        se_ui_set_stocks(&u, stocks, nstocks);
        u.screen = shots[i].scr;
        if (shots[i].scr == SE_SCREEN_STOCK)    u.stock = 0;
        if (shots[i].scr == SE_SCREEN_SETTINGS) u.setting = 0;
        if (shots[i].scr == SE_SCREEN_WORKING) {
            u.stock = nstocks > 4 ? 4 : 0;
            u.percent = 62;
            u.stage = "DEMOSAIC";
            snprintf(u.file, sizeof u.file, "IMG_4041.CR2");
            u.done = 3; u.total = 12;
        }
        if (shots[i].scr == SE_SCREEN_DONE) { u.done = 12; u.last_seconds = 486; }

        se_ui_draw(&u, &fb);
        snprintf(path, sizeof path, "%s/ui_%s.fb", dir, shots[i].name);
        mkdir(dir, 0755);
        f = fopen(path, "wb");
        if (!f) { fprintf(stderr, "filmdev: cannot write %s\n", path); return 1; }
        fwrite(fbmem, 1, sizeof fbmem, f);
        fclose(f);
        printf("wrote %s  (%d x %d, 4bpp)\n", path, SE_UI_W, SE_UI_H);
    }

    /* and the stock screen once per film, so every icon can be looked at */
    for (i = 0; i < nstocks; i++) {
        FILE *f;
        char  safe[SE_STOCK_NAME], *q;
        se_ui_init(&u);
        se_ui_set_stocks(&u, stocks, nstocks);
        u.screen = SE_SCREEN_STOCK;
        u.stock  = i;
        se_ui_draw(&u, &fb);
        snprintf(safe, sizeof safe, "%s", names[i]);
        for (q = safe; *q; q++) if (*q == ' ' || *q == '/') *q = '_';
        snprintf(path, sizeof path, "%s/film_%s.fb", dir, safe);
        f = fopen(path, "wb");
        if (!f) continue;
        fwrite(fbmem, 1, sizeof fbmem, f);
        fclose(f);
    }
    printf("wrote %d film screens to %s/film_*.fb\n", nstocks, dir);
    return 0;
}

/*
 * Extract a film stock from a Lightroom edit.
 *
 * A stock is per-channel tone response and nothing else, so the extraction
 * is simply: push a neutral ramp through the full grade and record what
 * comes out of each channel. For neutral input the HSL and saturation
 * stages are no-ops by construction, so this captures the profile curve,
 * the basic panel and every point curve exactly -- and drops precisely the
 * cross-channel work that mosaiced data cannot carry anyway.
 */
#define RAMP_PTS 17

static int make_stock(const char *xmp_path, const char *out_path)
{
    size_t xlen;
    void  *xtext = read_file(xmp_path, &xlen);
    se_grade    grade;
    se_xmp_info xi;
    FILE  *f;
    int    i, c;
    uint8_t ramp[3][RAMP_PTS];
    const char *chan[3] = { "CURVE_R", "CURVE_G", "CURVE_B" };
    const char *wbname = NULL;

    se_set_log(log_cb, NULL);
    if (!xtext) { fprintf(stderr, "filmdev: cannot read %s\n", xmp_path); return 1; }
    if (se_xmp_parse((const char *)xtext, xlen, &grade, &xi) != SE_OK) {
        fprintf(stderr, "filmdev: %s is not a Camera Raw sidecar\n", xmp_path);
        free(xtext);
        return 1;
    }
    free(xtext);

    for (i = 0; i < RAMP_PTS; i++) {
        float v[3];
        v[0] = v[1] = v[2] = (float)i / (float)(RAMP_PTS - 1);
        se_grade_apply(&grade, v);
        for (c = 0; c < 3; c++)
            ramp[c][i] = (uint8_t)(SE_CLAMP(v[c], 0.0f, 1.0f) * 255.0f + 0.5f);
    }

    if (xi.wb_preset > 0) wbname = se_wb_name(xi.wb_preset);

    ensure_dir_for(out_path);
    f = fopen(out_path, "w");
    if (!f) { fprintf(stderr, "filmdev: cannot write %s\n", out_path); return 1; }
    fprintf(f, "# Film stock extracted from %s\n", xmp_path);
    fprintf(f, "# Per-channel tone response only -- this is what can be applied\n");
    fprintf(f, "# to mosaiced sensor data, so DIGIC can do the demosaic and JPEG.\n");
    if (xi.unsupported_n) {
        int u;
        fprintf(f, "#\n# Dropped (needs all three channels at one pixel, or is spatial):\n#   ");
        for (u = 0; u < xi.unsupported_n; u++)
            fprintf(f, "%s%s", u ? ", " : "", xi.unsupported[u]);
        fprintf(f, "\n");
    }
    {
        int any_hsl = 0;
        for (i = 0; i < SE_HSL_BANDS; i++)
            if (grade.hue[i] || grade.sat[i] || grade.lum[i]) any_hsl = 1;
        if (any_hsl) fprintf(f, "#   HSL band adjustments\n");
        if (grade.saturation != 0.0f || grade.vibrance != 0.0f)
            fprintf(f, "#   saturation / vibrance\n");
    }
    fprintf(f, "\n");

    {
        const char *slash = strrchr(xmp_path, '/');
        const char *stem = slash ? slash + 1 : xmp_path;
        char nm[SE_STOCK_NAME];
        int  n = 0;
        while (stem[n] && stem[n] != '.' && n < SE_STOCK_NAME - 1) {
            nm[n] = (char)(stem[n] >= 'a' && stem[n] <= 'z' ? stem[n] - 32 : stem[n]);
            n++;
        }
        nm[n] = 0;
        fprintf(f, "VERSION   %d\n", SE_STOCK_VERSION);
        fprintf(f, "NAME      %s\n", nm);
    }
    fprintf(f, "DESC      EXTRACTED FROM LIGHTROOM\n");
    if (wbname) fprintf(f, "WB        %s\n", wbname);
    if (xi.grain_amount > 0) fprintf(f, "GRAIN     %.0f\n", xi.grain_amount);
    fprintf(f, "\n");

    for (c = 0; c < 3; c++) {
        fprintf(f, "%-9s", chan[c]);
        for (i = 0; i < RAMP_PTS; i++)
            fprintf(f, " %d,%d", i * 255 / (RAMP_PTS - 1), ramp[c][i]);
        fprintf(f, "\n");
    }
    fclose(f);

    printf("wrote %s\n", out_path);
    printf("  white balance : %s\n", wbname ? wbname : "as shot");
    printf("  grain         : %.0f\n", xi.grain_amount);
    printf("  response (in -> R G B):\n");
    for (i = 0; i < RAMP_PTS; i += 2)
        printf("    %3d -> %3d %3d %3d\n", i * 255 / (RAMP_PTS - 1),
               ramp[0][i], ramp[1][i], ramp[2][i]);
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "filmdev - Soup Engine host driver\n"
        "\n"
        "usage: filmdev <command> [options] <file.CR2>\n"
        "\n"
        "commands:\n"
        "  info              parse headers and print what we understand\n"
        "  raw               decode the sensor data and write a 16-bit PGM\n"
        "  develop           one frame: raw -> stock -> demosaic -> JPEG\n"
        "  stocks            list the film stocks in the stock directory\n"
        "  card <path>       copy the stocks onto a card as ML/FILM\n"
        "  contact <f.CR2> [out.jpg] [--from-raw] [--cols N]  one frame through every stock\n"
"  looks <f.CR2> [outdir] [--from-raw]     one full-size JPEG per stock\n"
        "  batch <files...> --stock <name> [-o <dir>]  develop many frames\n"
        "  selftest          run the engine's internal consistency checks\n"
        "  stress <f.CR2> [n]  throw n damaged copies of a raw file at the decoder\n"
        "  checkfir <f.fir>  verify a Canon firmware file before flashing it\n"
        "  checkcard <path>  diagnose a card the camera says has no update on it\n"
        "  ui [dir]          dump every camera screen as a raw overlay buffer\n"
        "  mkstock <x.xmp> <out.fs>   extract a film stock from a Lightroom edit\n"
        "\n"
        "options:\n"
        "  -o <path>         output file\n"
        "  --lut <file.cube> apply a 3D LUT (software path, full colour control)\n"
        "  --xmp <file.xmp>  apply a Lightroom sidecar in full 3D (software path)\n"
        "  --stock <name>    apply a film stock -- the path the camera will use\n"
        "  --ev <f>          exposure push/pull in stops\n"
        "  --quality <1-100> JPEG quality (default 92)\n"
        "  --no-dither       disable the 16->8 ordered dither\n"
        "  --half            half size by Bayer-quad binning (no demosaic at all)\n"
        "  --quarter         quarter size\n"
        "  --scale <1|2|4>   same thing, explicitly\n"
        "  -q                suppress the progress meter\n");
}

int main(int argc, char **argv)
{
    const char *cmd, *in_path = NULL, *out_path = NULL;
    const char *lut_path = NULL, *xmp_path = NULL;
    const char *stock_path = NULL;
    static se_lut1 stock_tone[3];
    static char    stock_name_buf[SE_STOCK_NAME];
    void   *filebuf;
    size_t  filelen;
    se_cr2_info info;
    se_err  rc;
    int     i;
    int     want_raw, want_dev;
    se_dev_opts opts;

    se_dev_opts_default(&opts);

    if (argc < 2) { usage(); return 2; }
    cmd = argv[1];

    if (strcmp(cmd, "ui") == 0)       { return dump_ui(argc > 2 ? argv[2] : "out"); }
    if (strcmp(cmd, "stocks") == 0)   return list_stocks();
    if (strcmp(cmd, "checkcard") == 0) {
        if (argc < 3) { usage(); return 2; }
        return check_card(argv[2]);
    }
    if (strcmp(cmd, "checkfir") == 0) {
        if (argc < 3) { usage(); return 2; }
        return check_fir(argv[2]);
    }
    if (strcmp(cmd, "stress") == 0) {
        if (argc < 3) { usage(); return 2; }
        se_set_log(log_cb, NULL);
        return stress_cr2(argv[2], argc > 3 ? atoi(argv[3]) : 120);
    }
    if (strcmp(cmd, "looks") == 0) {
        int from_raw = 0, k;
        const char *outdir = "out/looks";
        if (argc < 3) { usage(); return 2; }
        for (k = 3; k < argc; k++) {
            if (strcmp(argv[k], "--from-raw") == 0) from_raw = 1;
            else outdir = argv[k];
        }
        return looks_cmd(argv[2], outdir, from_raw);
    }
    if (strcmp(cmd, "contact") == 0) {
        if (argc < 3) { usage(); return 2; }
        {
            const char *outp = "out/contact.jpg";
            int from_raw = 0, cols = 0, k;
            for (k = 3; k < argc; k++) {
                if (strcmp(argv[k], "--from-raw") == 0) from_raw = 1;
                else if (strcmp(argv[k], "--cols") == 0 && k + 1 < argc) cols = atoi(argv[++k]);
                else outp = argv[k];
            }
            return contact_sheet(argv[2], outp, 0, from_raw, cols);
        }
    }
    if (strcmp(cmd, "card") == 0) {
        if (argc < 3) { usage(); return 2; }
        printf("preparing %s\n", argv[2]);
        return prepare_card(argv[2]);
    }
    if (strcmp(cmd, "mkstock") == 0) {
        if (argc < 4) { usage(); return 2; }
        return make_stock(argv[2], argv[3]);
    }
    if (strcmp(cmd, "selftest") == 0) { se_set_log(log_cb, NULL); return run_selftest(); }

    if (argc < 3) { usage(); return 2; }

    for (i = 2; i < argc; i++) {
        if      (strcmp(argv[i], "-o") == 0 && i + 1 < argc)        out_path = argv[++i];
        else if (strcmp(argv[i], "--lut") == 0 && i + 1 < argc)     lut_path = argv[++i];
        else if (strcmp(argv[i], "--xmp") == 0 && i + 1 < argc)     xmp_path = argv[++i];
        else if (strcmp(argv[i], "--stock") == 0 && i + 1 < argc)   stock_path = argv[++i];
        else if (strcmp(argv[i], "--ev") == 0 && i + 1 < argc)      opts.exposure_ev = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--quality") == 0 && i + 1 < argc) opts.jpeg_quality = atoi(argv[++i]);
        else if (strcmp(argv[i], "--no-dither") == 0)               opts.dither = 0;
        else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc)   opts.scale = atoi(argv[++i]);
        else if (strcmp(argv[i], "--half") == 0)                    opts.scale = 2;
        else if (strcmp(argv[i], "--quarter") == 0)                 opts.scale = 4;
        else if (strcmp(argv[i], "-q") == 0)                        g_quiet_progress = 1;
        else if (argv[i][0] == '-')  { usage(); return 2; }
        else                          in_path = argv[i];
    }
    if (!in_path) { usage(); return 2; }

    if (strcmp(cmd, "batch") == 0) {
        char *files[256];
        int   nf = 0;
        if (!stock_path) {
            fprintf(stderr, "filmdev: batch needs --stock <name>\n");
            return 2;
        }
        for (i = 2; i < argc; i++) {
            if (argv[i][0] == '-') {
                if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--stock") == 0 ||
                    strcmp(argv[i], "--scale") == 0 || strcmp(argv[i], "--quality") == 0 ||
                    strcmp(argv[i], "--ev") == 0)
                    i++;
                continue;
            }
            if (nf < 256) files[nf++] = argv[i];
        }
        if (!nf) { fprintf(stderr, "filmdev: batch needs some .CR2 files\n"); return 2; }
        return batch_develop(files, nf, stock_path,
                             out_path ? out_path : "out", opts, g_quiet_progress);
    }

    want_raw = (strcmp(cmd, "raw") == 0);
    want_dev = (strcmp(cmd, "develop") == 0);
    if (!want_raw && !want_dev && strcmp(cmd, "info") != 0) { usage(); return 2; }

    se_set_log(log_cb, NULL);
    se_set_progress(progress_cb, NULL);

    filebuf = read_file(in_path, &filelen);
    if (!filebuf) {
        fprintf(stderr, "filmdev: cannot read %s\n", in_path);
        return 1;
    }
    printf("file           : %s (%.2f MB)\n", in_path, filelen / 1048576.0);

    rc = se_cr2_parse(filebuf, filelen, &info);
    if (rc != SE_OK) {
        fprintf(stderr, "filmdev: parse failed: %s\n", se_strerror(rc));
        free(filebuf);
        return 1;
    }

    if (want_raw || want_dev) {
        size_t    npix = (size_t)info.raw_width * info.raw_height;
        uint16_t *raw  = calloc(npix, sizeof(uint16_t));
        void     *arena_mem = malloc(1 << 20);
        se_arena  arena;
        clock_t   t0;
        char      defout[512];

        if (!raw || !arena_mem) {
            fprintf(stderr, "filmdev: out of memory\n");
            return 1;
        }
        se_arena_init(&arena, arena_mem, 1 << 20);

        t0 = clock();
        rc = se_cr2_decode(filebuf, filelen, &info, raw, &arena);
        if (rc != SE_OK) {
            fprintf(stderr, "filmdev: decode failed: %s\n", se_strerror(rc));
            return 1;
        }
        fprintf(stderr, "  decoded in %.2f s on this machine\n",
                (double)(clock() - t0) / CLOCKS_PER_SEC);

        se_cr2_measure_black(&info, raw);
        dump_info(&info);
        printf("arena peak     : %zu bytes\n", arena.peak);
        printf("raw buffer     : %.1f MB\n", npix * 2 / 1048576.0);

        if (!out_path) {
            const char *slash = strrchr(in_path, '/');
            const char *stem  = slash ? slash + 1 : in_path;
            const char *dot   = strrchr(stem, '.');
            int n = dot ? (int)(dot - stem) : (int)strlen(stem);
            snprintf(defout, sizeof defout, "out/%.*s.%s", n, stem,
                     want_dev ? "jpg" : "pgm");
            out_path = defout;
        }

        if (want_raw) {
            ensure_dir_for(out_path);
        if (write_pgm16(out_path, raw, info.raw_width, info.raw_height) != 0) {
                fprintf(stderr, "filmdev: cannot write %s\n", out_path);
                return 1;
            }
        } else {
            size_t    need = se_develop_scratch_size(&info) + (1 << 20);
            void     *dmem = malloc(need);
            se_arena  darena;
            se_lut3   lut;
            const se_lut3 *use_lut = NULL;
            FILE     *fo;

            if (!dmem) { fprintf(stderr, "filmdev: out of memory\n"); return 1; }
            se_arena_init(&darena, dmem, need);

            if (stock_path) {
                se_stock st;
                char resolved[512];
                if (load_stock(stock_path, &st, resolved, sizeof resolved) != 0)
                    return 1;
                se_stock_bake(&st, stock_tone);
                opts.tone = stock_tone;
                opts.grain_amount = se_stock_grain(&st, SE_SRC_RAW);
                opts.grain_size = 22; opts.grain_frequency = 60;
                if (st.wb_preset > 0 && info.wb_presets_present)
                    opts.wb_override = info.wb_preset[st.wb_preset];
                opts.stock_name = stock_name_buf;
                { int k=0; while (st.name[k] && k < (int)sizeof stock_name_buf - 1) {
                      stock_name_buf[k] = st.name[k]; k++; } stock_name_buf[k]=0; }
                printf("stock          : %s - %s\n", st.name, st.desc);
                printf("  white balance: %s\n",
                       st.wb_preset > 0 ? se_wb_name(st.wb_preset) : "as shot");
                printf("  applied to the mosaic, as the camera will\n");
            } else if (xmp_path) {
                size_t xlen;
                void  *xtext = read_file(xmp_path, &xlen);
                se_grade    grade;
                se_xmp_info xi;
                if (!xtext) {
                    fprintf(stderr, "filmdev: cannot read %s\n", xmp_path);
                    return 1;
                }
                rc = se_xmp_parse((const char *)xtext, xlen, &grade, &xi);
                free(xtext);
                if (rc != SE_OK) {
                    fprintf(stderr, "filmdev: %s: %s\n", xmp_path, se_strerror(rc));
                    return 1;
                }
                rc = se_grade_bake(&grade, 33, &lut, &darena);
                if (rc != SE_OK) {
                    fprintf(stderr, "filmdev: baking sidecar: %s\n", se_strerror(rc));
                    return 1;
                }
                use_lut = &lut;
                opts.grain_amount    = xi.grain_amount;
                opts.grain_size      = xi.grain_size;
                opts.grain_frequency = xi.grain_frequency;
                if (xi.wb_preset > 0 && info.wb_presets_present)
                    opts.wb_override = info.wb_preset[xi.wb_preset];
                printf("look           : %s\n", xmp_path);
                printf("  white balance: %s\n",
                       xi.wb_name[0] ? xi.wb_name : "As Shot");
                if (xi.grain_amount > 0)
                    printf("  grain        : amount %.0f size %.0f frequency %.0f\n",
                           xi.grain_amount, xi.grain_size, xi.grain_frequency);
                if (xi.unsupported_n) {
                    int u;
                    printf("  not applied  : ");
                    for (u = 0; u < xi.unsupported_n; u++)
                        printf("%s%s", u ? ", " : "", xi.unsupported[u]);
                    printf("\n                 (spatial or proprietary; a colour LUT cannot express these)\n");
                }
            } else if (lut_path) {
                size_t llen;
                void  *ltext = read_file(lut_path, &llen);
                if (!ltext) {
                    fprintf(stderr, "filmdev: cannot read %s\n", lut_path);
                    return 1;
                }
                rc = se_lut3_parse_cube((const char *)ltext, llen, &lut, &darena);
                free(ltext);
                if (rc != SE_OK) {
                    fprintf(stderr, "filmdev: %s: %s\n", lut_path, se_strerror(rc));
                    return 1;
                }
                use_lut = &lut;
                printf("look           : %s (%dx%dx%d cube)\n",
                       lut_path, lut.size, lut.size, lut.size);
            } else {
                printf("look           : none (straight sRGB render)\n");
            }

            ensure_dir_for(out_path);
            fo = fopen(out_path, "wb");
            if (!fo) {
                fprintf(stderr, "filmdev: cannot write %s\n", out_path);
                return 1;
            }
            if (ends_with(out_path, ".ppm")) {
                int ow, oh;
                se_develop_out_size(&info, opts.scale, &ow, &oh);
                /* uncompressed, for comparing against a reference renderer */
                fprintf(fo, "P6\n%d %d\n255\n", ow, oh);
                opts.rows     = ppm_row_sink;
                opts.rows_ctx = fo;
            }
            {
                se_raw_src rsrc;
                memset(&rsrc, 0, sizeof rsrc);
                rsrc.mem = raw;      /* the host has the whole frame in RAM */
                if (!opts.stock_name)
                    opts.stock_name = xmp_path ? "XMP" : (lut_path ? "LUT" : "PLAIN");
                t0 = clock();
                rc = se_develop(&info, &rsrc, use_lut, &opts, &darena, jpeg_sink, fo);
            }
            fclose(fo);
            if (rc != SE_OK) {
                fprintf(stderr, "filmdev: develop failed: %s\n", se_strerror(rc));
                return 1;
            }
            fprintf(stderr, "  developed in %.2f s on this machine\n",
                    (double)(clock() - t0) / CLOCKS_PER_SEC);
            printf("develop arena  : %.2f MB peak\n", darena.peak / 1048576.0);
            {
                int ow, oh;
                se_develop_out_size(&info, opts.scale, &ow, &oh);
                printf("output         : %d x %d (scale 1/%d%s), quality %d\n",
                       ow, oh, opts.scale,
                       opts.scale == 1 ? ", Malvar-He-Cutler" : ", quad binned",
                       opts.jpeg_quality);
            }
        }
        printf("wrote          : %s\n", out_path);
    } else {
        dump_info(&info);
    }

    free(filebuf);
    return 0;
}
