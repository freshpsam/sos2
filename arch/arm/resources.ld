/* Large-memory SOS2 resource layout; independent of the process ABI. */
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x40010000;
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
        /* Cached as well: the scheduler's, page allocator's, network stack's and
         * audio mixer's state, which CPU 0 alone uses and no device reads
         * (virtio-net's and virtio-snd's queues stay uncached). */
        *kernel/protection/page_alloc.o(.bss .bss.* COMMON)
        *kernel/protection/process.o(.bss .bss.* COMMON)
        *kernel/audio/audio.o(.bss .bss.* COMMON)
        *kernel/net/network.o(.bss .bss.* COMMON)
        *kernel/net/network_time.o(.bss .bss.* COMMON)
        *kernel/wifi/*.o(.bss .bss.* COMMON)
        *third_party/lwip/*.o(.bss .bss.* COMMON)
        . = ALIGN(4096);
        __cpu_private_end = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } :data
    .stacks (NOLOAD) : ALIGN(4096) {
        __stack_guard = .; . += 4096;
        . += 65536; __stack_top = .;
        __irq_guard = .; . += 4096;
        . += 4096; __irq_stack_top = .;
        __abort_guard = .; . += 4096;
        . += 4096; __abort_stack_top = .;
        __undef_guard = .; . += 4096;
        . += 4096; __undef_stack_top = .;
        __exception_stack_top = .;
    } :data
    __image_end = .;
    ASSERT(__image_end < 0x48000000, "ARM32 kernel overlaps QEMU's DTB area")
    __page_pool_start = 0x60000000;
    __page_pool_end = 0x88000000;
    /DISCARD/ : { *(.comment .note* .eh_frame* .ARM.exidx* .ARM.extab*) }
}
