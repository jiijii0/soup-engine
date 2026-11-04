SECTIONS {
    .loader 0x000B3A9C : { *(.loader) }
    .hook   0x000B6964 : { *(.hook) }
}
