/** \file
 * Soup Engine for the EOS 60D, as a Magic Lantern module (soup.mo).
 *
 * Ported from the 50D port (../Canon-EOS-50D, film_develop.c +
 * film_menu.c by @rebelt2i), which builds the same
 * code into Magic Lantern itself. Here it is a loadable module instead:
 *
 *   - no change to Magic Lantern's own source, menu or memory map
 *   - Magic Lantern's menu and every other feature stay available
 *   - turn it off by disabling the module in ML's Modules tab
 *
 * What it does: after each shot, develops the frame through the selected
 * film stock and writes FILM/xxxCANON/FLM_nnnn.JPG (outside DCIM). The
 * original CR2 / JPG is never modified.
 *
 * Source per frame:
 *   CR2 present  -> the full-size JPEG preview Canon embeds in the CR2
 *                   (5184x3456 on the 60D), as the 5D Classic and 50D do.
 *   only a JPG   -> the camera's own JPEG (JPEG-only shooting).
 * Output is half size: 2592x1728 from a full-size frame. Canon's playback
 * never sees it (it lives outside DCIM); SET in playback shows it instead.
 *
 * State on the card (same files as the 50D build):
 *   ML/SE.CFG   FILM ON|OFF, STOCK <name>
 *   ML/SE.NXT   last frame number that was settled
 *   ML/FILM/    INDEX.TXT and the .fs stock files
 *
 * Every name on the card must be 8.3: this camera's file system cannot open
 * long names (upstream's bleachbypass.fs etc. are renamed by build-60d.sh).
 *
 * GPL v2, same as Soup Engine and Magic Lantern.
 */
#include <dryos.h>
#include <tasks.h>
#include <module.h>
#include <menu.h>
#include <bmp.h>
#include <fio-ml.h>
#include <fileprefix.h>
#include <mem.h>
#include <shoot.h>
#include <lens.h>
#include <propvalues.h>

#include "se.h"
#include "cr2.h"
#include "stock.h"
#include "look.h"
#include "jpeg_dec.h"
#include "jpeg_enc.h"
#include "soup_dev.h"
#include "soup_ui.h"

extern int ml_started;

/* ------------------------------------------------------------ sizes ---- */

#define FILM_MIN_CR2   (8u * 1024 * 1024)  /* smaller = still being written */
#define FILM_MIN_JPG   (512u * 1024)
#define FILM_MAX       32
#define FILM_NAME      32
#define FILM_PATH      96

#define SOUP_CFG   "ML/SE.CFG"
#define SOUP_NEXT  "ML/SE.NXT"   /* 8.3: DryOS FIO has no long names */
#define SOUP_INDEX "ML/FILM/INDEX.TXT"
#define SOUP_LOG   "ML/LOGS/SOUP.LOG"

/* ------------------------------------------------------------ state ---- */

static int film_on;                 /* menu: Film OFF/ON                 */
static int film_stock;              /* menu: index into film_names       */
static int film_count;
static char film_names[FILM_MAX][FILM_NAME];
static const char *film_choices[FILM_MAX];
static char film_files[FILM_MAX][FILM_NAME];   /* .fs file per name       */
static char film_descs[FILM_MAX][48];          /* DESC line, for the picker */

static int film_saved_on = -1, film_saved_stock = -1;
static int film_started;            /* develop task is running           */

/* status, shown in the menu */
static char film_status[48] = "idle";
static int  film_done_count, film_fail_count, film_last_secs;

/* progress popup */
static char film_job[40];

/* What the picker's DEVELOPING page and the shooting-screen badge show. */
static volatile int film_busy;          /* frames are queued or developing */
static volatile int film_pct;           /* current frame, 0..100            */
static volatile int film_event;         /* 1 = a frame finished, 2 = failed */
static char film_file[24];              /* "IMG_2290.CR2"                    */
static int  film_batch_done, film_batch_total;
static volatile int film_batch_t0, film_batch_secs;  /* whole batch, for DEVELOPED */
static int  film_shown = -1, film_shown_ms;

/* --------------------------------------------------------- logging ---- */

/* Append one line to ML/LOGS/SOUP.LOG. Only from the develop task, and
 * only between frames, never while a frame is streaming. */
void soup_log(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    int n;
    FILE *f;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 2) n = sizeof(buf) - 2;
    buf[n++] = '\n';

    f = FIO_CreateFileOrAppend(SOUP_LOG);
    if (!f) return;
    FIO_WriteFile(f, buf, (size_t)n);
    FIO_CloseFile(f);
}

/* ------------------------------------------------------ card helpers ---- */

/* Whole small file into a fresh DMA buffer, NUL terminated. Caller frees
 * with fio_free. FIO_ReadFile wants uncacheable memory on this body. */
static char *soup_slurp(const char *path, int cap, int *len)
{
    char *b = fio_malloc((size_t)cap);
    int n;
    *len = 0;
    if (!b) return 0;
    n = (int)read_file(path, b, (size_t)cap - 1);
    if (n <= 0) { fio_free(b); return 0; }
    if (n > cap - 1) n = cap - 1;
    b[n] = 0;
    *len = n;
    return b;
}

static void soup_copy(char *dst, int n, const char *src)
{
    int i = 0;
    if (n <= 0) return;
    while (src[i] && i < n - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int soup_eq_ci(const char *a, const char *b)
{
    int i;
    for (i = 0; a[i] && b[i]; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
    }
    return a[i] == 0 && b[i] == 0;
}

/* ------------------------------------------------------- stock list ---- */

/* NAME line in a stock file: the rest of the line, trailing blanks and any
 * comment dropped. Returns 1 if a name was stored. (Stopping at the first
 * space, as the 50D code did, showed "CLASSIC" for both Classic Chrome and
 * Classic Neg.) */
static int soup_key_from_body(const char *body, int n, const char *key,
                              char *dst, int dstn)
{
    int kl = (int)strlen(key);
    int i = 0;
    while (i < n) {
        int start = i, k, e, o;
        while (i < n && body[i] != '\n' && body[i] != '\r') i++;
        if (i - start > kl && !strncmp(body + start, key, kl) &&
            (body[start + kl] == ' ' || body[start + kl] == '\t')) {
            k = start + kl;
            while (k < i && (body[k] == ' ' || body[k] == '\t')) k++;
            e = k;
            while (e < i && body[e] != '#' && body[e] != ';') e++;
            while (e > k && (body[e - 1] == ' ' || body[e - 1] == '\t')) e--;
            for (o = 0; k < e && o < dstn - 1; ) dst[o++] = body[k++];
            dst[o] = 0;
            if (o > 0) return 1;
        }
        while (i < n && (body[i] == '\n' || body[i] == '\r')) i++;
    }
    return 0;
}

static void soup_read_index(void)
{
    char *text, *body;
    char path[FILM_PATH];
    int got, nbody, i, line0;

    film_count = 0;
    text = soup_slurp(SOUP_INDEX, 8192, &got);
    if (text) {
        line0 = 0;
        for (i = 0; i <= got && film_count < FILM_MAX; i++) {
            char *line;
            if (text[i] != '\n' && text[i] != '\r' && text[i] != 0) continue;
            text[i] = 0;
            line = text + line0;
            line0 = i + 1;
            if (!line[0] || line[0] == '#') continue;

            snprintf(path, sizeof(path), "ML/FILM/%s", line);
            body = soup_slurp(path, 16384, &nbody);
            if (!body) continue;
            if (!soup_key_from_body(body, nbody, "NAME", film_names[film_count], FILM_NAME))
                soup_copy(film_names[film_count], FILM_NAME, line);
            if (!soup_key_from_body(body, nbody, "DESC", film_descs[film_count], 48))
                film_descs[film_count][0] = 0;
            fio_free(body);
            soup_copy(film_files[film_count], FILM_NAME, line);
            film_choices[film_count] = film_names[film_count];
            film_count++;
        }
        fio_free(text);
    }
    if (film_count == 0) {
        soup_copy(film_names[0], FILM_NAME, "STANDARD");
        soup_copy(film_descs[0], 48, "NO ML/FILM ON THE CARD");
        film_files[0][0] = 0;
        film_choices[0] = film_names[0];
        film_count = 1;
    }
}

static void soup_read_cfg(void)
{
    char stock[FILM_NAME];
    char *text;
    int got, i, line0, n;

    stock[0] = 0;
    text = soup_slurp(SOUP_CFG, 512, &got);
    if (!text) return;
    line0 = 0;
    for (i = 0; i <= got; i++) {
        char *line;
        if (text[i] != '\n' && text[i] != '\r' && text[i] != 0) continue;
        text[i] = 0;
        line = text + line0;
        line0 = i + 1;
        if (!strncmp(line, "STOCK ", 6)) soup_copy(stock, FILM_NAME, line + 6);
        else if (!strncmp(line, "FILM ", 5)) film_on = soup_eq_ci(line + 5, "ON");
    }
    fio_free(text);
    for (n = 0; stock[0] && n < film_count; n++)
        if (soup_eq_ci(film_names[n], stock)) { film_stock = n; break; }
}

static void soup_write_cfg(void)
{
    char buf[128];
    int n;
    FILE *f;

    if (film_stock < 0 || film_stock >= film_count) film_stock = 0;
    n = snprintf(buf, sizeof(buf), "FILM %s\nSTOCK %s\n",
                 film_on ? "ON" : "OFF", film_names[film_stock]);
    if (n <= 0 || n >= (int)sizeof(buf)) return;
    f = FIO_CreateFile(SOUP_CFG);
    if (!f) return;
    FIO_WriteFile(f, buf, (size_t)n);
    FIO_CloseFile(f);
}

/* Parse the selected stock. 0 on success. "Standard" (no file) is the
 * engine's neutral default. */
static int soup_load_stock(int idx, se_stock *s)
{
    char path[FILM_PATH];
    char *text;
    int got, rc;

    se_stock_init(s);
    if (idx < 0 || idx >= film_count) idx = 0;
    if (!film_files[idx][0]) {
        soup_copy(s->name, SE_STOCK_NAME, film_names[idx]);
        return 0;
    }
    snprintf(path, sizeof(path), "ML/FILM/%s", film_files[idx]);
    text = soup_slurp(path, 16384, &got);
    if (!text) return -1;
    rc = se_stock_parse(text, (size_t)got, s);
    fio_free(text);
    if (rc != SE_OK) return -1;
    if (!s->name[0]) soup_copy(s->name, SE_STOCK_NAME, film_names[idx]);
    return 0;
}

/* --------------------------------------------------------- progress ---- */

static void soup_show(int pct)
{
    int now = get_ms_clock();
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct == film_shown) return;
    if (film_shown_ms && pct < 100 && (now - film_shown_ms) < 500) return;
    film_shown = pct;
    film_shown_ms = now;
    film_pct = pct;
    snprintf(film_status, sizeof(film_status), "%s %d%%", film_job, pct);
    /* Shown by soup_ui.c: a small badge on the shooting screen, and the
     * DEVELOPING page in the picker. No pop-up text box. */
}

static void soup_progress_cb(void *ctx, const char *stage, int percent)
{
    (void)ctx; (void)stage;
    /* The engine's progress hook is global: the playback viewer's decode
     * (in soup_ui) reports through it too. Only the develop counts. */
    if (strcmp(get_current_task_name(), "soup_film") != 0) return;
    soup_show(percent);
}

/* ----------------------------------------------------------- watcher ---- */

/*
 * Film files live outside DCIM, in a mirror of the camera's folder:
 * B:/DCIM/100CANON/IMG_2278.CR2 -> B:/FILM/100CANON/FLM_2278.JPG.
 * Canon's playback indexes everything in DCIM at power-on, and extra
 * JPEGs it didn't write there gave "No Image". Soup's own playback
 * toggle shows them instead.
 */
static void soup_film_dir(char *out, int n)
{
    const char *dir = get_dcim_dir();          /* "B:/DCIM/100CANON" */
    const char *sub = strstr(dir, "/DCIM/");
    if (sub) {
        int k = (int)(sub - dir);
        if (k > n - 1) k = n - 1;
        memcpy(out, dir, k);
        out[k] = 0;
        snprintf(out + k, n - k, "/FILM/%s", sub + 6);
    } else {
        snprintf(out, n, "B:/FILM/100CANON");
    }
}

static void soup_ensure_dir(const char *fdir)
{
    char top[FILM_PATH];
    int k = 0, i;
    for (i = 0; fdir[i]; i++) if (fdir[i] == '/') k = i;   /* last '/' */
    if (is_dir(fdir)) return;
    if (k > 0 && k < (int)sizeof(top)) {
        memcpy(top, fdir, k); top[k] = 0;
        if (!is_dir(top)) FIO_CreateDirectory(top);
    }
    FIO_CreateDirectory(fdir);
}

void soup_flm_path(int num, char *out, int n)
{
    char fdir[FILM_PATH];
    soup_film_dir(fdir, sizeof(fdir));
    snprintf(out, n, "%s/FLM_%04d.JPG", fdir, num);
}

/*
 * Which photo is Canon's playback showing? Not offered by Magic Lantern,
 * so read from two of Canon's own variables, found for 60D firmware
 * 1.1.1 by stepping through playback in the emulator and confirmed as
 * fixed globals in the playback code's literal pools:
 *   0x59B74  folder << 16 | file   (e.g. 100 << 16 | 2278)
 *   0x2A194  file
 * Reading them changes nothing. Both must agree, or we say we can't tell.
 */
int soup_play_current(int *folder, int *num)
{
    uint32_t a, b;
    if (!strstr(camera_model, "60D") || strncmp(firmware_version, "1.1.1", 5) != 0)
        return -1;
    a = *(volatile uint32_t *)0x59B74;
    b = *(volatile uint32_t *)0x2A194;
    if ((a & 0xFFFF) != b || b < 1 || b > 9999) return -1;
    if ((a >> 16) < 100 || (a >> 16) > 999) return -1;
    *folder = (int)(a >> 16);
    *num = (int)b;
    return 0;
}

void soup_flm_path_in(int folder, int num, char *out, int n)
{
    /* no "%.*s": DryOS's snprintf is not to be trusted with it */
    const char *dir = get_dcim_dir();          /* "B:/DCIM/100CANON" */
    const char *sub = strstr(dir, "/DCIM/");
    char drive[8] = "B:";
    int k = sub ? (int)(sub - dir) : 0;
    if (k > 0 && k < (int)sizeof(drive)) { memcpy(drive, dir, k); drive[k] = 0; }
    snprintf(out, n, "%s/FILM/%03d%s/FLM_%04d.JPG", drive, folder,
             get_dcim_dir_suffix(), num);
}

static void soup_paths(int num, char *cr2, char *jpg, char *flm, char *tmp, int n)
{
    const char *dir = get_dcim_dir();
    const char *prefix = get_file_prefix();
    char fdir[FILM_PATH];
    soup_film_dir(fdir, sizeof(fdir));
    snprintf(cr2, n, "%s/%s%04d.CR2", dir, prefix, num);
    snprintf(jpg, n, "%s/%s%04d.JPG", dir, prefix, num);
    snprintf(flm, n, "%s/FLM_%04d.JPG", fdir, num);
    snprintf(tmp, n, "%s/FLM_%04d.TMP", fdir, num);
}

static int soup_next_load(void)
{
    char *b;
    int n, v = 0;
    b = soup_slurp(SOUP_NEXT, 32, &n);
    if (b) { v = atoi(b); fio_free(b); }
    return v;
}

static void soup_next_save(int num)
{
    char b[16];
    int n = snprintf(b, sizeof(b), "%d\n", num);
    FILE *f = FIO_CreateFile(SOUP_NEXT);
    if (!f) return;
    if (n > 0) FIO_WriteFile(f, b, (size_t)n);
    FIO_CloseFile(f);
}

/* Is `path` at least `min` bytes and no longer growing? */
/*
 * Has the camera finished writing this file? Big enough to be a whole
 * frame and not growing -- or, for anything smaller (S3 JPEGs, a very
 * dark frame), unchanged in size for a few seconds. A small file used to
 * wait forever here and hold up every frame after it.
 */
#define FILM_SETTLE_MS 3000
static char     settle_path[FILM_PATH];
static uint32_t settle_size;
static int      settle_since;

static int soup_settled(const char *path, uint32_t min)
{
    uint32_t s1 = 0, s2 = 0;
    int now;
    if (FIO_GetFileSize(path, &s1) != 0 || s1 == 0) return 0;
    if (s1 >= min) {
        msleep(400);
        if (FIO_GetFileSize(path, &s2) != 0 || s2 != s1) return 0;
        return 1;
    }
    now = get_ms_clock();
    if (strcmp(settle_path, path) != 0 || settle_size != s1) {
        soup_copy(settle_path, sizeof(settle_path), path);
        settle_size = s1;
        settle_since = now;
        return 0;
    }
    return now - settle_since >= FILM_SETTLE_MS;
}

/* 1 = this number is settled, 0 = not ready yet, -1 = failed. */
static int soup_one(int num, int current)
{
    char cr2[FILM_PATH], jpg[FILM_PATH], flm[FILM_PATH], tmp[FILM_PATH];
    const char *src;
    se_stock stock;
    int is_cr2, t0, rc;

    soup_paths(num, cr2, jpg, flm, tmp, FILM_PATH);
    if (is_file(flm)) return 1;

    if (is_file(cr2)) {
        if (!soup_settled(cr2, FILM_MIN_CR2)) return 0;
        src = cr2; is_cr2 = 1;
    } else if (is_file(jpg)) {
        if (!soup_settled(jpg, FILM_MIN_JPG)) return 0;
        src = jpg; is_cr2 = 0;
    } else {
        /* Nothing with this number (deleted, or a gap): move on once a
         * later frame exists. */
        if (is_file(tmp)) FIO_RemoveFile(tmp);
        return num < current ? 1 : 0;
    }

    /* Do not compete with the camera for the card mid-burst. */
    while (!job_state_ready_to_take_pic()) msleep(200);

    if (soup_load_stock(film_stock, &stock) != 0) {
        soup_log("stock %s unreadable", film_names[film_stock]);
        return -1;
    }
    {
        char fdir[FILM_PATH];
        soup_film_dir(fdir, sizeof(fdir));
        soup_ensure_dir(fdir);
    }
    if (is_file(tmp)) FIO_RemoveFile(tmp);
    snprintf(film_job, sizeof(film_job), "%s %04d", stock.name, num);
    {
        const char *b = src + strlen(src);
        while (b > src && b[-1] != '/') b--;
        soup_copy(film_file, sizeof(film_file), b);
    }
    film_shown = -1;
    film_shown_ms = 0;
    soup_show(0);

    t0 = get_ms_clock();
    rc = soup_develop(src, is_cr2, tmp, &stock, soup_progress_cb);
    film_last_secs = (get_ms_clock() - t0) / 1000;

    if (rc == SE_OK && is_file(tmp) && FIO_RenameFile(tmp, flm) == 0 && is_file(flm)) {
        film_done_count++;
        snprintf(film_status, sizeof(film_status), "FLM_%04d done, %ds", num, film_last_secs);
        soup_log("FLM_%04d.JPG done in %d s (%s)", num, film_last_secs, stock.name);
        film_batch_done++;
        film_event = 1;
        return 1;
    }
    if (is_file(tmp)) FIO_RemoveFile(tmp);
    film_fail_count++;
    snprintf(film_status, sizeof(film_status), "%04d failed: %s", num, se_strerror(rc));
    film_event = 2;
    return -1;
}

/*
 * Where to start. Only frames shot while Film is ON get developed:
 *
 *   - at power-on, start at the newest frame already on the card, so
 *     nothing that was there before gets touched;
 *   - except to finish what the last session was doing: if ML/SE.NXT says
 *     at most FILM_RESUME frames were still pending when the camera went
 *     off (a develop interrupted by the power switch, or a short burst),
 *     those are picked up;
 *   - switching Film from OFF to ON always starts at the newest frame.
 *
 * (The 50D code looked back up to 20 frames for anything without an FLM
 * file, which on a fresh install meant developing the old shots.)
 */
#define FILM_RESUME 3

static int soup_start_point(int current, int at_boot)
{
    int saved = at_boot ? soup_next_load() : 0;
    int start = current;
    if (saved > 0 && saved < current && current - saved <= FILM_RESUME)
        start = saved;
    soup_next_save(start);
    soup_log("start at %04d (newest %04d%s)", start, current,
             start < current ? ", resuming" : "");
    return start;
}

static void soup_task(void *unused)
{
    int cursor = -1, fails = 0, boot = 1, was_on = 0;
    (void)unused;

    while (!ml_started) msleep(100);
    msleep(3000);                    /* let the camera finish booting */
    {
        struct card_info *c = get_shooting_card();
        soup_log("soup.mo started, %d stocks, film %s, file number %d, dir %s",
                 film_count, film_on ? "ON" : "OFF", c ? c->file_number : -1, get_dcim_dir());
    }

    for (;;) {
        struct card_info *card;
        int current, rc;

        card = get_shooting_card();
        current = card ? card->file_number : 0;

        if (!film_on) {
            film_busy = 0;
            /* OFF leaves every file alone, including ones still queued */
            if (was_on && current > 0) soup_next_save(current);
            was_on = 0;
            boot = 0;
            cursor = -1;
            snprintf(film_status, sizeof(film_status), "off");
            msleep(500);
            continue;
        }
        if (is_movie_mode() || current <= 0) { msleep(500); continue; }
        if (cursor < 0 || cursor > current) {
            /* power-on, Film switched on, or the numbering wrapped */
            cursor = soup_start_point(current, boot);
            boot = 0;
            fails = 0;
        }
        was_on = 1;
        if (cursor >= current) {
            if (!fails) snprintf(film_status, sizeof(film_status), "idle, %d done", film_done_count);
            if (film_busy)
                film_batch_secs = (get_ms_clock() - film_batch_t0 + 500) / 1000;
            film_busy = 0;
            msleep(500);
            continue;
        }

        /* a batch is whatever is queued from the moment work starts; it
         * grows if more frames are shot while it runs */
        if (!film_busy) {
            film_busy = 1;
            film_pct = 0;
            film_batch_done = 0;
            film_batch_total = current - cursor;
            film_batch_t0 = get_ms_clock();
        }
        if (film_batch_done + (current - cursor) > film_batch_total)
            film_batch_total = film_batch_done + (current - cursor);

        rc = soup_one(cursor + 1, current);
        if (rc > 0) {
            cursor++;
            fails = 0;
            soup_next_save(cursor);
        } else if (rc < 0) {
            /* Give a broken frame three tries, then skip it so one bad
             * file cannot stall everything shot after it. */
            if (++fails >= 3) {
                soup_log("giving up on %04d", cursor + 1);
                cursor++;
                fails = 0;
                soup_next_save(cursor);
            } else {
                msleep(2000);
            }
        } else {
            msleep(500);
        }
    }
}

/* ------------------------------------------------- picker accessors ---- */

int         soup_ui_count(void)     { return film_count; }
const char *soup_ui_name(int i)     { return (i >= 0 && i < film_count) ? film_names[i] : "?"; }
const char *soup_ui_desc(int i)     { return (i >= 0 && i < film_count) ? film_descs[i] : ""; }
int         soup_ui_current(void)   { return film_stock; }
int         soup_ui_film_on(void)   { return film_on; }

void soup_ui_get_progress(soup_progress *p)
{
    p->busy  = film_busy;
    p->pct   = film_pct;
    p->done  = film_batch_done;
    p->total = film_batch_total > 0 ? film_batch_total : 1;
    p->last_secs = film_busy ? film_last_secs : film_batch_secs;
    p->file  = film_file;
}

int soup_ui_take_event(void)
{
    int e = film_event;
    film_event = 0;
    return e;
}

void soup_ui_choose(int i)
{
    if (i < 0 || i >= film_count) return;
    film_stock = i;
    film_on = 1;
    film_saved_on = film_on;
    film_saved_stock = film_stock;
    soup_write_cfg();
}

static MENU_SELECT_FUNC(soup_menu_select)
{
    soup_ui_open_from_menu();
}

/* ------------------------------------------------------------- menu ---- */

static struct menu_entry soup_menu[];

static MENU_UPDATE_FUNC(soup_save_update)
{
    if (film_stock < 0 || film_stock >= film_count) film_stock = 0;
    if (film_on != film_saved_on || film_stock != film_saved_stock) {
        film_saved_on = film_on;
        film_saved_stock = film_stock;
        soup_write_cfg();
    }
}

static MENU_UPDATE_FUNC(soup_main_update)
{
    soup_save_update(entry, info);
    if (is_movie_mode())
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Paused in movie mode.");
    if (film_on)
        MENU_SET_VALUE("ON, %s", film_names[film_stock]);
}

static MENU_UPDATE_FUNC(soup_status_update)
{
    MENU_SET_VALUE("%s", film_status);
    MENU_SET_RINFO("%d ok / %d failed", film_done_count, film_fail_count);
}

static struct menu_entry soup_menu[] = {
    {
        .name    = "Soup Film",
        .priv    = &film_on,
        .max     = 1,
        .select  = soup_menu_select,
        .update  = soup_main_update,
        .help    = "SET: film picker (also SET on the shooting screen). Q: options.",
        .help2   = "Writes FILM/xxxCANON/FLM_nnnn.JPG. Originals are not touched.",
        .submenu_width = 700,
        .children = (struct menu_entry[]) {
            {
                .name   = "Film",
                .priv   = &film_on,
                .max    = 1,
                .choices = CHOICES("OFF", "ON"),
                .update = soup_save_update,
                .help   = "OFF leaves every file alone.",
            },
            {
                .name   = "Stock",
                .priv   = &film_stock,
                .min    = 0,
                .max    = 0,          /* set from ML/FILM/INDEX.TXT */
                .choices = film_choices,
                .update = soup_save_update,
                .help   = "Look applied to new shots. From ML/FILM on the card.",
            },
            {
                .name   = "Status",
                .update = soup_status_update,
                .icon_type = IT_ALWAYS_ON,
                .help   = "Last develop. Details in ML/LOGS/SOUP.LOG.",
            },
            MENU_EOL,
        },
    },
};

/* ----------------------------------------------------------- module ---- */

static unsigned int soup_init(void)
{
    struct menu_entry *stock_entry;

    soup_read_index();
    soup_read_cfg();
    if (film_stock < 0 || film_stock >= film_count) film_stock = 0;
    film_saved_on = film_on;
    film_saved_stock = film_stock;

    stock_entry = &soup_menu[0].children[1];
    stock_entry->choices = film_choices;
    stock_entry->max = film_count - 1;

    menu_add("Shoot", soup_menu, COUNT(soup_menu));
    task_create("soup_film", 0x1f, 0x10000, soup_task, 0);
    soup_ui_start();
    film_started = 1;
    return 0;
}

static unsigned int soup_deinit(void)
{
    /* Module unloading only happens at shutdown on these bodies. */
    return 0;
}

MODULE_INFO_START()
    MODULE_INIT(soup_init)
    MODULE_DEINIT(soup_deinit)
MODULE_INFO_END()

MODULE_CBRS_START()
    MODULE_CBR(CBR_KEYPRESS, soup_ui_keypress, 0)
MODULE_CBRS_END()
