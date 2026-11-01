/*
 * watch.c - the queue, the watcher that fills it, the worker that drains it.
 *
 * How a shot is noticed. Magic Lantern defines "after shot" callbacks
 * for modules and this build never fires them, so the watcher polls:
 * once a second, from Magic Lantern's own shoot task, it reads Canon's
 * file counter. A new number is a new frame. The frame is queued once
 * Canon reports itself idle and the raw's size has stopped changing,
 * because the counter can move before the file is finished.
 *
 * One worker task drains the queue. It is started once, at init, and
 * loops until power-off; it does not touch the card once shutdown has
 * been requested, because a task holding the card open is a shutdown
 * that does not finish.
 */
#include "soupeng.h"

volatile int se_busy;
char         se_status[64];
int          se_done, se_failed;

/*
 * Frames waiting to be developed. Filled by the watcher (once a second,
 * on Magic Lantern's shoot task) and by "Develop last shot" (on the GUI
 * task); drained by the worker. Three tasks, one array: the watcher and
 * the menu only ever append at `tail`, the worker only ever reads at
 * `head`, and each index is written by exactly one side, which is the
 * whole of the locking.
 *
 * A frame is WAITING until Canon is idle and its size has held still
 * for two consecutive looks; then it is READY. A frame that never
 * appears -- JPEG-only shooting, or a counter that moved for some other
 * reason -- is dropped after a while.
 */
se_job       se_q[SE_QUEUE];
volatile int se_q_head, se_q_tail;     /* indices, never wrap   */
volatile int se_preview_want, se_preview_gen;
int          se_job_stock;             /* the film of the frame in the works */

int se_q_count(void) { return se_q_tail - se_q_head; }

/* Frames shot while the queue was full: the watcher has not reached them. */
static volatile int se_backlog;
int se_pending(void) { return se_q_count() + se_backlog; }

se_job *se_q_push(const char *path, int force)
{
    se_job *j;
    int i;
    /* Already waiting? Then this is the same request twice -- "Develop
     * last shot" pressed while the watcher had just queued that shot --
     * and a second job would develop the frame a second time. */
    for (i = se_q_head; i < se_q_tail; i++) {
        j = &se_q[i % SE_QUEUE];
        if (j->ready >= 0 && strcmp(j->path, path) == 0) {
            if (force) { j->force = 1; j->ready = 1; }
            return j;
        }
    }
    if (se_q_count() >= SE_QUEUE) {
        se_logf("queue full, dropping %s", path);
        return 0;
    }
    j = &se_q[se_q_tail % SE_QUEUE];
    memset(j, 0, sizeof *j);
    snprintf(j->path, sizeof j->path, "%s", path);
    j->force = force;
    j->ready = force;
    j->stock = se_stock_index;
    j->quality = se_size;
    se_q_tail++;
    return j;
}

/*
 * A frame cut off by the power -- battery out, card pulled -- leaves its
 * FLM_ file at zero bytes, because the output goes out in 256 KB pieces
 * and the log is flushed after the job. A clean shutdown removes the
 * partial file; a cut cannot. So at power-on the shooting folder is
 * walked once and any raw whose output is empty is queued again.
 */
void se_resume_cut(void)
{
    const char *dir = get_dcim_dir();
    struct fio_file *file = alloc_fio_file();
    struct fio_dirent *d;
    char in[FIO_MAX_PATH_LENGTH], out[FIO_MAX_PATH_LENGTH];
    uint32_t sz;
    int n = 0;

    if (!file) return;
    d = FIO_FindFirstEx(dir, file);
    if (IS_ERROR(d)) { free(file); return; }
    do {
        struct file_info fi = convert_fio_file_info(file);
        if (!fi.name[0] || (fi.mode & ATTR_DIRECTORY)) continue;
        if (!ends_with_ci(fi.name, ".CR2")) continue;
        snprintf(in, sizeof in, "%s/%s", dir, fi.name);
        se_out_name(in, out, sizeof out);
        if (FIO_GetFileSize(out, &sz) == 0 && sz == 0 && se_q_push(in, 1)) {
            se_logf("resuming %s: its output was cut off", fi.name);
            n++;
        }
    } while (FIO_FindNextEx(d, file) == 0 && n < SE_QUEUE);
    FIO_FindClose(d);
    free(file);
}

/* ---------------------------------------------------------- the watcher */

static int se_last_file_number = -1;
static int se_last_folder_number = -1;

/*
 * Once a second, from Magic Lantern's shoot task.
 *
 * Canon's counter is the number of the last frame taken. It advances the
 * moment a frame is taken, which can be before the raw has finished
 * writing, so a new number only gets the frame QUEUED; it is developed
 * once lens_info.job_state is back to zero -- Canon's own "not writing"
 * -- and the file's size has held still across two looks. The 5D did
 * the same job with a directory walk every two seconds; the counter is
 * free.
 */
unsigned int se_watch_cbr(unsigned int ctx)
{
    struct card_info *card = get_shooting_card();
    int num = card->file_number, folder = card->folder_number;
    int i;

    /* the first look is the baseline: nothing already on the card is new */
    if (se_last_file_number < 0) {
        se_last_file_number = num;
        se_last_folder_number = folder;
        return 0;
    }

    if (num != se_last_file_number || folder != se_last_folder_number) {
        /* Queue only with auto on, but follow the counter either way: a
         * shot taken with auto off is not a backlog to develop the moment
         * it is switched back on. */
        int reached = num;
        int ahead = (num - se_last_file_number + 9999) % 9999;
        if (se_roll_on && ahead <= SE_BURST) se_roll_shot += ahead;
        /* A counter that moved a long way is not a burst: it was reset, or
         * this is another card. Nothing to catch up on. */
        if (se_auto && se_nstocks && ahead <= SE_BURST) {
            int n = se_last_file_number;
            /* a burst can move the counter by more than one between looks,
             * and one longer than the queue waits here, unread, until there
             * is room: stopping short keeps those frames for the next look */
            while (n != num && se_q_count() < SE_QUEUE) {
                char path[FIO_MAX_PATH_LENGTH];
                int next = n >= 9999 ? 1 : n + 1;
                snprintf(path, sizeof path, "%s/%s%04d.CR2", get_dcim_dir(), get_file_prefix(), next);
                if (!se_q_push(path, 0)) break;
                n = next;
            }
            reached = n;
        }
        se_last_file_number = reached;
        se_last_folder_number = folder;
    }
    se_backlog = se_auto ? (num - se_last_file_number + 9999) % 9999 : 0;

    se_screen_tick();               /* the label, or the picker's timeout */

    /* ripen what is waiting */
    for (i = se_q_head; i < se_q_tail; i++) {
        se_job *j = &se_q[i % SE_QUEUE];
        uint32_t sz;
        if (j->ready) continue;
        j->age++;
        sz = file_size(j->path);
        if (sz && sz == j->size) j->stable++; else j->stable = 0;
        j->size = sz;
        if (sz > (1u << 20) && j->stable >= 1 && lens_info.job_state == 0)
            j->ready = 1;
        else if (j->age > 30) {
            se_logf("gave up waiting for %s", j->path);
            j->ready = -1;          /* the worker skips it */
        }
    }
    return 0;
}

/* ----------------------------------------------------------- the worker */

static void se_develop_job(se_job *j)
{
    char out[FIO_MAX_PATH_LENGTH];
    const se_stock *st;
    se_result R;
    se_err rc;
    int scale = se_scale(j->quality);

    if (j->stock < 0 || j->stock >= se_nstocks) j->stock = 0;
    st = &se_stocks[j->stock];
    se_job_stock = j->stock;
    se_out_name(j->path, out, sizeof out);

    if (!j->force && file_size(out) > 0) {
        se_logf("%s already developed", out);
        return;
    }

    se_busy = 1;
    se_stage = "starting"; se_pct = 0;
    se_waiting = se_q_count() > 1 ? se_q_count() - 1 : 0;
    se_logf("develop %s + %s, %s size, %d waiting, %d KB free", j->path, st->name,
            se_size_name(j->quality), se_waiting, GetFreeMemForMalloc() >> 10);

    rc = se_develop_preview(j->path, out, st, scale, &R);

    if (rc == SE_OK) {
        se_done++;
        snprintf(se_status, sizeof se_status, "%s %d.%ds",
                 out + strlen(get_dcim_dir()) + 1, R.ms_total / 1000, (R.ms_total % 1000) / 100);
        se_logf("wrote %s: %dx%d, %d KB, look hash %08x", out, R.ow, R.oh,
                R.bytes >> 10, R.hash);
        se_logf("time: hdr %d read %d stream %d total %d ms; arena peak %d KB",
                R.ms_hdr, R.ms_read, R.ms_stream, R.ms_total, (int)(R.arena_peak >> 10));
        if (!ml_shutdown_requested && !se_menu_is_open())
            NotifyBox(1500, "Soup: %s %d.%d s", st->name, R.ms_total / 1000, (R.ms_total % 1000) / 100);
    } else if (R.aborted) {
        se_logf("%s abandoned: power off", j->path);
    } else {
        se_failed++;
        snprintf(se_status, sizeof se_status, "failed: %s", se_strerror(rc));
        se_logf("develop failed: %s (rc=%d)", se_strerror(rc), rc);
        if (!ml_shutdown_requested && !se_menu_is_open())
            NotifyBox(3000, "Soup: failed, %s", se_strerror(rc));
    }
    se_busy = 0;
    se_log_flush();
}

/*
 * Lives for the whole session. Polls rather than blocks on a semaphore:
 * half a second of latency on a job that takes forty is nothing, and it
 * keeps the queue's locking as simple as it is.
 *
 * Canon's auto power-off is held while there is work. A full-size frame
 * takes three minutes and the default idle timer is one, so without this
 * the camera would switch itself off mid-develop -- and shutdown, which
 * is honoured within a row, would abandon the frame. Magic Lantern's
 * file manager and raw recorder hold it the same way. Only the worker
 * calls these two, so they always pair.
 */
void se_worker_task(void *arg)
{
    int held = 0;

    while (!ml_shutdown_requested) {
        int pending = se_q_count() > 0;

        /* someone is looking at a print with no preview: first in the line */
        if (se_preview_want) {
            int no = se_preview_want;
            if (se_make_preview(no) != 0) se_logf("no preview for FLM_%04d", no);
            se_preview_want = 0;
            se_preview_gen++;
            continue;
        }

        if (pending && !held) { powersave_prohibit(); held = 1; }
        if (!pending && held) { powersave_permit();   held = 0; }

        if (pending) {
            se_job *j = &se_q[se_q_head % SE_QUEUE];
            if (j->ready > 0) {
                se_develop_job(j);
                se_q_head++;
            } else if (j->ready < 0) {
                se_q_head++;
            } else {
                msleep(500);         /* the watcher is still ripening it */
            }
            continue;
        }
        msleep(500);
    }
    if (held) powersave_permit();
}
