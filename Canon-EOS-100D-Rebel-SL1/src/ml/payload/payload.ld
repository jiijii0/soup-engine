/*
 * The soup engine payload lives at 0x009D0000.
 *
 * Magic Lantern's 5D Classic port patches Canon's heap base from 0xA00000
 * down to 0x980000, reserving 512 KB for itself (platform/5DC.111/init.c).
 * Of that, the relocated AUTOEXEC image occupies 0x980000..0x9B4000, its
 * 64 KB spool buffer sits at 0x9B3C40, and bss runs out to about 0x9CB000 --
 * measured by scanning every literal in the shipped binary that points above
 * the image. Nothing in Magic Lantern or Canon's firmware references
 * anything above that, so 0x9D0000..0xA00000 is ours: 192 KB, with a 20 KB
 * cushion below it in case the bss estimate is a little short.
 */
/*
 * Two stacks, not one. The watcher task develops frames while the GUI task
 * may be drawing the menu, so the two entry paths run concurrently and must
 * not share a stack.
 *
 * The menu stack is no longer just somewhere for bmp_printf to stand. The
 * display callback renders a whole frame of the interface now, so it carries
 * engine/ui.c's call depth -- still modest, since its largest local is a
 * 64-byte string and the frame itself is on the heap. 16 KB covers it with
 * room to spare, and there is a further 53 KB of unused payload region below
 * 0x009F0000 holding nothing live, so even a bad overrun has a long way to
 * go before it reaches anything that matters.
 *
 * The develop path carries the CR2 parser and the JPEG encoder, whose large
 * buffers are arena-allocated but whose frames are not.
 */
/*
 * The worker stack was 24 KB and a single 24,576-byte local in develop_one
 * consumed all of it, writing through into bss and corrupting the very
 * tables it then read. That local is static now, but the margin was far too
 * thin to begin with: the develop path carries the CR2 parser, the band
 * pipeline and the JPEG encoder. 48 KB, with bss ending around 0x9E2000, so
 * there is still a wide gap below.
 */
MEMORY
{
    payload (rwx) : ORIGIN = 0x009D0000, LENGTH = 128K
    mstack  (rw)  : ORIGIN = 0x009F0000, LENGTH = 16K
    wstack  (rw)  : ORIGIN = 0x009F4000, LENGTH = 48K
}

ENTRY(_se_header)

SECTIONS
{
    .text : {
        KEEP(*(.header))        /* the fixed entry table, must be first */
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > payload

    .data : {
        *(.data*)
        . = ALIGN(4);
    } > payload

    /*
     * bss is NOLOAD, so it costs nothing in the blob, and the entry stub
     * zeroes it explicitly. That is not belt-and-braces: this memory is
     * never written between power-on and our first run, so if the stub did
     * not clear it we would be reading whatever the last session left there.
     */
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > payload

    __payload_end = .;
    __mstack_top    = ORIGIN(mstack) + LENGTH(mstack); /* menu callbacks */
    __wstack_top    = ORIGIN(wstack) + LENGTH(wstack); /* watcher task   */
    __wstack_bottom = ORIGIN(wstack);                  /* canary lives here */

    /DISCARD/ : { *(.comment) *(.ARM.attributes) *(.note*) }
}
