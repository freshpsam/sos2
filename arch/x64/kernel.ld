/* SOS2's x64 kernel layout, for QEMU's microvm and PCs: linked where it
 * runs, at 64 MiB, which QEMU's PVH loader or SOS2's UEFI loader fills from
 * the ELF's segments (arch/x64/boot.S, boot/uefi/loader.c), clear of
 * what UEFI firmware keeps low down while it runs (OVMF's decompressed
 * firmware volumes reach about 29 MiB) and below what it allocates from
 * the top; independent of the process ABI.
 * The Xen note gives PVH the 32-bit entry, and a PC's kernel links with
 * uefi_start as its ELF entry. Everything is write-back cached memory: x86's
 * DMA is coherent, so .cpu_private needs no section of its own here. */
ENTRY(pvh_start)
PHDRS
{
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
    note PT_NOTE FLAGS(4);
}
SECTIONS
{
    . = 0x4000000;
    __image_start = .;
    .text : {
        KEEP(*(.text.boot))
        *(.text .text.*)
        . = ALIGN(4096);
    } :text
    __text_end = .;
    .note.Xen : { KEEP(*(.note.Xen)) } :rodata :note
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
    .bss (NOLOAD) : ALIGN(4096) {
        __bss_start = .;
        *(.cpu_private)
        *(.bss .bss.* COMMON)
        . = ALIGN(4096);
        __bss_end = .;
    } :data
    /* The boot CPU's stack, above an unmapped guard page. */
    .stacks (NOLOAD) : ALIGN(4096) {
        __stack_guard = .; . += 4096;
        . += 65536; __stack_top = .;
    } :data
    __image_end = .;
    ASSERT(__image_end <= 0x8000000, "x64 kernel larger than 64 MiB")
    /DISCARD/ : { *(.comment .note.GNU-stack .note.gnu.* .eh_frame*) }
}
