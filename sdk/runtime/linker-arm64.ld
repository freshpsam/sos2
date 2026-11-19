/* Static arm64 programs: ELF64 in the image window WEB_IMAGE_BASE to
 * WEB_IMAGE_END (include/user_abi.h), as sdk/runtime/linker.ld for ARM32. */
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
    tls PT_TLS FLAGS(4);
}

SECTIONS
{
    . = 0x1010000000;
    .text : {
        KEEP(*(.text._start))
        *(.text .text.*)
        *(.rodata .rodata.*)
    } :text
    /* Registered at startup (start.c); crtend.o's zero terminator replaced. */
    .eh_frame : {
        __eh_frame_start = .;
        KEEP(*(.eh_frame))
        LONG(0)
    } :text
    .gcc_except_table : { *(.gcc_except_table .gcc_except_table.*) } :text
    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
        *(.got .got.*)
        . = ALIGN(8);
        __preinit_array_start = .;
        KEEP(*(.preinit_array))
        __preinit_array_end = .;
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
        __fini_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array))
        __fini_array_end = .;
    } :data
    .tdata : {
        __tls_start = .;
        *(.tdata .tdata.*)
        __tls_data_end = .;
    } :data :tls
    .tbss (NOLOAD) : {
        *(.tbss .tbss.*)
        __tls_end = .;
    } :data :tls
    __tls_align = MAX(8, MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)));
    . = __tls_end;
    .bss (NOLOAD) : { *(.bss .bss.* COMMON) } :data
    ASSERT(. < 0x1020000000, "Native program exceeds image window")
    /DISCARD/ : { *(.comment .note*) }
}
