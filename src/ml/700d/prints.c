/*
 * prints.c - the small copies Prints shows, and the lists of what is on the
 * card.
 *
 * A print on this camera takes seconds to decode, which is no way to flip
 * through frames, so every frame gets a preview as it develops: the rows are
 * passing through anyway, and adding them up costs little. It lives in
 * ML/PRINTS as FLM_1234.SPV, 648x432 and a 216x144 thumbnail, already in the
 * screen's colours, so opening one is a card read. Older prints have none;
 * the worker makes one from the JPEG the first time it is asked.
 *
 * The screen's photo palette is a 6x6x5 colour cube at fixed entries, which
 * is what lets the worker write these with the pack not even loaded. Photos
 * go on the bitmap layer rather than Canon's YUV one because the first camera
 * run proved the palette holds on the shooting screen, and because staying
 * here means the half-press closes it as it closes everything else. tools/
 * mkui.js puts the cube at the same entries. 180 colours is good for judging a
 * frame, not for judging a grade: for that, look at the JPEG on a computer.
 */
#include "soupeng.h"

#define SE_CUBE_BASE 69
/* "SPV2". SPV1 files used an ordered dither: their magic no longer matches, so
 * the worker makes each one again from its JPEG the first time it is asked. */
#define SE_PV_MAGIC  0x32565053

typedef struct {
    uint32_t magic, flm_bytes;
    int32_t  size;                 /* se_size's numbering */
    char     film[32];
    uint8_t  pad[20];
} se_pv_head;                      /* 64 bytes, then thumbnail, then preview */

#define SE_PV_TH    (SE_TH_W * SE_TH_H)
#define SE_PV_FULL  (SE_PV_W * SE_PV_H)
#define SE_PV_FILE  (64 + SE_PV_TH + SE_PV_FULL)

/* ------------------------------------------------------------ the reducer */
/* One user at a time: the worker. */
static uint8_t  *pv_file;
static uint32_t  pv_acc[SE_PV_W * 3], pv_tacc[SE_TH_W * 3];
static int       pv_f, pv_rows, pv_y;

/*
 * Error diffusion: each pixel takes the nearest cube colour and passes what it
 * missed on to its neighbours, Floyd-Steinberg, right to left on odd rows so
 * the error has no direction to drift in. This replaced a 4x4 Bayer dither,
 * whose fixed pattern showed as a cross-hatch at the LCD's pixel size. On the
 * same frames it scored 2 to 3 dB closer to the original once blurred to the
 * eye's resolution, and no palette or pack change was needed. Errors are in
 * 1/16ths; two rows of each are all it keeps, with a spare pixel at each end.
 */
static int16_t pv_err[2][(SE_PV_W + 2) * 3], pv_terr[2][(SE_TH_W + 2) * 3];

/* the blue levels mkui.js gives the cube: red and green are multiples of 51 */
static const uint8_t pv_blue[5] = { 0, 64, 128, 191, 255 };

static void pv_diffuse(const uint32_t *rgb, uint8_t *out, int w, int16_t *cur, int16_t *next, int rev)
{
    int k, c, step = rev ? -3 : 3, i = rev ? (w - 1) * 3 : 0;
    memset(next, 0, (size_t)(w + 2) * 3 * sizeof *next);
    for (k = 0; k < w; k++, i += step) {
        int idx = 0;
        int16_t *e = cur + i + 3, *n = next + i + 3;
        for (c = 0; c < 3; c++) {
            int v = (int)rgb[i + c] + (e[c] >> 4), q, err;
            if (v < 0) v = 0; else if (v > 255) v = 255;
            q = (v * (c == 2 ? 4 : 5) + 128) >> 8;
            err = v - (c == 2 ? pv_blue[q] : q * 51);
            idx = idx * (c == 2 ? 5 : 6) + q;
            e[c + step] += (int16_t)(err * 7);
            n[c - step] += (int16_t)(err * 3);
            n[c]        += (int16_t)(err * 5);
            n[c + step] += (int16_t)err;
        }
        out[i / 3] = (uint8_t)(SE_CUBE_BASE + idx);
    }
}

int se_pv_begin(int width)
{
    pv_f = width / SE_PV_W;
    if (pv_f < 1) return -1;
    pv_file = fio_malloc(SE_PV_FILE);
    if (!pv_file) return -1;
    memset(pv_file, 0, SE_PV_FILE);
    memset(pv_acc, 0, sizeof pv_acc);
    memset(pv_tacc, 0, sizeof pv_tacc);
    memset(pv_err, 0, sizeof pv_err);
    memset(pv_terr, 0, sizeof pv_terr);
    pv_rows = pv_y = 0;
    return 0;
}

/* One row of RGB, at any width that is a multiple of 648. */
void se_pv_row(const uint8_t *rgb)
{
    int x, k;
    if (!pv_file || pv_y >= SE_PV_H) return;
    for (x = 0; x < SE_PV_W; x++) {
        uint32_t r = 0, g = 0, b = 0;
        for (k = 0; k < pv_f; k++, rgb += 3) { r += rgb[0]; g += rgb[1]; b += rgb[2]; }
        pv_acc[x * 3] += r; pv_acc[x * 3 + 1] += g; pv_acc[x * 3 + 2] += b;
    }
    if (++pv_rows < pv_f) return;

    {
        uint8_t *out = pv_file + 64 + SE_PV_TH + pv_y * SE_PV_W;
        int n = pv_f * pv_f, ty = pv_y / 3, odd = pv_y & 1;
        for (x = 0; x < SE_PV_W; x++) {
            uint32_t r = pv_acc[x * 3] / n, g = pv_acc[x * 3 + 1] / n, b = pv_acc[x * 3 + 2] / n;
            pv_acc[x * 3] = r; pv_acc[x * 3 + 1] = g; pv_acc[x * 3 + 2] = b;
            pv_tacc[(x / 3) * 3] += r; pv_tacc[(x / 3) * 3 + 1] += g; pv_tacc[(x / 3) * 3 + 2] += b;
        }
        pv_diffuse(pv_acc, out, SE_PV_W, pv_err[odd], pv_err[!odd], odd);
        if (pv_y % 3 == 2) {
            uint8_t *t = pv_file + 64 + ty * SE_TH_W;
            odd = ty & 1;
            for (x = 0; x < SE_TH_W * 3; x++) pv_tacc[x] /= 9;
            pv_diffuse(pv_tacc, t, SE_TH_W, pv_terr[odd], pv_terr[!odd], odd);
            memset(pv_tacc, 0, sizeof pv_tacc);
        }
    }
    memset(pv_acc, 0, sizeof pv_acc);
    pv_rows = 0;
    pv_y++;
}

void se_pv_abort(void)
{
    if (pv_file) { fio_free(pv_file); pv_file = 0; }
}

static void pv_path(int no, char *buf, size_t cap)
{
    snprintf(buf, cap, "ML/PRINTS/FLM_%04d.SPV", no);
}

/* Writes it and lets go of the buffer. A preview that fails is only a
 * preview: the worker makes it again when it is next asked for. */
void se_pv_save(int no, const char *film, int size, uint32_t flm_bytes)
{
    char path[48];
    se_pv_head *h = (se_pv_head *)pv_file;
    FILE *f;
    if (!pv_file) return;
    h->magic = SE_PV_MAGIC;
    h->flm_bytes = flm_bytes;
    h->size = size;
    snprintf(h->film, sizeof h->film, "%s", film);
    pv_path(no, path, sizeof path);
    f = FIO_CreateFile(path);
    if (f) {
        if (FIO_WriteFile(f, pv_file, SE_PV_FILE) != SE_PV_FILE) se_logf("preview short write");
        FIO_CloseFile(f);
    }
    se_pv_abort();
}

void se_pv_remove(int no)
{
    char path[48];
    pv_path(no, path, sizeof path);
    FIO_RemoveFile(path);
}

/* ------------------------------------------------------------ reading one */
static int pv_read(int no, uint32_t off, void *dst, int n)
{
    char path[48];
    uint8_t *bounce, *d = dst;
    FILE *f;
    int ok = 1;
    pv_path(no, path, sizeof path);
    f = FIO_OpenFile(path, O_RDONLY | O_SYNC);
    if (!f) return -1;
    bounce = fio_malloc(64 * 1024);
    if (!bounce) { FIO_CloseFile(f); return -1; }
    if (FIO_SeekSkipFile(f, off, SEEK_SET) != (int64_t)off) ok = 0;
    while (ok && n > 0) {
        int want = n > 64 * 1024 ? 64 * 1024 : n, got = FIO_ReadFile(f, bounce, (size_t)want);
        if (got <= 0) { ok = 0; break; }
        memcpy(d, bounce, (size_t)got);
        d += got; n -= got;
    }
    fio_free(bounce);
    FIO_CloseFile(f);
    return ok ? 0 : -1;
}

/* The preview's record, if it has one for the print now on the card: a
 * number reused after a reformat must not show the old picture. */
int se_pv_info(int no, char *film, size_t cap, int *size)
{
    char flm[FIO_MAX_PATH_LENGTH];
    static se_pv_head h;
    se_flm_path(no, flm, sizeof flm);
    if (pv_read(no, 0, &h, sizeof h) != 0 || h.magic != SE_PV_MAGIC) return -1;
    if (h.flm_bytes != file_size(flm)) return -1;
    h.film[sizeof h.film - 1] = 0;
    snprintf(film, cap, "%s", h.film);
    *size = h.size;
    return 0;
}

int se_pv_thumb(int no, uint8_t *dst) { return pv_read(no, 64, dst, SE_PV_TH); }
int se_pv_full(int no, uint8_t *dst)  { return pv_read(no, 64 + SE_PV_TH, dst, SE_PV_FULL); }

/* ---------------------------------------------------------- what is there */
uint16_t se_prints[SE_MAX_LIST], se_raws[SE_MAX_LIST];
int      se_nprints, se_nraws;

void se_flm_path(int no, char *buf, size_t cap) { snprintf(buf, cap, "%s/FLM_%04d.JPG", get_dcim_dir(), no); }
void se_cr2_path(int no, char *buf, size_t cap) { snprintf(buf, cap, "%s/%s%04d.CR2", get_dcim_dir(), get_file_prefix(), no); }

/* The four digits before the extension, or -1. */
int se_file_no(const char *name)
{
    int n = (int)strlen(name), i, v = 0;
    if (n < 8 || name[n - 4] != '.') return -1;
    for (i = n - 8; i < n - 4; i++) {
        if (name[i] < '0' || name[i] > '9') return -1;
        v = v * 10 + name[i] - '0';
    }
    return v;
}

/*
 * One walk of the shooting folder: names only, no stat of each file. A print
 * and a raw pair up by number, in a bit each, so a folder of thousands costs
 * the walk and no searching. Newest first in both lists.
 */
void se_walk_card(void)
{
    static uint8_t flm[1250], cr2[1250];
    struct fio_file *file = alloc_fio_file();
    struct fio_dirent *d;
    int no;

    memset(flm, 0, sizeof flm);
    memset(cr2, 0, sizeof cr2);
    se_nprints = se_nraws = 0;
    if (!file) return;
    d = FIO_FindFirstEx(get_dcim_dir(), file);
    if (IS_ERROR(d)) { free(file); return; }
    do {
        struct file_info fi = convert_fio_file_info(file);
        if (!fi.name[0] || (fi.mode & ATTR_DIRECTORY)) continue;
        no = se_file_no(fi.name);
        if (no < 0) continue;
        if (ends_with_ci(fi.name, ".CR2")) cr2[no >> 3] |= (uint8_t)(1 << (no & 7));
        else if (ends_with_ci(fi.name, ".JPG") && strncmp(fi.name, "FLM_", 4) == 0) flm[no >> 3] |= (uint8_t)(1 << (no & 7));
    } while (FIO_FindNextEx(d, file) == 0);
    FIO_FindClose(d);
    free(file);

    for (no = 9999; no >= 0; no--) {
        int f = (flm[no >> 3] >> (no & 7)) & 1, c = (cr2[no >> 3] >> (no & 7)) & 1;
        if (f && se_nprints < SE_MAX_LIST) se_prints[se_nprints++] = (uint16_t)no;
        if (c && !f && se_nraws < SE_MAX_LIST) se_raws[se_nraws++] = (uint16_t)no;
    }
}
