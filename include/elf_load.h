#ifndef SOS2_ELF_LOAD_H
#define SOS2_ELF_LOAD_H
#include "vm.h"
/* Loads a bounded static ARM ELF32 executable into an otherwise empty space.
 * shared: map the read-only segments from the one shared block (the engine,
 * which every page runs) instead of private copies. */
bool elf_load(struct vm_space *space, const void *image, size_t size, bool shared, uint32_t *entry);
/* Whether elf_load would accept the image, checked without loading it. */
bool elf_valid(const void *image, size_t size);
#endif
