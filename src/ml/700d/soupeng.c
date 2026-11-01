/*
 * soupeng.c - Soup Engine as a Magic Lantern module, for the EOS 700D.
 *
 * The 5D Classic build (ml/payload/) patches itself into Magic Lantern's
 * binary and calls Canon's ROM by absolute address, because that body's
 * Magic Lantern port has no module loader. This one does: it loads .mo
 * files off the card at boot and resolves what they import against its
 * own symbol table. So this is an ordinary module, and everything it
 * needs from the camera -- files, memory, tasks, the menu, the screen --
 * is asked of Magic Lantern rather than taken from the firmware.
 *
 * This file is the films, the menu, the config and the module's
 * lifetime. develop.c is one frame, watch.c is the queue and the tasks
 * around it, screen.c is the label and the keys, ui.c is the film screen;
 * soupeng.h is what they share. tools/sim700d runs all five on the desktop
 * against a mock of the camera, which is where the queue and the keys are
 * tested.
 */
#include "soupeng.h"

#define SE_STOCK_TEXT   8192      /* soup800.fs, the longest, is under 3 KB */

/* ---------------------------------------------------------------- state */

se_stock *se_stocks;                       /* SE_MAX_STOCKS of them */
static char      se_stock_file[SE_MAX_STOCKS][16];/* the 8.3 name on card */
int       se_nstocks;
static int       se_film_dir_missing;

/*
 * Remembered across power cycles by Magic Lantern's config system.
 *
 * The film is remembered by NAME, as the 5D does it: the list is whatever
 * is in ML/FILM, and a position in it silently changes meaning the moment
 * a film is added or removed. Magic Lantern's config holds integers, so
 * what it holds is a hash of the name; at boot the list is searched for
 * the film that hashes to it. The index is the live selection and is not
 * saved.
 */
int se_stock_index;
CONFIG_INT("soupeng.film",   se_stock_id,    0);   /* FNV-1a of the name */
CONFIG_INT("soupeng.size",   se_size,        0);   /* 0 half, 1 full, 2 quarter */

/* The numbering is the order the choices were added in, so a saved
 * config keeps meaning what it meant. */
int se_scale(int size) { return size == 1 ? 1 : size == 2 ? 4 : 2; }
const char *se_size_name(int size) { return size == 1 ? "full" : size == 2 ? "quarter" : "half"; }
CONFIG_INT("soupeng.auto",   se_auto,        1);   /* develop every shot */
CONFIG_INT("soupeng.roll",   se_roll_on,     0);   /* count a roll: no size, a counter */
CONFIG_INT("soupeng.rollno", se_roll_no,     1);
CONFIG_INT("soupeng.rollct", se_roll_shot,   0);
CONFIG_INT("soupeng.colour", se_ui_colour,   0);   /* 0 night, 1 ember, 2 mono */
CONFIG_INT("soupeng.idle",   se_ui_idle,    20);   /* seconds */

static void se_menu_film_max(void);       /* defined below the menu table  */


/* --------------------------------------------------------------- logging */
/*
 * Lines go to Magic Lantern's console at once and to ML/LOGS/SOUPENG.LOG
 * when flushed. The file is written only between develops, never during
 * one: the 5D learnt that opening a log on the card in the middle of
 * streaming a frame off it gave the card something else to do at exactly
 * the wrong moment, and there is no reason to find out whether this body
 * minds.
 *
 * Formats are kept to %d %s %x. The printf here is Canon's: no %u, no
 * %.*s, no width flags -- it prints the format text literally instead.
 */
#define SE_LOG_CAP 4096
static char se_logbuf[SE_LOG_CAP];
static int  se_loglen;

void se_logf(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n++] = '\n';
    line[n] = 0;

    printf("soup: %s", line);
    if (se_loglen + n < SE_LOG_CAP) {
        memcpy(se_logbuf + se_loglen, line, (size_t)n);
        se_loglen += n;
    }
}

void se_log_flush(void)
{
    FILE *f;
    if (!se_loglen || ml_shutdown_requested) return;
    f = FIO_CreateFileOrAppend("ML/LOGS/SOUPENG.LOG");
    if (f) {
        FIO_WriteFile(f, se_logbuf, (size_t)se_loglen);
        FIO_CloseFile(f);
    }
    se_loglen = 0;
}

/* The engine never prints; it calls this. */
static void se_log_hook(void *ctx, const char *msg)
{
    se_logf("engine: %s", msg);
}

/* ---------------------------------------------------------------- utils */

int ends_with_ci(const char *n, const char *ext)
{
    size_t l = strlen(n), e = strlen(ext);
    size_t i;
    if (l < e) return 0;
    for (i = 0; i < e; i++) {
        char a = n[l - e + i], b = ext[i];
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    return 1;
}

/* FNV-1a, the same hash tools/previewcheck.c prints. */
uint32_t se_hash_bytes(uint32_t h, const void *p, int n)
{
    const uint8_t *b = (const uint8_t *)p;
    int i;
    for (i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

/*
 * Read a small text file whole. buf gets a terminating NUL.
 *
 * buf must come from fio_malloc: Magic Lantern's FIO_ReadFile refuses a
 * cacheable buffer above 8 KB, because Canon's FIO reads by DMA and a
 * cached buffer would be read back from the cache, not from the data.
 */
static int read_text(const char *path, char *buf, size_t cap, size_t *got)
{
    uint32_t size;
    FILE *f;
    int n;

    if (FIO_GetFileSize(path, &size) != 0) return -1;
    if (size >= cap) return -1;
    f = FIO_OpenFile(path, O_RDONLY | O_SYNC);
    if (!f) return -1;
    n = FIO_ReadFile(f, buf, size);
    FIO_CloseFile(f);
    if (n != (int)size) return -1;
    buf[n] = 0;
    *got = (size_t)n;
    return 0;
}

/* Size of a file, or 0 if it does not exist. */
uint32_t file_size(const char *path)
{
    uint32_t sz = 0;
    if (FIO_GetFileSize(path, &sz) != 0) return 0;
    return sz;
}

/* ------------------------------------------------------------ the films */

static void se_sort_stocks(void)
{
    /* insertion sort by name; a dozen entries, once per scan */
    int i, j;
    for (i = 1; i < se_nstocks; i++) {
        se_stock tmp = se_stocks[i];
        char tmpf[16];
        memcpy(tmpf, se_stock_file[i], sizeof tmpf);
        for (j = i; j > 0 && strcmp(se_stocks[j - 1].name, tmp.name) > 0; j--) {
            se_stocks[j] = se_stocks[j - 1];
            memcpy(se_stock_file[j], se_stock_file[j - 1], sizeof tmpf);
        }
        se_stocks[j] = tmp;
        memcpy(se_stock_file[j], tmpf, sizeof tmpf);
    }
}

/*
 * Walk ML/FILM on the card Magic Lantern booted from and parse every
 * .fs file. The 5D build reads an INDEX.TXT instead, because that body's
 * FindNext was unreliable; here the directory walk is Magic Lantern's
 * own, used by its file manager, so the index is not needed.
 */
static void se_scan_stocks(void)
{
    char dir[32], path[64];
    char *text;
    struct fio_file *file;
    struct fio_dirent *d;
    const char *drive = get_ml_card()->drive_letter;

    se_nstocks = 0;
    se_film_dir_missing = 0;
    snprintf(dir, sizeof dir, "%s:/ML/FILM", drive);

    text = fio_malloc(SE_STOCK_TEXT);
    file = alloc_fio_file();
    if (!text || !file) {
        se_logf("out of memory scanning films");
        if (text) fio_free(text);
        if (file) free(file);
        return;
    }

    d = FIO_FindFirstEx(dir, file);
    if (IS_ERROR(d)) {
        se_film_dir_missing = 1;
        se_logf("no %s on the card", dir);
        fio_free(text);
        free(file);
        return;
    }

    do {
        struct file_info fi = convert_fio_file_info(file);
        size_t n;
        se_stock *s;

        if (!fi.name[0] || fi.name[0] == '.') continue;   /* exFAT quirk; dotfiles */
        if (fi.mode & ATTR_DIRECTORY) continue;
        if (!ends_with_ci(fi.name, ".fs")) continue;
        if (se_nstocks >= SE_MAX_STOCKS) {
            se_logf("more than %d films, ignoring the rest", SE_MAX_STOCKS);
            break;
        }

        snprintf(path, sizeof path, "%s/%s", dir, fi.name);
        if (read_text(path, text, SE_STOCK_TEXT, &n) != 0) {
            se_logf("cannot read %s", fi.name);
            continue;
        }
        s = &se_stocks[se_nstocks];
        se_stock_init(s);
        if (se_stock_parse(text, n, s) != SE_OK) {
            se_logf("cannot parse %s", fi.name);
            continue;
        }
        snprintf(se_stock_file[se_nstocks], sizeof se_stock_file[0], "%s", fi.name);
        se_nstocks++;
    } while (FIO_FindNextEx(d, file) == 0);

    FIO_FindClose(d);
    fio_free(text);
    free(file);

    se_sort_stocks();
    se_logf("%d films in %s", se_nstocks, dir);
}

/*
 * The remembered film. FNV-1a of its name is what the config holds; the
 * list is searched for it at boot and after a rescan, and the first film
 * is the fallback when nothing matches -- a film that was deleted, or a
 * fresh card.
 */
uint32_t se_name_id(const char *n)
{
    uint32_t h = FNV_INIT;
    while (*n) { h ^= (uint8_t)*n++; h *= 16777619u; }
    return h;
}

static void se_select_remembered(void)
{
    int i;
    se_stock_index = 0;
    for (i = 0; i < se_nstocks; i++)
        if ((int)se_name_id(se_stocks[i].name) == se_stock_id) { se_stock_index = i; break; }
}

/* ---------------------------------------------------------------- menu */

static MENU_UPDATE_FUNC(se_film_update)
{
    if (se_nstocks == 0) {
        MENU_SET_VALUE(se_film_dir_missing ? "no ML/FILM" : "none found");
        MENU_SET_HELP("Put the .fs film files in ML/FILM on the card, then Rescan.");
        return;
    }
    if (se_stock_index < 0 || se_stock_index >= se_nstocks)
        se_stock_index = 0;
    se_stock_id = (int)se_name_id(se_stocks[se_stock_index].name);   /* what gets saved */
    MENU_SET_VALUE("%s", se_stocks[se_stock_index].name);
    MENU_SET_HELP("%s", se_stocks[se_stock_index].desc);
    MENU_SET_RINFO("%d/%d", se_stock_index + 1, se_nstocks);
}

static MENU_UPDATE_FUNC(se_develop_update)
{
    if (se_busy) {
        char text[48];
        se_count_waiting();
        se_progress_text(text, sizeof text);      /* "develop 45%, 3 waiting" */
        MENU_SET_VALUE("%s", text);
    } else if (se_q_count()) {
        MENU_SET_VALUE("%d waiting", se_q_count());
    } else if (se_status[0]) {
        MENU_SET_VALUE("%s", se_status);
    } else {
        MENU_SET_VALUE("");
    }
}

static MENU_SELECT_FUNC(se_develop_select)
{
    char cr2[FIO_MAX_PATH_LENGTH];
    if (!se_nstocks) { NotifyBox(2000, "Soup: no films in ML/FILM"); return; }
    if (se_newest_cr2(cr2, sizeof cr2) != 0) {
        NotifyBox(2000, "Soup: no CR2 in %s", get_dcim_dir());
        return;
    }
    if (se_q_push(cr2, 1))
        NotifyBox(1000, "Soup: %s %s", se_busy ? "queued" : "developing", cr2 + strlen(get_dcim_dir()) + 1);
}

static MENU_UPDATE_FUNC(se_auto_update)
{
    if (se_auto && !se_nstocks) MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "No films in ML/FILM.");
    if (se_done || se_failed) MENU_SET_RINFO("%d done, %d failed", se_done, se_failed);
}

static MENU_UPDATE_FUNC(se_rescan_update)
{
    MENU_SET_VALUE("%d found", se_nstocks);
}

void se_rescan(void)
{
    /* a queued frame holds its film by place in the list */
    if (se_busy || se_q_count()) return;
    se_scan_stocks();
    se_select_remembered();
    se_menu_film_max();
    se_log_flush();
}

static MENU_SELECT_FUNC(se_rescan_select) { se_rescan(); }

/* The top-level line shows the chosen film without opening the submenu. */
static MENU_UPDATE_FUNC(se_top_update)
{
    if (se_busy) {
        char text[48];
        se_count_waiting();
        se_progress_text(text, sizeof text);
        MENU_SET_VALUE("%s", text);
    } else if (se_nstocks && se_stock_index >= 0 && se_stock_index < se_nstocks)
        MENU_SET_VALUE("%s%s", se_stocks[se_stock_index].name, se_auto ? "" : " (manual)");
    else
        MENU_SET_VALUE("no films");
    /* Their Advanced Bracket is a few rows up in this same menu and one
     * SET press turns it on; then every shutter press is a sequence. */
    if (is_hdr_bracketing_enabled())
        MENU_SET_WARNING(MENU_WARN_ADVICE, "Advanced Bracket (above) is ON: each shutter press takes several frames.");
}

static struct menu_entry se_menu[] =
{
    {
        .name           = "Soup Engine",
        .select         = menu_open_submenu,
        .update         = se_top_update,
        .help           = "Film looks, in the camera. INFO in shooting mode opens the film screen.",
        .help2          = "Films are the .fs files in ML/FILM on the card.",
        .submenu_width  = 710,
        .children       = (struct menu_entry[]) {
            {
                .name       = "Film",
                .priv       = &se_stock_index,
                .min        = 0,
                .max        = 0,        /* set once the card has been scanned */
                .update     = se_film_update,
                .icon_type  = IT_DICE,
                .help       = "The look every photo gets from now on.",
            },
            {
                .name       = "Auto develop",
                .priv       = &se_auto,
                .max        = 1,
                .update     = se_auto_update,
                .help       = "Develop every shot as it is taken. FLM_1234.JPG beside each raw.",
                .help2      = "Keep shooting while it works; frames queue up. The raw is never modified.",
            },
            {
                .name       = "Develop last shot",
                .select     = se_develop_select,
                .update     = se_develop_update,
                .icon_type  = IT_ACTION,
                .help       = "Develop the newest raw on the card with this film, now.",
                .help2      = "Also how to redo a frame after changing the film.",
            },
            {
                .name       = "Size",
                .priv       = &se_size,
                .max        = 2,
                .choices    = CHOICES("Half", "Full", "Quarter"),
                .help       = "Half is 2592x1728, about 40 s. Full is 18 MP, about 3 min.",
                .help2      = "Quarter is 1296x864 and quick: a frame to share, not to print.",
            },
            {
                .name       = "Rescan films",
                .select     = se_rescan_select,
                .update     = se_rescan_update,
                .icon_type  = IT_ACTION,
                .help       = "Read ML/FILM again after changing the files on the card.",
            },
            MENU_EOL,
        },
    },
};

/* the Film entry's toggle range is the list's length */
static void se_menu_film_max(void)
{
    se_menu[0].children[0].max = se_nstocks > 0 ? se_nstocks - 1 : 0;
}

/* ------------------------------------------------------------- lifetime */

static unsigned int se_module_init(void)
{
    se_set_log(se_log_hook, NULL);
    se_set_progress(se_progress_hook, NULL);

    se_stocks = malloc(sizeof(se_stock) * SE_MAX_STOCKS);
    if (!se_stocks) {
        se_logf("cannot allocate %d bytes for the films",
                (int)(sizeof(se_stock) * SE_MAX_STOCKS));
        se_log_flush();
        return 1;
    }
    se_scan_stocks();
    se_select_remembered();
    se_menu_film_max();
    se_ui_avail = se_nstocks && file_size("ML/DATA/SOUPUI.BIN") > 0;
    FIO_CreateDirectory("ML/PRINTS");

    menu_add("Shoot", se_menu, COUNT(se_menu));
    if (se_nstocks) se_resume_cut();
    task_create("soup_dev", 0x1c, 0x8000, se_worker_task, 0);
    /* above the develop, or the screen would stall behind it for a frame */
    task_create("soup_ui", 0x1b, 0x2000, se_ui_task, 0);

    /* The build's date and time, so the log says which soupeng.mo ran:
     * a card that kept an older copy looks exactly like a new bug. */
    se_logf("soupeng built %s %s; film screen %s; engine format %d, %d films, film %s, size %s, auto %s; free: malloc %d KB, AllocateMemory %d KB",
            __DATE__, __TIME__, se_ui_avail ? "on" : "off (no ML/DATA/SOUPUI.BIN)",
            SE_STOCK_VERSION, se_nstocks, se_nstocks ? se_stocks[se_stock_index].name : "-",
            se_size_name(se_size), se_auto ? "on" : "off",
            GetFreeMemForMalloc() >> 10, GetFreeMemForAllocateMemory() >> 10);
    se_log_flush();
    return 0;
}

static unsigned int se_module_deinit(void)
{
    menu_remove("Shoot", se_menu, COUNT(se_menu));
    if (se_stocks) { free(se_stocks); se_stocks = NULL; }
    return 0;
}

MODULE_INFO_START()
    MODULE_INIT(se_module_init)
    MODULE_DEINIT(se_module_deinit)
MODULE_INFO_END()

MODULE_CBRS_START()
    MODULE_CBR(CBR_SECONDS_CLOCK, se_watch_cbr, 0)
    MODULE_CBR(CBR_KEYPRESS,      se_key_cbr,   0)   /* in this order: the raw */
    MODULE_CBR(CBR_KEYPRESS_RAW,  se_raw_cbr,   0)   /* one sees what this passes */
MODULE_CBRS_END()

MODULE_CONFIGS_START()
    MODULE_CONFIG(se_stock_id)
    MODULE_CONFIG(se_size)
    MODULE_CONFIG(se_auto)
    MODULE_CONFIG(se_roll_on)
    MODULE_CONFIG(se_roll_no)
    MODULE_CONFIG(se_roll_shot)
    MODULE_CONFIG(se_ui_colour)
    MODULE_CONFIG(se_ui_idle)
MODULE_CONFIGS_END()
