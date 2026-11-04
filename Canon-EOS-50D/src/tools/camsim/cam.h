/*
 * cam.h (simulator) - the camera's firmware, faked on the desktop.
 *
 * This stands in for ml/payload/cam.h so that ml/payload/film.c -- the real
 * file, not a transcription of it -- can be compiled and run on a machine
 * where it can be debugged. Everything the payload does to the camera goes
 * through this header, so substituting it is enough.
 *
 * The point is not merely to make it run. It is to make it run against an
 * API that behaves as badly as the real one is allowed to:
 *
 *   - reads return less than asked for, which is what silently corrupted
 *     every frame the camera produced until it was found;
 *   - the heap is the 2264 KB the body actually reported free, so an
 *     allocation that would never fit is refused here too;
 *   - the worker stack is a real 48 KB, so an overflow is caught here
 *     rather than on the camera, where it quietly rewrites bss instead;
 *   - the BMP overlay is a real 360x240 buffer behind a real pair of
 *     pointers, drawn into through the same packing bmp_puts uses, so code
 *     that reads its own pixels back gets a truthful answer.
 *
 * Set SE_SIM_SHORT_READS=0 in the environment to turn the first off and
 * confirm a fault really is about short reads.
 */
#ifndef SE_CAM_H
#define SE_CAM_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>

/*
 * The map of ML's image for the active PLATFORM. The build puts
 * ml/platforms/$(PLATFORM) on the include path after this directory, so
 * there is exactly one copy of these numbers and no way for the two sides
 * to drift apart.
 */
#include <mlmap.h>

/* ---- the "addresses" the payload calls through ---------------------- */
long sim_find_first(long dir, long file);
int  sim_find_next(long cookie, long file);
void sim_find_close(long cookie);
int  sim_get_file_size(long path, long out);
void sim_task_create(long name, long prio, long stack, long entry, long arg);
long sim_tick_get(void);

#define CAM_FIO_FindFirstEx   ((uintptr_t)sim_find_first)
#define CAM_FIO_FindNextEx    ((uintptr_t)sim_find_next)
#define CAM_FIO_FindClose     ((uintptr_t)sim_find_close)
#define CAM_FIO_GetFileSize   ((uintptr_t)sim_get_file_size)
#define CAM_task_create       ((uintptr_t)sim_task_create)
#define CAM_tickGet           ((uintptr_t)sim_tick_get)

#define CAM_CALL0(addr, t)                 (((t (*)(void))(addr))())
#define CAM_CALL1(addr, t, a)              (((t (*)(long))(addr))((long)(a)))
#define CAM_CALL2(addr, t, a, b)           (((t (*)(long,long))(addr))((long)(a),(long)(b)))
#define CAM_CALL3(addr, t, a, b, c)        (((t (*)(long,long,long))(addr))((long)(a),(long)(b),(long)(c)))
#define CAM_CALL4(addr, t, a, b, c, d)     (((t (*)(long,long,long,long))(addr))((long)(a),(long)(b),(long)(c),(long)(d)))
#define CAM_CALL5(addr, t, a, b, c, d, e)  (((t (*)(long,long,long,long,long))(addr))((long)(a),(long)(b),(long)(c),(long)(d),(long)(e)))

/* ---- simulated RAM --------------------------------------------------- */
/*
 * The payload reads and writes single words of the camera's address space in
 * three separate places now: ML's relocated image around 0x00980000, ML's
 * bss a little above it, and Canon's own low RAM, where the pointer to the
 * BMP buffer on the panel lives at 0x00029328 and DISPLAY_IS_ON at 0x0000FCA0.
 * A single array based at 0x00980000 cannot hold the last of those -- it
 * would index far below the start of the array and corrupt the heap silently.
 *
 * So MEM() goes through a function that knows which regions exist and calls
 * sim_mem_fault() on anything else. An address the payload has no business
 * touching is a bug worth failing the run for, and failing it here costs
 * nothing compared with finding it on the body.
 *
 * The slots are uintptr_t, not uint32_t: the payload stores real pointers in
 * these words -- the display callback's address, the BMP buffer -- and on
 * this host those are 64-bit. Narrowing them would turn a desktop run into a
 * crash the camera would never see.
 */
#define SIM_LOW_BASE   0x00000000
#define SIM_LOW_SIZE   (256 * 1024)
#define SIM_ML_BASE    0x0004B000
#define SIM_ML_SIZE    (512 * 1024)

#define SIM_REG_BASE  0xC0F14000
#define SIM_REG_SIZE  0x1000
extern uintptr_t sim_reg[SIM_REG_SIZE / 4];
extern uintptr_t sim_low[SIM_LOW_SIZE / 4];
extern uintptr_t sim_ml[SIM_ML_SIZE / 4];
uintptr_t *sim_mem_fault(uintptr_t addr);

static inline uintptr_t *sim_cell(uintptr_t a)
{
    if (a >= SIM_ML_BASE && a < SIM_ML_BASE + SIM_ML_SIZE)
        return &sim_ml[(a - SIM_ML_BASE) >> 2];
    if (a < SIM_LOW_BASE + SIM_LOW_SIZE)
        return &sim_low[(a - SIM_LOW_BASE) >> 2];
    /* the DIGIC register window the palette lives in */
    if (a >= SIM_REG_BASE && a < SIM_REG_BASE + SIM_REG_SIZE)
        return &sim_reg[(a - SIM_REG_BASE) >> 2];
    return sim_mem_fault(a);
}

#define MEM(x) (*sim_cell((uintptr_t)(x)))

/* Canon's imaging pipeline, as the payload addresses it. The simulator has
 * no DIGIC, so camsim seeds a plausible state object at these addresses and
 * the probe's code path runs here exactly as it does on the camera. */
#define CAM_DEVSTATE_PTR   0x00004F28
#define CAM_DEVSTATE_BUF   0x28
#define CAM_DEVBUF_ALIAS   0x00800000
#define CAM_DEVBUF_SIZE    0x0005B000

/*
 * On the camera this masks the uncached alias bit and range-checks against
 * Canon's RAM. Here the only honest equivalent is "is this one of the
 * buffers the simulator actually has", because a made-up camera address is
 * not a host address and following it ends the process -- which is exactly
 * what the first version of this did, and exactly the kind of dereference
 * the camera check exists to prevent.
 */
int sim_imgbuf_ok(uintptr_t p);

static inline int cam_imgbuf_ptr_ok(uintptr_t p)
{
    return (p & 3) == 0 && sim_imgbuf_ok(p);
}

/* Here the tab names are ordinary host strings. */
static inline int cam_mlstr_ok(uintptr_t p) { return p != 0; }

static inline int cam_ram_ptr_ok(uintptr_t p)
{
    return p >= 0x00001000 && p < 0x10000000 && (p & 3) == 0;
}

/* The display spike. There is no panel here, so camsim models the one
 * thing that matters: DISPLAY_IS_ON and the overlay pointer go from zero to
 * valid when this is called. */
#define CAM_PALETTE_BASE  0xC0F14800
#define CAM_PALETTE_LATCH 0xC0F14878
#define CAM_EngDrvOut     ((uintptr_t)sim_engdrvout)
void sim_engdrvout(long reg, long val);
#define CAM_BmpDDev_take  ((uintptr_t)sim_bmpddev_take)
#define CAM_BmpDDev_give  ((uintptr_t)sim_bmpddev_give)
void sim_bmpddev_take(void);
void sim_bmpddev_give(void);
void sim_turn_on_display(void);
#define CAM_TurnOnDisplay ((uintptr_t)sim_turn_on_display)
void  sim_notify_bmp_on(void);
#define CAM_NotifyBmpOn   ((uintptr_t)sim_notify_bmp_on)

/*
 * The bitmap layer. On the camera a frame in the overlay buffer is only
 * scanned out once DispCon's ShowBitmap has enabled the layer, and that
 * call does nothing at all unless the display is already on.
 */
void  sim_show_bitmap(void);
void  sim_hide_bitmap(void);
#define CAM_ShowBitmap    ((uintptr_t)sim_show_bitmap)
#define CAM_HideBitmap    ((uintptr_t)sim_hide_bitmap)
#define CAM_bmp_shown     0x0000FC9D

/* Canon's event poster. ML fakes a MENU press through this to get the
 * panel lit, rather than touching the display hardware itself. */
void  sim_gui_control(long code, long a1, long a2, long a3);
#define CAM_GUI_Control   ((uintptr_t)sim_gui_control)
/* SE_BGMT_MENU / SE_FAKE_BTN / SE_BTN_* come from the platform cam.h via
 * mlmap.h's sibling — the camera header is substituted entirely by this
 * file, so the button map is repeated here for the active body. */
#define SE_BGMT_MENU      5
#define SE_FAKE_BTN       (-123456)

/* platform/50D.109/gui.h — kept in sync with ml/platforms/50D.109/cam.h */
#define SE_BTN_WHEEL_UP     0x00
#define SE_BTN_WHEEL_DOWN   0x01
#define SE_BTN_WHEEL_LEFT   0x02
#define SE_BTN_WHEEL_RIGHT  0x03
#define SE_BTN_SET          0x04
#define SE_BTN_MENU         0x05
#define SE_BTN_INFO         0x06
#define SE_BTN_PLAY         0x08
#define SE_BTN_TRASH        0x09
#define SE_BTN_UNPRESS_UDLR 0x15
#define SE_BTN_UP           0x16
#define SE_BTN_UP_RIGHT     0x17
#define SE_BTN_UP_LEFT      0x18
#define SE_BTN_RIGHT        0x19
#define SE_BTN_LEFT         0x1A
#define SE_BTN_DOWN_RIGHT   0x1B
#define SE_BTN_DOWN_LEFT    0x1C
#define SE_BTN_DOWN         0x1D

/* The simulator stores the panel buffer directly. The camera header turns
 * Canon's raw VRAM pointer into the 720x480 crop; here that pointer already
 * is the buffer. */
static inline uintptr_t cam_overlay_ptr(void)
{
    return (uintptr_t)MEM(CAM_bmp_vram_raw);
}

/* ---- the calls themselves ------------------------------------------- */
void *cam_alloc(size_t n);
void  cam_free(void *p);
void  cam_meminfo(int *total, int *freebytes);
int   cam_open(const char *path);
int   cam_create(const char *path);
int   cam_read(int h, void *buf, size_t n);
int   cam_write(int h, const void *b, size_t n);
void  cam_close(int h);
void  cam_remove(const char *p);
int   cam_seek(int h, int pos, int whence);
void  cam_msleep(int ms);
void  cam_menu_icon(int x, int y, int type, int arg);
void  cam_memcpy(volatile void *dst, const void *src, size_t n);

/* No caches here, so the alias is the address and refreshing is a no-op.
 * The payload still calls both, so the code paths are the same ones the
 * camera runs. */
#define CAM_UNCACHED(p) ((void *)(p))
#define CAM_CACHED(p)   ((void *)(p))
static inline void cam_copy_uncached(void *dst, const void *src, int n)
{
    memcpy(dst, src, (size_t)n);
}

/*
 * On the camera this is `mrs r0, cpsr`. Here it reports the mode VxWorks
 * actually runs its tasks in, so the payload's own check of it is exercised
 * rather than compiled out.
 */
/* The camera uses SWP; here the compiler's own atomic, so the simulator is
 * exercising a real lock rather than pretending the race away. */
static inline int cam_test_and_set(volatile int *lock)
{
    return __sync_lock_test_and_set(lock, 1);
}
static inline void cam_release(volatile int *lock)
{
    __sync_lock_release(lock);
}

#define SIM_CPSR 0x0000001F                /* system mode, interrupts enabled */
static inline uint32_t cam_cpsr(void) { return SIM_CPSR; }

/*
 * The camera header range-checks this against Canon's RAM map. Here the
 * buffers are host allocations, so the only honest answer is "non-null and
 * aligned" -- the range check is a fact about the camera that the simulator
 * is in no position to confirm or deny.
 */
static inline int cam_panel_ptr_ok(uintptr_t p)
{
    return p && !(p & 3);
}

#endif /* SE_CAM_H */
