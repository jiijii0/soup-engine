/*
 * The soup engine payload on the EOS 50D, firmware 1.0.9.
 *
 * Magic Lantern's 50D.109 build (2025-06-20) links at 0x4B000 and ends its
 * bss at 0xCDE00. copy_and_restart publishes that address as Canon's
 * user_mem_start. patch-film.py moves the published end to 0xFE000, so
 * 0xCE000..0xFE000 is reserved the same way the 5D Classic reserved the
 * top of its heap: Canon will not hand it out.
 *
 * 0xCE000 is 512 bytes above the real bss end. The two stacks sit at the
 * top of the reservation, same sizes as before.
 */
MEMORY
{
    payload (rwx) : ORIGIN = 0x000CE000, LENGTH = 128K
    mstack  (rw)  : ORIGIN = 0x000EE000, LENGTH = 16K
    wstack  (rw)  : ORIGIN = 0x000F2000, LENGTH = 48K
}

ENTRY(_se_header)

SECTIONS
{
    .text : {
        KEEP(*(.header))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > payload

    .data : {
        *(.data*)
        . = ALIGN(4);
    } > payload

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > payload

    __payload_end = .;
    __mstack_top    = ORIGIN(mstack) + LENGTH(mstack);
    __wstack_top    = ORIGIN(wstack) + LENGTH(wstack);
    __wstack_bottom = ORIGIN(wstack);

    /DISCARD/ : { *(.comment) *(.ARM.attributes) *(.note*) }
}
