/* The Nexus 5's kernel at the stock bootloader's kernel address (RAM base 0
 * + 0x8000), below its copy of the device tree at 0x02700000 and the ramdisk
 * at 0x02900000. The resources and the page pool go to the second RAM bank,
 * above ARM32 user space (kernel/protection/boot_memory.c). */
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x00008000;
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
        /* and the Wi-Fi driver's: SDIO in PIO, without DMA. */
        *boards/nexus5/wifi.o(.bss .bss.* COMMON)
        *boards/nexus5/bcm4339.o(.bss .bss.* COMMON)
        *boards/nexus5/sdio.o(.bss .bss.* COMMON)
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
    .page_pool (NOLOAD) : ALIGN(16384) {
        __page_pool_start = .;
        . += 8 * 1024 * 1024;
        __page_pool_end = .;
    } :data
    __image_end = .;
    ASSERT(__image_end <= 0x02700000, "Nexus 5 kernel overlaps the bootloader's device tree copy")
    /DISCARD/ : { *(.comment .note* .eh_frame* .ARM.exidx* .ARM.extab*) }
}
