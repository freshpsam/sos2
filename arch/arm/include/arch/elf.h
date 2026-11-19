#ifndef SOS2_ARCH_ELF_H
#define SOS2_ARCH_ELF_H
#include <stdint.h>

/* Static ARM32 executables: ELF32, little-endian, EM_ARM. */
#define ELF_IDENT   "\177ELF\1\1\1"
#define ELF_MACHINE 40

struct elf_header {
    unsigned char ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct program_header {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
};

_Static_assert(sizeof(struct elf_header) == 52, "ELF32 header");
_Static_assert(sizeof(struct program_header) == 32, "ELF32 segment");
#endif
