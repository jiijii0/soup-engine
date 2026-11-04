/*
 * soupeng.h - what the module's six files share.
 *
 *   soupeng.c   the films, the menu, the config, the module's lifetime
 *   develop.c   one frame: the preview out of the raw, the look, the JPEG
 *   watch.c     the queue, the watcher that fills it, the worker that drains it
 *   screen.c    the label on the shooting screen and what the keys do
 *   ui.c        the film screen and Home's pages, drawn from the pack on the card
 *   prints.c    the previews Prints shows, and the lists of what is on the card
 *
 * Everything here is module-global on purpose: the six files are one
 * program that happens to be in six pieces, and Magic Lantern links the
 * module as a single relocatable object. The se_ prefix keeps the names
 * clear of Magic Lantern's own symbol table, into which every global of
 * a module is exported.
 */
#ifndef SE700_SOUPENG_H
#define SE700_SOUPENG_H

#include <ml.h>

#include "se.h"
#include "stock.h"
#include "cr2.h"
#include "lut.h"
#include "jpeg_dec.h"
#include "jpeg_enc.h"
#include "look.h"

#define SE_MAX_STOCKS   32

/* ---------------------------------------------------------- the films */
extern se_stock *se_stocks;                /* SE_MAX_STOCKS of them, sorted */
extern int       se_nstocks;
extern int       se_stock_index;           /* the live selection            */
extern int       se_stock_id;              /* FNV-1a of its name, saved     */
extern int       se_size;                  /* 0 half, 1 full, 2 quarter     */
extern int       se_auto;                  /* develop every shot            */

int         se_scale(int size);            /* a size as a decode scale      */
const char *se_size_name(int size);
uint32_t    se_name_id(const char *name);  /* what se_stock_id holds        */

/* ----------------------------------------------------------- logging */
void se_logf(const char *fmt, ...);
void se_log_flush(void);

/* ------------------------------------------------------------- utils */
#define FNV_INIT 2166136261u
uint32_t se_hash_bytes(uint32_t h, const void *p, int n);
int      ends_with_ci(const char *n, const char *ext);
uint32_t file_size(const char *path);      /* 0 if it does not exist        */

/* ------------------------------------------------------- the develop */
typedef struct {
    int      ow, oh, scale;
    int      bytes;
    uint32_t hash;
    int      ms_hdr, ms_read, ms_stream, ms_total;
    size_t   arena_peak;
    int      aborted;                      /* power-off during the frame    */
} se_result;

extern volatile int se_busy;               /* a frame is being developed    */
extern const char  *se_stage;              /* its progress, for the screen  */
extern int          se_pct;
extern int          se_waiting;            /* frames queued behind it       */
extern char         se_status[64];         /* what the last develop did     */
extern int          se_done, se_failed;    /* this session                  */

se_err se_develop_preview(const char *cr2_path, const char *out_path,
                          const se_stock *st, int scale, se_result *R);
void   se_progress_hook(void *ctx, const char *stage, int pct);
int    se_progress_text(char *buf, size_t cap);   /* "stream 45%, 3 waiting" */
void   se_count_waiting(void);                    /* se_waiting, read live   */
int    se_newest_cr2(char *out, size_t cap);
void   se_out_name(const char *cr2, char *out, size_t cap);

/* --------------------------------------------------------- the queue */
#define SE_QUEUE 8

typedef struct {
    char     path[FIO_MAX_PATH_LENGTH];
    uint32_t size;          /* last size seen                        */
    int      stable;        /* looks at which the size did not change */
    int      age;           /* looks since queued                     */
    int      ready;         /* 1 develop it, -1 skip it, 0 not yet    */
    int      force;         /* asked for by hand: overwrite, no checks */
    int      stock, quality; /* what it was shot on: the choice at queueing,
                               not whatever is selected when its turn comes */
} se_job;

#define SE_BURST 64                        /* more than this is a reset counter */
extern se_job       se_q[SE_QUEUE];
extern int          se_job_stock;
extern volatile int se_q_head, se_q_tail;  /* indices, never wrap           */

int     se_q_count(void);
int     se_pending(void);                           /* queued, and shot but not yet queued */
se_job *se_q_push(const char *path, int force);     /* once per path */
void    se_resume_cut(void);                        /* at power-on   */

unsigned int se_watch_cbr(unsigned int ctx);     /* once a second         */
void         se_worker_task(void *arg);          /* for the whole session */

/* -------------------------------------------------------- the screen */
/* -------------------------------------------------------------- prints */
#define SE_PV_W      648
#define SE_PV_H      432
#define SE_TH_W      216
#define SE_TH_H      144
#define SE_MAX_LIST  512
extern uint16_t se_prints[SE_MAX_LIST], se_raws[SE_MAX_LIST];  /* newest first */
extern int      se_nprints, se_nraws;
int   se_pv_begin(int width);
void  se_pv_row(const uint8_t *rgb);
void  se_pv_save(int no, const char *film, int size, uint32_t flm_bytes);
void  se_pv_abort(void);
void  se_pv_remove(int no);
int   se_pv_info(int no, char *film, size_t cap, int *size);
int   se_pv_thumb(int no, uint8_t *dst);
int   se_pv_full(int no, uint8_t *dst);
int   se_make_preview(int no);                   /* the worker's */
void  se_walk_card(void);
int   se_file_no(const char *name);              /* FLM_1234.JPG: 1234 */
void  se_flm_path(int no, char *buf, size_t cap);
void  se_cr2_path(int no, char *buf, size_t cap);
extern volatile int se_preview_want, se_preview_gen;     /* UI asks, worker answers */

extern int   se_roll_on, se_roll_no, se_roll_shot;
extern int   se_ui_colour, se_ui_idle;
void         se_rescan(void);

extern int   se_ui_avail;                        /* the pack is on the card */
void         se_draw_status(void);               /* film, or progress     */
void         se_screen_tick(void);               /* from the watcher      */
int          se_menu_is_open(void);
unsigned int se_key_cbr(unsigned int key);       /* translated keys     */
unsigned int se_raw_cbr(se_ctx_t ctx);           /* then the raw events */
void         se_ui_task(void *arg);              /* for the whole session */

/* ------------------------------------------------------ the film screen */
int   se_ui_open(void);                          /* 0, or -1: use the list */
void  se_ui_close(int shutting_down);
int   se_ui_stock(int pos);                      /* carousel place to film */
int   se_ui_pos(int stock);
int   se_ui_count(void);
int   se_ui_shelf_jump(int pos, int dir);
void  se_ui_paint(int pos);
void  se_ui_slide(int pos, int dir, int sub);
void  se_ui_paint_header(void);
int   se_ui_header_stale(void);
void  se_ui_reflip(void);
void  se_ui_pal_tick(void);
void  se_ui_loaded(volatile int *stop);
void  se_ui_toast(const char *text);
int   se_ui_toast_live(void);

/* the pages behind Home */
enum { PG_FILM, PG_HOME, PG_DARK, PG_PRINTS, PG_VIEW, PG_LENS, PG_SYSTEM };
#define SE_HOME_ROWS 6
#define SE_SYS_ROWS  4
extern int se_home_sel, se_dark_sel, se_print_sel, se_sys_sel, se_ui_sheet;
void  se_ui_enter(int page);                     /* task: palette, buffers, lists */
void  se_ui_paint_page(int page);
int   se_ui_prints_tick(int page);               /* 1: something arrived, repaint */
int   se_ui_dark_count(void);
int   se_ui_dark_raw(int row);                   /* a row's raw number, or -1 */
void  se_ui_apply_palette(void);

#endif
