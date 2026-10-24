/*
 * cam.h - the EOS 50D firmware 1.0.9, as seen from the payload.
 *
 * The payload is a freestanding blob: it does not link against Magic Lantern
 * and does not use its stubs. It calls Canon's own ROM functions by absolute
 * address instead, which is why it can be rebuilt and re-patched without
 * rebuilding Magic Lantern at all.
 *
 * Every address below was obtained twice, independently:
 *
 *   1. by disassembling the shipped 2012 AUTOEXEC.BIN -- dump_with_buffer at
 *      0x00980518 calls FIO_RemoveFile, FIO_CreateFile, FIO_WriteFile,
 *      FIO_CloseFile and memcpy in that order, and the Free Memory menu item
 *      at 0x00987630 calls GetMemoryInformation and bmp_printf;
 *   2. from the port's own independently-derived stubs.S.
 *
 * The two agree on every address they have in common. Addresses marked
 * "ML" live in the relocated Magic Lantern image at 0x00980000 and are only
 * valid with the 2012 binary this payload is patched into; addresses marked
 * "ROM" are firmware 1.1.1 and are fixed.
 */
#ifndef SE_CAM_H
#define SE_CAM_H

#include <stdint.h>
#include <stddef.h>

/*
 * ROM entry points. Each one is a veneer in the pinned 50D.109 autoexec
 * (the `ldr pc` table around 0x9CA00) and the same address in
 * platform/50D.109/stubs.S. The two lists agree; the veneer is the one
 * this binary actually calls.
 */
/* ------------------------------------------------------------- ROM: memory */
#define CAM_AllocateMemory       0xFF867B54
#define CAM_FreeMemory           0xFF867D18
#define CAM_GetMemoryInformation 0xFF8680C8

/* --------------------------------------------------------------- ROM: file */
#define CAM_FIO_OpenFile        0xFF982434
#define CAM_FIO_CreateFile      0xFF9824E8
#define CAM_FIO_RemoveFile      0xFF982704
#define CAM_FIO_ReadFile        0xFF982820
#define CAM_FIO_SeekSkipFile    0xFF9828D0
#define CAM_FIO_WriteFile       0xFF9829D0
#define CAM_FIO_CloseFile       0xFF982590
#define CAM_FIO_GetFileSize     0xFF982B84
/*
 * The Find family, read out of the ROM rather than taken from the port's
 * stubs.S, which has FindNextEx wrong: the address it gives (0xFFB4720C) is
 * a three-instruction getter that returns a global and never touches the
 * directory, so a walk using it spins forever on the first entry.
 *
 * 0xFFAE5F8C  FindFirstEx(dir, file)  opendir, readdir, then malloc(0x104)
 *                                     for a cookie holding the dir handle at
 *                                     +0x100. Returns the cookie, or the
 *                                     small integers 1 or 5 on failure.
 * 0xFFAE6018  FindNextEx(cookie, file)  readdir into file; 0 on success.
 * 0xFFAE6070  FindClose(cookie)         closedir and free.
 */
#define CAM_FIO_FindFirstEx     0xFF983474
#define CAM_FIO_FindNextEx      0xFF9835A0
#define CAM_FIO_FindClose       0xFF9836A0

/* ------------------------------------------------- Canon's imaging pipeline */
/*
 * The seam between DIGIC's develop and the hardware JPEG encoder.
 *
 * Canon names all of this in its own debug strings: ../Develop/Develop.c,
 * ../JpCore.c, ShootDevelop, JpCoreIntrHandler, "[SHT]->sbsDevelopDone" and
 *
 *     "[SHT]->pDevelopBufferAddr%#lx(%#lx)"
 *
 * which is printed at 0xFF898B00 from two values filled in by a small getter
 * at 0xFF8B890C:
 *
 *     ldr ip, [0x00004F28]      the imaging state object
 *     ldr r3, [ip, #0x28]       the develop buffer
 *     mov r2, #0x5B000          its size, 372,736 bytes
 *     add r3, r3, #0x800000     an address alias of some kind
 *
 * The state object is referenced from 42 places across the Develop and Shoot
 * code, so it is the real thing rather than a one-off local.
 *
 * NONE OF THIS IS DEREFERENCED YET. 372,736 bytes is not a 2496x1664 frame,
 * so either the develop is banded or this is one buffer among several, and
 * the +0x800000 is an aliasing scheme we do not understand -- we have just
 * spent three camera trips on a different alias that returned the right
 * byte count and the wrong bytes. The addresses get logged and nothing else
 * until a dump says what they point at.
 */
#define CAM_DEVSTATE_PTR   0x00004F28   /* -> the imaging state object      */
#define CAM_DEVSTATE_BUF   0x28         /* its develop buffer field         */
#define CAM_DEVBUF_ALIAS   0x00800000   /* what the getter adds to it       */
#define CAM_DEVBUF_SIZE    0x0005B000   /* what the getter reports          */
#define CAM_getDevelopBuffer 0xFF8B890C /* the getter itself, for later     */

/*
 * Is this plausibly a pointer into Canon's RAM?
 *
 * Checked before the state object is dereferenced at all. A null or stale
 * value here is entirely possible -- nothing says that object exists before
 * the first capture -- and following it would fault and take the task with
 * it, which is the one way a read-only probe can still cost something.
 */
/* DryOS alias. Live-view buffers in the 50D consts are 0x41xxxxxx. */
#define CAM_UNCACHED_BIT  0x40000000u
#define CAM_UNCACHED(p)   ((void *)((uintptr_t)(p) | CAM_UNCACHED_BIT))
#define CAM_CACHED(p)     ((void *)((uintptr_t)(p) & ~(uintptr_t)CAM_UNCACHED_BIT))

/* Is this a pointer to a string inside ML's image? Camera-specific, so the
 * simulator -- whose strings are host pointers -- answers differently. */
static inline int cam_mlstr_ok(uintptr_t p)
{
    return p >= 0x0004B000 && p < 0x000FE000;
}

static inline int cam_ram_ptr_ok(uintptr_t p)
{
    uintptr_t a = p & ~(uintptr_t)CAM_UNCACHED_BIT;
    return a >= 0x00001000 && a < 0x20000000 && (p & 3) == 0;
}

/* ----------------------------------------------------------- ROM: display */
/*
 * Turning the panel on without Magic Lantern's menu.
 *
 * The interface currently reaches the screen only because ML's menu redraw
 * lights the panel and then blits our frame for us. With ML gone, both
 * become ours. MEM(0x0000FCA0) reads zero whenever the panel is dark, and
 * MEM(0x00029328) -- the overlay buffer pointer -- reads zero with it.
 *
 * ML's own code calls this with no arguments and only when the display is
 * off, in two places (debug.c and tweaks.c):
 *
 *     if (!DISPLAY_IS_ON) call("TurnOnDisplay");
 *
 * Shipped, working code on this body, which is the best evidence available
 * that it is safe to call. We copy the guard as well as the call.
 */
/*
 * The overlay palette.
 *
 * Our frame is raw 4-bit indices, and the table those index is programmed
 * by Magic Lantern's menu, not by Canon -- set_ml_palette() writes sixteen
 * words to the DIGIC palette registers and then latches them. With ML's
 * menu shut nobody writes it, so the indices mean whatever Canon last left
 * there, and an index that lands on a transparent entry draws nothing at
 * all. That is the likeliest reason a successful blit to a lit panel was
 * invisible.
 *
 * The values are ML's own, for this body, as 0xBBGGRRAA-derived DIGIC
 * words. Writing them is writing hardware registers, which is why it is a
 * step of its own: the one previous excursion near DIGIC registers threw an
 * Err. These are display registers rather than anything in the capture
 * path, and ML writes them on every menu redraw, so the exposure is far
 * smaller -- but it is still its own experiment.
 */
#define CAM_PALETTE_BASE    0xC0F14080   /* 16 entries, one word each */
#define CAM_PALETTE_LATCH   0xC0F14078   /* write 1 to apply          */

/*
 * NotifyTurnOnDisplayToBitmapDisplayDevice. Recorded, deliberately NOT
 * called.
 *
 * Canon's own debug string at 0xFFB0AC38 names it:
 *   "  [BmpDDev] NotifyTurnOnDisplayToBitmapDisplayDevice (PRI)"
 *
 * It takes an argument and writes a flag at 0x0000FE38 -- 1 when the
 * argument is zero, 0 otherwise -- then calls 0xFFB0A95C. Nothing in the
 * whole 8 MB ROM branches to it, so there is no caller to learn the
 * argument's meaning from, and it is marked PRI where TurnOnDisplay is
 * PUB.
 *
 * It was wanted for a reason that turned out to be wrong: the overlay was
 * invisible because the panel was off, not because the bitmap device
 * needed telling. Left here because knowing what it is and why it is not
 * used is worth more than the three lines it costs.
 */
#define CAM_NotifyBmpOn     0xFFB0AC74   /* not called -- see above */

/*
 * GUI_Control -- how Magic Lantern lights this screen, and it never
 * touches the display hardware to do it.
 *
 * ML's menu_open() for CONFIG_5DC is, in full:
 *
 *     if (!DISPLAY_IS_ON) fake_simple_button(BGMT_MENU);
 *     msleep(50);
 *
 * and fake_simple_button is GUI_Control(code, 0, FAKE_BTN, 0). It asks
 * Canon to open its own menu, and Canon then does the whole sequence --
 * powers the panel, shows the bitmap layer, sets its GUI state -- in the
 * order Canon intends. ML draws over the top. piggyback_canon_menu() and
 * canon_gui_disable_front_buffer() are both compiled out on this body, so
 * that really is the entire strategy, and it has worked for a decade.
 *
 * 0xFF8190D0 shuffles its arguments and tail-calls 0xFF817500:
 *     mov ip, r1 ; mov r3, r2 ; mov r1, r0 ; mov r2, ip ; mov r0, #0
 */
#define CAM_GUI_Control     0xFF822AB4
#define CAM_EngDrvOut       0xFF97D794
/* BGMT_MENU from platform/50D.109/gui.h. FAKE_BTN is ML's own sentinel. */
#define SE_BGMT_MENU        5
#define SE_FAKE_BTN         (-123456)

/* Button codes as event->param reaches the GUI task (platform/50D.109/gui.h).
 * Both dials are accepted: the top dial is WHEEL_UP/DOWN and the rear dial
 * is WHEEL_LEFT/RIGHT. */
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

/*
 * ShowBitmap / HideBitmap -- '[DispCon] ShowBitmap (PUB)' at 0xFFB097CC.
 *
 * This is why a frame in the overlay buffer was invisible. The bitmap
 * layer is hidden, and Canon shows it when it opens a menu of its own:
 *
 *     ffb09814  ldr r3, [pc, #36]   ; -> 0x0000FCA0   display_is_on
 *     ffb09818  ldr r2, [r3]
 *     ffb0981c  cmp r2, #1
 *     ffb09820  bne 0xffb09834      ; display off: does nothing at all
 *     ffb09824  ldr r3, [pc, #24]   ; -> 0x0000FA74
 *     ffb09828  ldr r2, [r3, #236]
 *     ffb0982c  mvn r3, #21         ; 0xFFFFFFEA
 *     ffb09830  str r3, [r2]        ; and the layer is on
 *
 * Neither takes an argument, and ShowBitmap only acts when the display is
 * already on -- so TurnOnDisplay has to come first.
 *
 * HideBitmap is the exact mirror: 21 where Show writes ~21. Both also
 * write that value as a byte to 0x0000FC9D, which is therefore the layer's
 * current state and can be read before touching anything.
 *
 * NEITHER IS CALLED. They are what the overlay needed and they are not how
 * Magic Lantern gets it: ML asks Canon for a menu and lets Canon do all of
 * this in its own order. A build that called TurnOnDisplay and ShowBitmap
 * directly did put the interface on screen -- and then froze the camera.
 * Kept because they explain WHY the overlay was invisible, which is worth
 * more than the three lines.
 */
#define CAM_ShowBitmap      0xFFB097E8
#define CAM_HideBitmap      0xFFB09864
#define CAM_bmp_shown       0x0000FC9D   /* 0xEA shown, 0x15 hidden */

#define CAM_TurnOnDisplay   0xFFB091D0
#define CAM_BmpDDev_take    0xFFB09E84
#define CAM_BmpDDev_give    0xFFB09EA8

/* --------------------------------------------------------------- ROM: misc */
#define CAM_msleep              0xFF866E08
#define CAM_task_create         0xFF866EBC   /* (name, prio, stack, entry, arg) */
/* This build's millisecond clock. Not a ROM tick; the 50D stubs have none. */
#define CAM_tickGet             0x00078B18

/* ----------------------------------------------------------------- ML code */
#define ML_bmp_printf           0x0098110C   /* (fontspec, x, y, fmt, ...) */
#define ML_menu_draw_icon       0x0098479C   /* (x, y, type, arg)          */

/*
 * The font/colour word bmp_printf takes as its first argument. 0x00030201 is
 * the value the Free Memory item uses: palette index 1 (white) on index 2
 * (black), font 3. Taking it verbatim from working code avoids having to
 * reconstruct Magic Lantern's FONT() macro for this vintage.
 */
#define SE_FONT_PLAIN   0x00030201

/*
 * A word of the camera's address space. Defined here rather than where it is
 * used so the desktop simulator can substitute an ordinary array and run the
 * real payload code without writing to address 0x009B1720.
 */
#define MEM(x) (*(volatile uint32_t *)(uintptr_t)(x))

typedef int (*se_fn)(void);

#define CAM_CALL0(addr, t)                 (((t (*)(void))(addr))())
#define CAM_CALL1(addr, t, a)              (((t (*)(long))(addr))((long)(a)))
#define CAM_CALL2(addr, t, a, b)           (((t (*)(long,long))(addr))((long)(a),(long)(b)))
#define CAM_CALL3(addr, t, a, b, c)        (((t (*)(long,long,long))(addr))((long)(a),(long)(b),(long)(c)))
#define CAM_CALL4(addr, t, a, b, c, d)     (((t (*)(long,long,long,long))(addr))((long)(a),(long)(b),(long)(c),(long)(d)))
#define CAM_CALL5(addr, t, a, b, c, d, e)  (((t (*)(long,long,long,long,long))(addr))((long)(a),(long)(b),(long)(c),(long)(d),(long)(e)))

/* ------------------------------------------------------------------ memory */
static inline void *cam_alloc(size_t n)  { return CAM_CALL1(CAM_AllocateMemory, void *, n); }
static inline void  cam_free(void *p)    { CAM_CALL1(CAM_FreeMemory, void, p); }

/* Fills total and free in bytes. */
static inline void cam_meminfo(int *total, int *freebytes)
{
    CAM_CALL2(CAM_GetMemoryInformation, void, total, freebytes);
}

/* --------------------------------------------------------------------- I/O */
#define SE_O_RDONLY 0

/* Canon returns -1 for a failed open; everything else is a handle. */
static inline int cam_open(const char *path)
{
    return CAM_CALL2(CAM_FIO_OpenFile, int, path, SE_O_RDONLY);
}
static inline int cam_create(const char *path)
{
    return CAM_CALL1(CAM_FIO_CreateFile, int, path);
}
static inline int  cam_read (int h, void *buf, size_t n) { return CAM_CALL3(CAM_FIO_ReadFile,  int, h, buf, n); }
static inline int  cam_write(int h, const void *b, size_t n) { return CAM_CALL3(CAM_FIO_WriteFile, int, h, b, n); }
static inline void cam_close(int h) { CAM_CALL1(CAM_FIO_CloseFile, void, h); }
static inline void cam_remove(const char *p) { CAM_CALL1(CAM_FIO_RemoveFile, void, p); }
/* FIO_SeekSkipFile takes the offset as int64. A three-register call would
 * put `whence` in the high half of the offset. */
static inline int cam_seek(int h, int pos, int whence)
{
    typedef int64_t (*seek_fn)(int, int64_t, int);
    int64_t r = ((seek_fn)CAM_FIO_SeekSkipFile)(h, (int64_t)pos, whence);
    return (r < 0) ? -1 : 0;
}

static inline void cam_msleep(int ms) { CAM_CALL1(CAM_msleep, void, ms); }

/* ------------------------------------------------------- cache coherency */
/*
 * The uncached alias of a RAM address.
 *
 * ARM946E-S has a write-back data cache and no coherency with DMA, and
 * Canon's FIO reads by DMA. So a buffer we have touched sits in the cache,
 * FIO writes the data to main memory behind it, and reading the buffer
 * afterwards returns whatever the cache still holds. The read reports the
 * full count and no error, because from FIO's point of view nothing went
 * wrong -- the bytes are in memory. They are just not the bytes we see.
 *
 * That is what ruined the frames: the picture is correct until the first
 * cache line that survived from the previous refill, and from that byte on
 * the entropy decoder is reading someone else's bits.
 *
 * 0x40000000 is the DryOS alias bit. Magic Lantern's mem.h ORs it on under
 * every camera that is not VxWorks, and the 50D live-view buffers are
 * published at 0x41xxxxxx, which is that bit set.
 *
 * Magic Lantern wraps its own FIO buffers in UNCACHEABLE(), so this is not
 * a theory about the hardware; it is what the port already does.
 */
/*
 * Copy out of a DMA-written buffer, reading it only through the uncached
 * alias.
 *
 * The DMA buffer is never written through its cached address. It is read
 * only through the alias, and the bytes are delivered into the caller's
 * buffer, which DMA never touches.
 */
static inline void cam_copy_uncached(void *dst, const void *src, int n)
{
    volatile const uint8_t *u = (volatile const uint8_t *)CAM_UNCACHED(src);
    uint8_t *d = (uint8_t *)dst;
    int i = 0;

    /* words while both sides are aligned; four times fewer bus reads */
    if (((((uintptr_t)d) | ((uintptr_t)u)) & 3) == 0) {
        volatile const uint32_t *uw = (volatile const uint32_t *)(const void *)u;
        uint32_t *dw = (uint32_t *)dst;
        int w = n >> 2;
        for (; i < w; i++) dw[i] = uw[i];
        i <<= 2;
    }
    for (; i < n; i++) d[i] = u[i];
}

/*
 * The same check for an address that may carry the uncached alias bit.
 *
 * The develop buffer came back as 0x10A00000, which is 0x00A00000 seen
 * through the 0x10000000 alias -- exactly where Magic Lantern's reserved
 * region ends, so precisely where Canon's imaging buffers would be expected
 * to begin. A plain range check rejects it for being too high, which is the
 * mistake cam_panel_ptr_ok already made once.
 */
static inline int cam_imgbuf_ptr_ok(uintptr_t p)
{
    uintptr_t a = p & ~(uintptr_t)CAM_UNCACHED_BIT;
    return a >= 0x00001000 && a < 0x20000000 && (p & 3) == 0;
}

/* A local copy. The 50D build reaches Canon's memcpy through a veneer we
 * do not need: the overlay is written through the pointer Canon published,
 * which is already the uncached view when DMA owns it. */
static inline void cam_memcpy(volatile void *dst, const void *src, size_t n)
{
    volatile uint8_t *d = (volatile uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    size_t i;
    for (i = 0; i < n; i++) d[i] = s[i];
}

/* ML's image: the overlay, the fonts and the Debug entry list. */
#include "mlmap.h"

/*
 * bmp_vram_real(), transcribed from this autoexec.
 *
 * It loads bmp_vram_info[1].vram2 and, when the low 12 bits are one of the
 * shapes BMP_VRAM_START knows, returns the 720x480 crop (inset 0x70F8 into
 * the 960-wide buffer). An unrecognised pointer is returned unchanged so a
 * bad guess is not added on top of it.
 */
static inline uintptr_t cam_overlay_ptr(void)
{
    uintptr_t raw = (uintptr_t)MEM(CAM_bmp_vram_raw);
    uintptr_t low = raw & 0xFFFu;
    uintptr_t base = raw;

    if (low == 0x100u)      base = raw - 0x70F8u;
    else if (low == 0x008u) base = raw;
    else if (low == 0x108u) base = raw - 0x7100u;
    else if (low == 0x4C8u) base = raw - 0x74C0u;
    else if (low == 0x4C0u) base = raw - 0x74B8u;
    else if (low == 0xC28u) base = raw - 0x7C20u;
    else                    return raw;
    return base + 0x70F8u;
}

/* ------------------------------------------------------------------ screen */
/*
 * bmp_printf is variadic, so it has to be declared rather than wrapped in a
 * fixed-arity macro: a variadic call has a different ABI contract than a
 * same-arity fixed call once arguments reach the stack.
 */
typedef void (*cam_printf_fn)(uint32_t fontspec, int x, int y, const char *fmt, ...);
#define cam_printf (*(cam_printf_fn)ML_bmp_printf)

static inline void cam_menu_icon(int x, int y, int type, int arg)
{
    CAM_CALL4(ML_menu_draw_icon, void, x, y, type, arg);
}

/*
 * The processor mode we are running in.
 *
 * Worth a line of the log on its own account: the last session's theory for
 * why a task died was that CP15 writes from a Canon task raise an undefined
 * instruction because the task runs unprivileged. MRS is readable in every
 * mode, so this settles it without guessing.
 */
/*
 * Test and set, for the one place two tasks of ours touch the same thing.
 *
 * ARM946E-S is ARMv5TE and has SWP, which is exactly this and is atomic
 * against other masters on the bus. A plain flag was not enough: the window
 * between reading it and writing it is small but a preempting task lands in
 * it often enough to matter, and the symptom -- a log file truncated by one
 * task while another was writing it -- destroys the only diagnostic channel
 * the camera has.
 *
 * Returns the previous value: zero means the caller now holds it.
 */
static inline int cam_test_and_set(volatile int *lock)
{
    int v = 1;
#ifdef __arm__
    __asm__ __volatile__("swp %0, %0, [%1]" : "+r"(v) : "r"(lock) : "memory");
#else
    (void)lock;
#endif
    return v;
}

static inline void cam_release(volatile int *lock)
{
    __asm__ __volatile__("" ::: "memory");
    *lock = 0;
}

static inline uint32_t cam_cpsr(void)
{
#ifdef __arm__
    uint32_t r;
    __asm__ __volatile__("mrs %0, cpsr" : "=r"(r));
    return r;
#else
    return 0;
#endif
}

/*
 * Is the word at 0x00029328 plausibly the overlay buffer?
 *
 * We are about to memcpy 86,400 bytes through it, so it is worth a look
 * first. The address itself is as trustworthy as Magic Lantern's own menu --
 * bmp_idle_copy memcpys exactly this many bytes to exactly this pointer on
 * every redraw, in shipped code that works on this body -- but a null or a
 * stale value while the panel is down is a real possibility, and bmp_puts
 * itself refuses an odd one.
 *
 * Canon's RAM on this body is the low 32 MB. Rejecting anything above that,
 * and anything inside Magic Lantern's region or our own, means a wild value
 * costs a line in the log instead of the camera.
 */
static inline int cam_panel_ptr_ok(uintptr_t p)
{
    /*
     * Canon hands this one out through the uncached alias -- the overlay is
     * DMA-written, so it must. The camera logged 0x1058919C and an earlier
     * version of this check called that nonsense and refused it; it is
     * 0x0058919C with the alias bit set. Range-check the real address.
     */
    uintptr_t a = (uintptr_t)CAM_CACHED((void *)p);
    if (!a || (a & 3)) return 0;
    if (a < 0x1000 || a >= 0x20000000) return 0;
    if (a >= 0x0004B000 && a < 0x000FE000) return 0;   /* ML's and ours */
    return 1;
}

#endif /* SE_CAM_H */
