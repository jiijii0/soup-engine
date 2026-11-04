/*
 * mlstub.S - the only code placed inside Magic Lantern's own image.
 *
 * It goes over dump_edmac at 0x00980820, which is dead in the shipped 2012
 * binary: a scan of every branch and every literal in the relocated image
 * finds nothing at all pointing into 0x00980820..0x009808F0 from outside it,
 * only the function's own internal branches. Its literal pool runs to
 * 0x0098090C and ML's veneer table of `bx lr` thunks starts at 0x00980910,
 * which is live -- so 236 bytes is the hard budget and the build checks it.
 *
 * Two entry points:
 *
 *   se_loader  the Debug menu item's .priv, launched by ML's own
 *              run_in_separate_task. Reads ML/FILMENG.BIN off the card to
 *              0x009D0000, and only enters it if it starts with 'FILM'.
 *
 *   se_hook    spliced over my_gui_task's call to
 *              handle_common_events_startup. Offers each button to the
 *              payload first; anything the payload does not want goes to
 *              ML's handler exactly as before.
 *
 * The design rule here, learned the hard way: nothing runs at boot and
 * nothing is trusted. se_loaded lives in this stub -- inside ML's image,
 * which is certainly mapped -- and is set only after the magic word has been
 * verified. Until then se_hook never touches 0x009D0000 at all. So a
 * missing, truncated or corrupt payload costs exactly one menu item, and the
 * camera boots and behaves normally either way.
 */
    .syntax unified
    .arm

    .equ FIO_OpenFile,   0xFFAE5B1C
    .equ FIO_ReadFile,   0xFFAE5B54
    .equ FIO_CloseFile,  0xFFAE5B78
    .equ PAYLOAD,        0x009D0000
    .equ PAYLOAD_MAX,    0x00028000      @ 160 KB, the payload's image budget
    .equ FILM_MAGIC,     0x4D4C4946      @ 'FILM'
    .equ ML_STARTUP_EV,  0x0098A0F0      @ handle_common_events_startup
    .equ ML_INIT_TASK,   0x00980440      @ ML's own init task body

    .section .text

/* ------------------------------------------------------------- se_loader */
/* Runs as its own task. Takes no useful arguments. */
    .global se_loader
se_loader:
    push    {r4, r5, lr}
    adr     r0, Lpathstr
    mov     r1, #0                      @ O_RDONLY
    ldr     r4, Lopen
    blx     r4
    cmn     r0, #1                      @ Canon returns -1 on failure
    beq     Lout
    mov     r5, r0                      @ keep the handle
    mov     r1, #PAYLOAD                @ encodable: 0x9D << 16
    mov     r2, #PAYLOAD_MAX            @ encodable: 0x28 << 12
    ldr     r4, Lread
    blx     r4
    mov     r4, r0                      @ bytes read
    mov     r0, r5
    ldr     r5, Lclose
    blx     r5

    mov     r0, #PAYLOAD
    cmp     r4, #0x100                  @ far too short to be the payload
    blt     Lout
    ldr     r1, [r0]
    ldr     r2, Lmagic
    cmp     r1, r2
    bne     Lout

    adr     r1, se_loaded               @ only now is the payload trustworthy
    mov     r2, #1
    str     r2, [r1]
    add     r0, r0, #4                  @ PAYLOAD+0x04 = the task entry
    blx     r0
Lout:
    pop     {r4, r5, pc}

/* --------------------------------------------------------------- se_hook */
/* int se_hook(struct event *ev) -- 0 consumes the event. */
    .global se_hook
se_hook:
    push    {r4, lr}
    mov     r4, r0
    adr     r12, se_loaded
    ldr     r12, [r12]
    cmp     r12, #0
    beq     Lpass
    ldr     r12, Lpaykey                @ PAYLOAD+0x44 = the key handler
    blx     r12
    cmp     r0, #0
    beq     Lconsumed
Lpass:
    mov     r0, r4
    pop     {r4, lr}
    ldr     r12, Lmlev
    bx      r12                         @ tail-call ML's handler
Lconsumed:
    pop     {r4, pc}                    @ r0 is already 0

/* --------------------------------------------------------------- se_boot */
/*
 * The soup engine, started at power-on.
 *
 * Magic Lantern creates its init task with
 *
 *     mov r1, #31 ; mov r2, #8192 ; ldr r3, [0x00980774] ; bl task_create
 *
 * so that literal is a task entry, not an inline call. Pointing it here
 * means our loader runs as its own 8 KB task at boot, and the engine is
 * live before the camera has finished waking up -- no menu, no manual
 * enable.
 *
 * ML's init is then chained, so the camera still comes up exactly as
 * before. That matters for the safety property this stub was built around:
 * if SE.BIN is missing or corrupt, se_loader simply returns and ML boots
 * normally. A bad payload still costs nothing.
 *
 * ML's init runs FIRST, and the payload is loaded after it returns.
 *
 * The other way round does not work: this task is created during Canon's
 * startup, long before the filesystem is up, so se_loader could not open
 * the card and returned empty-handed -- the engine never loaded and the
 * first camera run produced no log at all.
 *
 * ML's init is the right thing to wait on because it does the waiting for
 * us: hijack_gui_main_task spins until GuiMainTask exists, and
 * ml_big_init_task then reads its own config off the card. By the time it
 * returns the card is demonstrably readable, because ML has just read it.
 *
 * It does return: 0x00980440 ends `pop {r4,lr}` then tail-branches to
 * 0x9877D8, which tail-branches to 0xFFB35634 and comes back through lr.
 * That lr is the task-exit address Canon gave us, so it is saved in r6 --
 * callee-saved under AAPCS, and se_loader does not touch it.
 */
    .global se_boot
se_boot:
    mov     r6, lr                  @ the task-exit return, kept safe
    bl      ML_INIT_TASK            @ let ML come all the way up first
    bl      se_loader               @ ...then load: the card is ready now
    mov     lr, r6
    bx      lr

/* ------------------------------------------------------------- the flag */
    .global se_loaded
se_loaded:
    .word   0

Lopen:     .word FIO_OpenFile
Lread:     .word FIO_ReadFile
Lclose:    .word FIO_CloseFile
Lmagic:    .word FILM_MAGIC
Lmlev:     .word ML_STARTUP_EV
Lpaykey:   .word PAYLOAD + 0x44
Lpathstr:  .asciz "A:/SE.BIN"
    .align 2
    .global se_stub_end
se_stub_end:
