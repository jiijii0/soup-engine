/*
 * mlstub.S - the only code placed inside Magic Lantern's 50D image.
 *
 * Two holes, both padding in the 2025-06-20 50D.109 autoexec.bin:
 *
 *   .loader at 0x000B3A9C, 148 bytes after the NUL of "NO LENS NAME".
 *   The four bytes in front of it stay zero so that string still ends.
 *
 *   .hook   at 0x000B6964, 72 of an 80-byte alignment gap. The pointer
 *   that follows the gap is not touched.
 *
 * se_boot replaces the init-task pointer copy_and_restart writes into
 * the firmware (literal 0x00052FE0, was my_init_task). It runs that
 * function first -- Canon comes up, ML's own init runs -- and only then
 * reads SE.BIN. se_hook is the bl in ml_gui_main_task that used to call
 * handle_common_events_startup; anything the payload does not consume
 * is tail-called onward.
 *
 * se_loaded lives in this stub and is set only after the magic word has
 * been checked. Until then se_hook never touches 0xCE000.
 */
    .syntax unified
    .arm

    .equ FIO_OpenFile,   0x0009CD80      @ veneer -> 0xFF982434
    .equ FIO_ReadFile,   0x0009CC7C      @ veneer -> 0xFF982820
    .equ FIO_CloseFile,  0x0009CC1C      @ veneer -> 0xFF982590
    .equ PAYLOAD,        0x000CE000
    .equ PAYLOAD_MAX,    0x00020000
    .equ FILM_MAGIC,     0x4D4C4946      @ 'FILM'
    .equ ML_STARTUP_EV,  0x0006E834      @ handle_common_events_startup
    .equ ML_INIT_TASK,   0x00052E28      @ my_init_task

    .section .loader, "ax"

    .global se_boot
se_boot:
    push    {r4, lr}
    bl      ML_INIT_TASK
    bl      se_loader
    pop     {r4, lr}
    bx      lr

    .global se_loader
se_loader:
    push    {r4, r5, lr}
    adr     r0, Lpath
    mov     r1, #0
    bl      FIO_OpenFile
    cmn     r0, #1
    beq     Lout
    mov     r5, r0
    mov     r1, #0xCE000
    mov     r2, #0x20000
    bl      FIO_ReadFile
    mov     r4, r0
    mov     r0, r5
    bl      FIO_CloseFile
    mov     r0, #0xCE000
    cmp     r4, #0x100
    blt     Lout
    ldr     r1, [r0]
    ldr     r2, Lmagic
    cmp     r1, r2
    bne     Lout
    adr     r1, se_loaded
    mov     r2, #1
    str     r2, [r1]
    add     r0, r0, #4
    blx     r0
Lout:
    pop     {r4, r5, pc}

    .global se_loaded
se_loaded:
    .word   0
Lmagic: .word FILM_MAGIC
Lpath:  .asciz "A:/SE.BIN"
    .align  2
    .global se_loader_end
se_loader_end:

    .section .hook, "ax"

    .global se_hook
se_hook:
    push    {r4, lr}
    mov     r4, r0
    ldr     r12, Lflag
    ldr     r12, [r12]
    cmp     r12, #0
    beq     Lpass
    ldr     r12, Lpay
    blx     r12
    cmp     r0, #0
    beq     Leat
Lpass:
    mov     r0, r4
    pop     {r4, lr}
    b       ML_STARTUP_EV
Leat:
    pop     {r4, pc}

Lflag:  .word se_loaded
Lpay:   .word PAYLOAD + 0x44
    .global se_hook_end
se_hook_end:
