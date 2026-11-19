/* SOS2's AArch64 kernel layout, for QEMU and the Galaxy Tab A7 Lite;
 * independent of the process ABI. Both bootloaders load the Image at
 * 0x40080000 (RAM + text_offset). QEMU puts an initrd or the DTB at
 * 0x48000000; the tablet's LK keeps Samsung's debug logs from 0x46C00000. */
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x40080000;
    __image_start = .;
    .text : {
        KEEP(*(.text.boot))
        *(.text .text.*)
        . = ALIGN(4096);
    } :text
    __text_end = .;
    .rodata : {
        *(.rodata .rodata.*)
        . = ALIGN(4096);
    } :rodata
    __rodata_end = .;
    .data : {
        *(.data .data.*)
        BYTE(0)
        . = ALIGN(16);
        __file_end = .;
    } :data
    .bss (NOLOAD) : ALIGN(16384) {
        __bss_start = .;
        __cpu_private_start = .;
        *(.cpu_private)
        . = ALIGN(4096);
        __cpu_private_end = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } :data
    .stacks (NOLOAD) : ALIGN(4096) {
        __stack_guard = .; . += 4096;
        . += 65536; __stack_top = .;
        __exception_stack_top = .;
    } :data
    __image_end = .;
    __image_size = __image_end - __image_start;
    ASSERT(__image_end <= 0x46c00000, "arm64 kernel overlaps the tablet's debug log region")
    /DISCARD/ : { *(.comment .note* .eh_frame*) }
}
