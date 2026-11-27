# Source builds for SOS2's native x64 runtime. No target Linux search paths.
# The compilers come from X64_PREFIX, PATH or the x86_64-elf toolchain that
# tools/build_x64_toolchain.py builds into .tools/x86_64-elf (as
# tools/arch.py finds them).
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
get_filename_component(SOS2_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(DEFINED ENV{X64_PREFIX} AND NOT "$ENV{X64_PREFIX}" STREQUAL "")
    set(_SOS2_PREFIX "$ENV{X64_PREFIX}")
    if(NOT IS_ABSOLUTE "${_SOS2_PREFIX}" AND _SOS2_PREFIX MATCHES "/")
        set(_SOS2_PREFIX "${SOS2_ROOT}/${_SOS2_PREFIX}")
    endif()
else()
    find_program(_SOS2_GCC x86_64-elf-gcc PATHS "${SOS2_ROOT}/.tools/x86_64-elf/bin" REQUIRED)
    string(REGEX REPLACE "gcc$" "" _SOS2_PREFIX "${_SOS2_GCC}")
endif()
set(CMAKE_C_COMPILER "${_SOS2_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${_SOS2_PREFIX}g++")
set(CMAKE_ASM_COMPILER "${_SOS2_PREFIX}gcc")
set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
set(SOS2_NATIVE_TARGET TRUE CACHE BOOL "Build for SOS2's native runtime")
set(SOS2_NATIVE "${SOS2_ROOT}/build/native-toolchain-x64")
set(CMAKE_SYSROOT "${SOS2_NATIVE}/install/x86_64-elf")
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# x86-64-v2, RIP-relative code (programs are linked at 0x1010000000, beyond the
# small code model's absolute addresses), linked --no-relax so that ld keeps
# GOT loads RIP-relative rather than 32-bit absolute, as tools/arch.py.
set(_SOS2_FLAGS "-march=x86-64-v2 -mtune=generic -fpie -ffunction-sections -fdata-sections -I${SOS2_ROOT}/sdk/include -I${SOS2_ROOT}/include -isystem ${CMAKE_SYSROOT}/include -include ${SOS2_ROOT}/sdk/include/sos2/features.h")
set(CMAKE_C_FLAGS_INIT "${_SOS2_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "-isystem ${CMAKE_SYSROOT}/include/c++/14.2.0 -isystem ${CMAKE_SYSROOT}/include/c++/14.2.0/x86_64-elf ${_SOS2_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -no-pie -nostartfiles -T${SOS2_ROOT}/sdk/runtime/linker-x64.ld -Wl,-z,max-page-size=4096,-z,noexecstack,--gc-sections,--wrap=_Unwind_Find_FDE,--no-relax -L${CMAKE_SYSROOT}/lib -Wl,--whole-archive ${SOS2_NATIVE}/sdk/libsos2.a -Wl,--no-whole-archive")
set(CMAKE_C_STANDARD_LIBRARIES "-Wl,--start-group -lc -lm -lgcc -Wl,--end-group")
set(CMAKE_CXX_STANDARD_LIBRARIES "-Wl,--start-group -lstdc++ -lc -lm -lgcc -Wl,--end-group")
