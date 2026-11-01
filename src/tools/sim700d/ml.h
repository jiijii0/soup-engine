/*
 * ml.h (simulator) - Magic Lantern, faked on the desktop.
 *
 * This stands in for ml/700d/ml.h so that the module's five real sources
 * can be compiled and run on a machine where they can be debugged. The
 * module reaches Magic Lantern through that one header and nothing else,
 * so substituting it is enough.
 *
 * As with the 5D's camsim, the point is to behave as badly as the real
 * thing is allowed to, where that is cheap:
 *
 *   - fio_malloc comes out of a pool the size Magic Lantern's reported
 *     (3 MB), so a buffer that would not fit on the camera is refused here;
 *   - FIO_ReadFile returns short counts, which is what the read_at loop
 *     exists for;
 *   - the card is a directory, so what the module wrote can be compared
 *     with what the desktop computes, byte for byte;
 *   - the screen is a 720x480 grid and a draw outside it is an error;
 *   - the bitmap layer is real memory with a real palette behind it, so
 *     what the film screen paints can be looked at, and the palette, the
 *     front buffer and the memory can all be checked after it closes;
 *   - task_create makes a real thread, msleep really sleeps, and
 *     ml_shutdown_requested really is read by the worker.
 *
 * The driver, sim.c, owns the fakes' state and reads it back as assertions.
 */
#ifndef SE700_ML_H
#define SE700_ML_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

/* ------------------------------------------------------------- libc */
/* The module calls these through Magic Lantern's declarations. Here they
 * are the host's, declared by hand so the module never includes a host
 * header -- which would bring the host's FILE with it. The driver, which
 * does include the host's headers, skips this block. */
#ifndef SE700_SIM_DRIVER
int    printf(const char *fmt, ...);
int    snprintf(char *s, size_t n, const char *fmt, ...);
int    vsnprintf(char *s, size_t n, const char *fmt, va_list ap);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
char  *strcpy(char *d, const char *s);
void  *memcpy(void *d, const void *s, size_t n);
void  *memset(void *d, int c, size_t n);
void  *malloc(size_t n);
void   free(void *p);
#endif

/* --------------------------------------------------------------- FIO */
typedef struct sim_file FILE;

#ifndef O_RDONLY
#define O_RDONLY 0
#endif
#ifndef O_SYNC
#define O_SYNC   010000
#endif
#define SEEK_SET 0
#define FIO_MAX_PATH_LENGTH 0x80
#define ATTR_DIRECTORY 0x10
#define IS_ERROR(p) (1 & (uintptr_t)(p))

struct fio_file;
struct fio_dirent;
struct file_info { uint16_t mode; uint32_t size; uint64_t timestamp; char name[FIO_MAX_PATH_LENGTH]; };
struct card_info { char *drive_letter; char *type; int cluster_size; int free_space_raw;
                   int file_number; int folder_number; char *maker; char *model; };

FILE  *FIO_OpenFile(const char *name, unsigned mode);
FILE  *FIO_CreateFile(const char *name);
FILE  *FIO_CreateFileOrAppend(const char *name);
int    FIO_ReadFile(FILE *f, void *buf, size_t n);
int    FIO_WriteFile(FILE *f, const void *buf, size_t n);
void   FIO_CloseFile(FILE *f);
int    FIO_GetFileSize(const char *name, uint32_t *size);
uint32_t FIO_GetFileSize_direct(const char *name);
int64_t FIO_SeekSkipFile(FILE *f, int64_t pos, int whence);
int    FIO_RemoveFile(const char *name);
struct fio_dirent *FIO_FindFirstEx(const char *dir, struct fio_file *file);
int    FIO_FindNextEx(struct fio_dirent *d, struct fio_file *file);
void   FIO_FindClose(struct fio_dirent *d);
struct fio_file *alloc_fio_file(void);
struct file_info convert_fio_file_info(struct fio_file *f);
struct card_info *get_ml_card(void);
struct card_info *get_shooting_card(void);
const char *get_dcim_dir(void);
char  *get_file_prefix(void);

/* ------------------------------------------------------------ memory */
#define MEM_TEMPORARY 2
void  *__mem_malloc(size_t n, unsigned flags, const char *file, unsigned line);
void  *fio_malloc(size_t n);
void   fio_free(void *p);
int    GetFreeMemForMalloc(void);
int    GetFreeMemForAllocateMemory(void);

/* ------------------------------------------------------------- tasks */
extern int ml_shutdown_requested;
void   msleep(int ms);
int    get_ms_clock(void);
int    task_create(const char *name, int prio, int stack, void (*entry)(void *), void *arg);
void   powersave_prohibit(void);
void   powersave_permit(void);

/* ------------------------------------------------------------ screen */
#define FONT_SMALL 1
#define FONT_MED   2
#define FONT(f, fg, bg) (((f) << 16) | ((bg) << 8) | (fg))
#define COLOR_WHITE 0x01
#define COLOR_BLACK 0x02
#define COLOR_CYAN  0x05
int    bmp_printf(uint32_t font, int x, int y, const char *fmt, ...);
void   bmp_fill(uint8_t color, int x, int y, int w, int h);
void   bmp_draw_rect(int color, int x, int y, int w, int h);
void   NotifyBox(int timeout, char *fmt, ...);
#define COLOR_RED   0x08
extern uint32_t LCD_Palette[];
void     EngDrvOut(uint32_t reg, uint32_t value);
uint32_t shamem_read(uint32_t addr);
uint8_t *bmp_vram_real(void);
void     bmp_draw_to_idle(int value);
void     canon_gui_disable_front_buffer(void);
void     canon_gui_enable_front_buffer(int also_redraw);
#ifndef SE700_SIM_DRIVER
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; };
#else
struct tm;
#endif
void     LoadCalendarFromRTC(struct tm *tm);
#define FONT_ALIGN_RIGHT 0x02000000   /* x is the text's right edge */
int    gui_menu_shown(void);
int    display_is_on(void);
int    is_play_mode(void);
void   redraw(void);
extern int lv;
extern int gui_state;                 /* Canon's: 0 is the idle shooting screen */
#define GUISTATE_IDLE 0
int    get_gui_mode(void);            /* Canon's dialog: 0 is none */
int    is_hdr_bracketing_enabled(void);
struct lens_info_sim { unsigned job_state; char name[32]; unsigned focal_len; uint8_t raw_aperture, raw_shutter, raw_iso; };
const char *lens_format_aperture(int raw);
const char *lens_format_shutter(int raw);
const char *lens_format_iso(int raw);
void fake_simple_button(int code);
int  FIO_CreateDirectory(const char *name);
extern struct lens_info_sim lens_info;

/* -------------------------------------------------------------- keys */
/* the 700D's raw button codes, from Magic Lantern's platform/700D.115/gui.h */
#define BGMT_WHEEL_UP          0
#define BGMT_WHEEL_DOWN        1
#define BGMT_PRESS_SET         4
#define BGMT_UNPRESS_SET       5
#define BGMT_MENU              6
#define BGMT_INFO              7
#define BGMT_PLAY              0xB
#define BGMT_TRASH             0xD
#define BGMT_PRESS_ZOOM_IN     0xe
#define BGMT_UNPRESS_ZOOM_IN   0xf
#define BGMT_PRESS_ZOOM_OUT    0x10
#define BGMT_UNPRESS_ZOOM_OUT  0x11
#define BGMT_Q                 0x1D
#define BGMT_LV                0x1E
#define BGMT_PRESS_RIGHT       0x24
#define BGMT_UNPRESS_RIGHT     0x25
#define BGMT_PRESS_LEFT        0x26
#define BGMT_UNPRESS_LEFT      0x27
#define BGMT_PRESS_UP          0x28
#define BGMT_UNPRESS_UP        0x29
#define BGMT_PRESS_DOWN        0x2a
#define BGMT_UNPRESS_DOWN      0x2b
#define BGMT_PRESS_HALFSHUTTER 0x4e
#define GMT_OLC_INFO_CHANGED   0x67
#define BGMT_TOUCH_1_FINGER    0x6b
#define BGMT_UNTOUCH_1_FINGER  0x6c
#define BGMT_TOUCH_2_FINGER    0x72
#define BGMT_UNTOUCH_2_FINGER  0x73
struct event { int type; int param; void *obj; int arg; };
typedef uintptr_t se_ctx_t;           /* a struct event * fits; see ml/700d/ml.h */

#define MODULE_KEY_PRESS_HALFSHUTTER  1
#define MODULE_KEY_UNPRESS_HALFSHUTTER 2
#define MODULE_KEY_PRESS_FULLSHUTTER  3
#define MODULE_KEY_UNPRESS_FULLSHUTTER 4
#define MODULE_KEY_WHEEL_UP           5
#define MODULE_KEY_WHEEL_DOWN         6
#define MODULE_KEY_WHEEL_LEFT         7
#define MODULE_KEY_WHEEL_RIGHT        8
#define MODULE_KEY_PRESS_SET          9
#define MODULE_KEY_UNPRESS_SET       10
#define MODULE_KEY_PRESS_UP          12
#define MODULE_KEY_PRESS_RIGHT       15
#define MODULE_KEY_PRESS_LEFT        16
#define MODULE_KEY_PRESS_DOWN        19
#define MODULE_KEY_UNPRESS_UDLR      20
#define MODULE_KEY_PRESS_ZOOMIN      21
#define MODULE_KEY_MENU              22
#define MODULE_KEY_INFO              23
#define MODULE_KEY_PLAY              24
#define MODULE_KEY_TRASH             25
#define MODULE_KEY_REC               27
#define MODULE_KEY_LV                28
#define MODULE_KEY_Q                 29
#define MODULE_KEY_TOUCH_1_FINGER    35
#define MODULE_KEY_UNTOUCH_1_FINGER  36
#define MODULE_KEY_TOUCH_2_FINGER    37
#define MODULE_KEY_UNTOUCH_2_FINGER  38

/* -------------------------------------------------------------- menu */
struct menu_display_info { char value[36]; char help[80]; char rinfo[36]; int warning_level; };
struct menu_entry {
    const char *name; void *priv; int min, max; const char **choices;
    void (*select)(void *priv, int delta);
    void (*update)(struct menu_entry *e, struct menu_display_info *info);
    unsigned icon_type; const char *help, *help2; int submenu_width;
    struct menu_entry *children;
};
#define MENU_SELECT_FUNC(f) void f(void *priv, int delta)
#define MENU_UPDATE_FUNC(f) void f(struct menu_entry *entry, struct menu_display_info *info)
#define MENU_SET_VALUE(...)   snprintf(info->value, sizeof info->value, __VA_ARGS__)
#define MENU_SET_HELP(...)    snprintf(info->help,  sizeof info->help,  __VA_ARGS__)
#define MENU_SET_RINFO(...)   snprintf(info->rinfo, sizeof info->rinfo, __VA_ARGS__)
#define MENU_SET_WARNING(level, ...) do { info->warning_level = (level); } while (0)
#define MENU_WARN_ADVICE      2
#define MENU_WARN_NOT_WORKING 3
#define MENU_EOL { .priv = (void *)-1 }
#define CHOICES(...) (const char *[]) { __VA_ARGS__ }
#define IT_DICE   2
#define IT_ACTION 5
#define COUNT(x)  ((int)(sizeof(x) / sizeof((x)[0])))
void menu_add(const char *name, struct menu_entry *e, int count);
void menu_remove(const char *name, struct menu_entry *e, int count);
MENU_SELECT_FUNC(menu_open_submenu);

/* ------------------------------------------------- the module tables */
/* The real macros build tables the loader reads. Here they become plain
 * globals the driver can find: the init/deinit pair, the two callbacks,
 * and the config variables (which are ordinary ints either way). */
#define CONFIG_INT(name, var, value) int var = value
#define CBR_SECONDS_CLOCK 5
#define CBR_KEYPRESS      7
#define CBR_KEYPRESS_RAW  8
#define MODULE_INFO_START()        unsigned (*sim_module_init)(void), (*sim_module_deinit)(void); \
                                   static void __attribute__((constructor)) sim_info_ctor(void) {
#define MODULE_INIT(f)             sim_module_init = f;
#define MODULE_DEINIT(f)           sim_module_deinit = f;
#define MODULE_INFO_END()          }
#define MODULE_CBRS_START()        unsigned (*sim_cbr_clock)(unsigned), (*sim_cbr_key)(unsigned), (*sim_cbr_raw)(se_ctx_t); \
                                   static void __attribute__((constructor)) sim_cbrs_ctor(void) {
#define MODULE_CBR(type, f, ctx)   if ((type) == CBR_SECONDS_CLOCK) sim_cbr_clock = (unsigned (*)(unsigned))(f); \
                                   else if ((type) == CBR_KEYPRESS) sim_cbr_key = (unsigned (*)(unsigned))(f); \
                                   else if ((type) == CBR_KEYPRESS_RAW) sim_cbr_raw = (unsigned (*)(se_ctx_t))(f);
#define MODULE_CBRS_END()          }
#define MODULE_CONFIGS_START()     static void __attribute__((unused)) sim_configs_noop(void) {
#define MODULE_CONFIG(v)           (void)v;
#define MODULE_CONFIGS_END()       }

#endif
