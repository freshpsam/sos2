/* SOS2's UEFI loader as a position-independent ELF, which tools/elf2efi.py
 * turns into a PE32+ EFI application: its sections on page boundaries from
 * 4 KiB, where the PE headers end, and only relative relocations, which
 * become the PE's base relocations. */
ENTRY(efi_main)
SECTIONS
{
    . = 0x1000;
    .text : { *(.text .text.*) }
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(4096);
    .data : { *(.data .data.*) *(.got .got.*) }
    .bss : { *(.bss .bss.* COMMON) }
    . = ALIGN(4096);
    .dynamic : { *(.dynamic) }
    .rela.dyn : { *(.rela .rela.*) }
    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }
    .hash : { *(.hash .gnu.hash) }
    /DISCARD/ : { *(.comment .note* .eh_frame* .interp) }
}
