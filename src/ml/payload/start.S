/*
 * start.S - the payload's fixed entry table and startup stub.
 *
 * Two addresses outside this image are patched to call into it -- the init
 * task entry and the GUI event hook -- and neither must move when the
 * payload is recompiled. So everything the outside world needs lives in a
 * table at a fixed offset from the payload base (see the active platform's
 * payload.ld), and the patch tool reads the table out of the built blob
 * rather than guessing at symbol addresses.
 *
 * Several slots are dead now that Magic Lantern's menu is not used at all.
 * They are zeros rather than compacted: the offsets are a layout the patch
 * tool and the simulator both agree on, and renumbering them to save twelve
 * bytes is a way to get something subtly wrong.
 *
 * Offsets (from the payload base, 0x000CE000 on the 50D):
 *   0x00  magic 'FILM'
 *   0x04  b se_boot_start -- THE ENTRY POINT. mlstub's loader finishes with
 *         `add r0, r0, #4 ; blx r0`, so whatever is here is what runs after
 *         the payload is read off the card. Zeroing it, which a previous
 *         commit did on the grounds that it "was the menu select target",
 *         sent the init task through three nops into the string pointer at
 *         +0x10 and the camera came up with Err and no working buttons.
 *   0x08  unused -- was the menu `display` callback
 *   0x0C  unused -- was the menu `select` callback
 *   0x10  &str_stock_name
 *   0x14  &str_stock_help
 *   0x18  &str_dev_name
 *   0x1C  &str_dev_help
 *   0x20  &se_stock_index
 *   0x24  &se_res_index
 *   0x28  payload image size, filled in by the patch tool
 *   0x2C  &str_res_name
 *   0x30  &str_res_help
 *   0x34  unused -- was a second boot entry, never reached: the init-task
 *         literal points at mlstub's se_boot, which chains ML's own init
 *         and then calls the loader, which enters at +0x04
 *   0x38  &str_auto_name
 *   0x3C  &str_auto_help
 *   0x40  &se_auto_on
 *   0x44  b se_boot_key   -- offered every button by the gui hook
 *   0x48  unused -- was the menu page renderer detour
 */
    .syntax unified
    .arm

    .section .header, "ax"
    .global _se_header
_se_header:
    .word   0x4D4C4946              @ 'FILM'
    b       se_boot_start           @ +0x04 the entry point -- see above
    .word   0                       @ +0x08 (was the display callback)
    .word   0                       @ +0x0C (was the menu select callback)
    .word   se_str_stock_name       @ +0x10
    .word   se_str_stock_help       @ +0x14
    .word   se_str_dev_name         @ +0x18
    .word   se_str_dev_help         @ +0x1C
    .word   se_stock_index          @ +0x20
    .word   se_res_index            @ +0x24
    .word   0                       @ +0x28 image size (patched in)
    .word   se_str_res_name         @ +0x2C
    .word   se_str_res_help         @ +0x30
    .word   0                       @ +0x34 (was a second boot entry)
    .word   se_str_auto_name        @ +0x38
    .word   se_str_auto_help        @ +0x3C
    .word   se_auto_on              @ +0x40
    b       se_boot_key             @ +0x44
    .word   0                       @ +0x48 (was the renderer detour)

/*
 * Every entry point runs on whichever Canon task called us, and those task
 * stacks are small -- a few KB. The engine's working set is all arena
 * allocated, but the JPEG encoder and CR2 header parser still want a few
 * hundred bytes of frame each, and bmp_printf is not free either. So each
 * entry switches to the payload's own 32 KB stack and restores the caller's
 * on the way out. Switching sp inside a task is safe: the scheduler saves
 * and restores sp per task, and we always put it back.
 */
/*
 * SE_ENTER is for TASK BODIES ONLY -- se_boot_watch and se_boot_screen,
 * each of which owns its stack for the life of the camera:
 *
 *     __wstack_top   the watcher    0x009F4000..0x00A00000   48 KB
 *     __mstack_top   the screen     0x009F0000..0x009F4000   16 KB
 *
 * Anything that is not a task body runs on whichever Canon task called it
 * and must leave sp alone, because every one of these stacks already
 * belongs to something. Two contexts on one stack is not a subtle failure:
 * the second one's pushes land on the first one's return address.
 */
.macro SE_ENTER top
    push    {r4, r5, lr}
    mov     r5, sp
    ldr     r4, =\top
    mov     sp, r4
    push    {r5}
.endm

.macro SE_LEAVE
    pop     {r5}
    mov     sp, r5
    pop     {r4, r5, lr}
    bx      lr
.endm

    .text

/*
 * void se_boot_start(void)
 *
 * The payload's entry point, reached by mlstub's loader the moment SE.BIN
 * has been read and its magic checked.
 *
 * It does NOT switch stacks, and must not. It runs on the init task and
 * creates the two tasks below, and those tasks own the two stacks -- the
 * screen owns mstack from the moment it is created. An earlier version of
 * this switched to __mstack_top and then created a task that runs on
 * __mstack_top: the new task's pushes landed on the saved sp and lr this
 * function was going to return through, SE_LEAVE popped whatever was
 * there, and the init task jumped into nothing. The camera shot photographs
 * and answered no button at all, because ML's startup never finished.
 *
 * The function this replaced carried that warning in its own comment. It
 * needs almost no stack -- two task_create calls -- so it uses the init
 * task's.
 */
    .global se_boot_start
se_boot_start:
    push    {r0, lr}
    bl      se_zero_bss_once
    bl      se_start
    pop     {r0, lr}
    bx      lr

/*
 * The screen task body. Created alongside the watcher, so it owns the menu
 * stack -- free now that nothing of ours runs inside ML's menu redraw --
 * for the life of the camera.
 */
    .global se_boot_screen
se_boot_screen:
    SE_ENTER __mstack_top
    bl      se_zero_bss_once
    bl      se_screen
    SE_LEAVE

/*
 * int se_boot_key(struct event *ev)
 *
 * Reached from the gui hook, so it runs on Canon's GUI task while the film
 * engine's own task may be drawing. It deliberately does NOT switch stacks:
 * it only updates an index and raises a redraw flag, which costs a handful
 * of words, and the worker stack belongs to the task that is drawing.
 */
    .global se_boot_key
se_boot_key:
    push    {r0, lr}
    bl      se_zero_bss_once
    pop     {r0, lr}
    b       se_key

/*
 * The watcher task body. Created by se_start_watcher, so it owns the watcher
 * stack for the life of the camera and never overlaps with a menu callback.
 */
    .global se_boot_watch
se_boot_watch:
    SE_ENTER __wstack_top
    bl      se_zero_bss_once
    bl      se_watch
    SE_LEAVE

/*
 * Clear bss on the first call through any entry point.
 *
 * cstart copies only the loaded part of the image, so bss arrives holding
 * whatever was in RAM at power-on. There is no single "init" moment we are
 * guaranteed to be called at -- the menu may draw us before anything else
 * touches us -- so the guard lives in .data, which *is* copied and therefore
 * starts at a known value.
 */
    .global se_zero_bss_once
se_zero_bss_once:
    push    {r0, r1, r2, r3, r12, lr}   @ r0-r3 are the caller's arguments
    ldr     r3, =se_bss_ready
    ldr     r2, [r3]
    cmp     r2, #0
    bne     2f
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r2, #0
1:  cmp     r0, r1
    strlo   r2, [r0], #4
    blo     1b
    mov     r2, #1
    str     r2, [r3]
2:  pop     {r0, r1, r2, r3, r12, lr}
    bx      lr

    .data
    .global se_bss_ready
se_bss_ready:
    .word   0
