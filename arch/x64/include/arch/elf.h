#ifndef SOS2_ARCH_ELF_H
#define SOS2_ARCH_ELF_H
#include <stdint.h>

/* Static x64 executables: ELF64, little-endian, EM_X86_64. */
#define ELF_IDENT   "\177ELF\2\1\1"
#define ELF_MACHINE 62

struct elf_header {
    unsigned char ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct program_header {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

_Static_assert(sizeof(struct elf_header) == 64, "ELF64 header");
_Static_assert(sizeof(struct program_header) == 56, "ELF64 segment");
#endif
