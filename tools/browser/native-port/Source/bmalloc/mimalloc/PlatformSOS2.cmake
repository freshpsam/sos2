# newlib's <stdatomic.h> defines ATOMIC_VAR_INIT for struct-wrapped atomics,
# which breaks mimalloc's C11 initializers; under C17 mimalloc uses plain ones.
add_compile_options($<$<COMPILE_LANGUAGE:C>:-std=gnu17>)
# SOS2 backs every mapping with RAM when it is created, so reserved address
# space is real memory. 8 KiB slices (instead of 32 KiB) shrink small pages and
# the minimum arena to 1 MiB; reserve arenas at that size, mapped committed
# instead of committing them slice by slice with mprotect.
add_compile_definitions(MI_ARENA_SLICE_SHIFT=13 MI_DEFAULT_ARENA_RESERVE=1024 MI_DEFAULT_ARENA_EAGER_COMMIT=1)
# On arm64 mimalloc builds for ARMv8.1-A by default; the Cortex-A53 is ARMv8.0.
set(MI_NO_OPT_ARCH ON CACHE BOOL "Keep the target CPU's architecture" FORCE)
