ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
    tls PT_TLS FLAGS(4);
}

SECTIONS
{
    . = 0x11000000;
    .text : {
        KEEP(*(.text._start))
        *(.text .text.*)
        *(.rodata .rodata.*)
    } :text
    .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } :text
    .ARM.exidx : {
        __exidx_start = .;
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        __exidx_end = .;
    } :text
    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
        *(.got .got.*)
        . = ALIGN(4);
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
    ASSERT(. < 0x18000000, "Native program exceeds image window")
    /DISCARD/ : { *(.comment .note*) }
}
