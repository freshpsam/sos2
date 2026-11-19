# One browser kernel per board and architecture; no embedded experiments or
# sample apps. ARM32 serves QEMU and the Redmi 2; arm64 serves QEMU and the
# Galaxy Tab A7 Lite.
# On PATH, or an Arm GNU Toolchain unpacked into .tools/ (as tools/arch.py).
ARM32_PREFIX ?= $(if $(shell command -v arm-none-eabi-gcc 2>/dev/null),arm-none-eabi-,$(CURDIR)/.tools/arm-none-eabi/bin/arm-none-eabi-)
ARM32_CC := $(ARM32_PREFIX)gcc
ARM32_OBJCOPY := $(ARM32_PREFIX)objcopy
ARM64_PREFIX ?= $(if $(shell command -v aarch64-none-elf-gcc 2>/dev/null),aarch64-none-elf-,$(CURDIR)/.tools/aarch64-none-elf/bin/aarch64-none-elf-)
ARM64_CC := $(ARM64_PREFIX)gcc
ARM64_OBJCOPY := $(ARM64_PREFIX)objcopy
HOST_CC ?= cc
HOST_CFLAGS ?= -std=c11 -O1 -g -Wall -Wextra -Werror
COMMON_FLAGS := -marm -mfloat-abi=soft -mgeneral-regs-only -mno-unaligned-access \
    -std=c11 -O2 -g -ffreestanding -fno-builtin -fno-stack-protector \
    -fno-pic -fno-pie -fno-unwind-tables -fno-asynchronous-unwind-tables \
    -Wall -Wextra -Werror -MMD -MP -Iinclude -Iinclude/boot -Iarch/arm/include -Ithird_party/lzma \
    -Ithird_party/littlefs -DLFS_CONFIG=sos2_lfs_config.h \
    -Iinclude/lwip -Ithird_party/lwip/src/include
QEMU_FLAGS := $(COMMON_FLAGS) -mcpu=cortex-a15
REDMI2_PROTECTED_CFLAGS := $(COMMON_FLAGS) -mcpu=cortex-a53 -DSOS2_BOARD_REDMI2=1 -Iboards/redmi2 -Ithird_party/jitterentropy
include tools/wifi.mk
REDMI2_PROTECTED_CFLAGS += $(WIFI_CRYPTO_FLAGS)

LIBRARIES := third_party/littlefs/lfs.c third_party/lzma/LzmaDec.c \
    $(wildcard third_party/lwip/src/core/*.c third_party/lwip/src/core/ipv4/*.c) third_party/lwip/src/netif/ethernet.c
ARM_SOURCES := arch/arm/mmu.c arch/arm/machine.c arch/arm/timer.c arch/arm/boot_cache.c arch/arm/smp.c
# QEMU's randomness is virtio's (entropy.c); the devices measure CPU jitter.
QEMU_SOURCES := $(wildcard kernel/*.c) \
    $(filter-out kernel/protection/jitter_entropy.c,$(wildcard kernel/protection/*.c kernel/net/*.c)) \
    $(ARM_SOURCES) $(LIBRARIES)
PHONE_SOURCES := $(filter-out kernel/ramfb.c kernel/input.c kernel/virtio_gpu.c,$(wildcard kernel/*.c)) \
    $(filter-out kernel/protection/block.c kernel/protection/entropy.c,$(wildcard kernel/protection/*.c)) \
    $(wildcard boards/redmi2/*.c) kernel/net/network.c kernel/net/network_time.c kernel/net/strings.c \
    $(ARM_SOURCES) $(LIBRARIES) $(wildcard third_party/jitterentropy/src/*.c) $(WIFI_SOURCES) \
    $(HOSTAP_SOURCES) $(WIFI_MBEDTLS_SOURCES)
QEMU_OBJECTS := $(patsubst %.c,build/resources-arm32/%.o,$(QEMU_SOURCES)) \
    build/resources-arm32/arch/arm/boot.o build/resources-arm32/arch/arm/process.o build/resources-arm32/arch/arm/cache_ops.o build/resources-arm32/arch/arm/psci.o
PHONE_OBJECTS := $(patsubst %.c,build/redmi2-protected-arm32/%.o,$(PHONE_SOURCES)) \
    build/redmi2-protected-arm32/arch/arm/boot.o build/redmi2-protected-arm32/arch/arm/process.o build/redmi2-protected-arm32/arch/arm/scm.o build/redmi2-protected-arm32/arch/arm/cache_ops.o build/redmi2-protected-arm32/arch/arm/psci.o

build/resources-arm32/%.o: %.c Makefile tools/build.mk
	@mkdir -p $(@D)
	$(ARM32_CC) $(QEMU_FLAGS) -c $< -o $@
build/resources-arm32/%.o: %.S Makefile tools/build.mk
	@mkdir -p $(@D)
	$(ARM32_CC) $(QEMU_FLAGS) -c $< -o $@
build/redmi2-protected-arm32/third_party/jitterentropy/%.o: third_party/jitterentropy/%.c Makefile tools/build.mk
	@mkdir -p $(@D)
	$(ARM32_CC) $(REDMI2_PROTECTED_CFLAGS) -O0 -fwrapv -c $< -o $@
build/redmi2-protected-arm32/%.o: %.c Makefile tools/build.mk
	@mkdir -p $(@D)
	$(ARM32_CC) $(REDMI2_PROTECTED_CFLAGS) -c $< -o $@
build/redmi2-protected-arm32/%.o: %.S Makefile tools/build.mk
	@mkdir -p $(@D)
	$(ARM32_CC) $(REDMI2_PROTECTED_CFLAGS) -c $< -o $@

build/resources-arm32/kernel.elf: $(QEMU_OBJECTS) arch/arm/resources.ld
	$(ARM32_CC) $(QEMU_FLAGS) -nostdlib -Wl,-T,arch/arm/resources.ld,-Map,$(@:.elf=.map),--build-id=none,-z,noexecstack $(QEMU_OBJECTS) -lgcc -o $@
build/redmi2-protected-arm32/kernel.elf: $(PHONE_OBJECTS) boards/redmi2/kernel.ld
	$(ARM32_CC) $(REDMI2_PROTECTED_CFLAGS) -nostdlib -Wl,-T,boards/redmi2/kernel.ld,-Map,$(@:.elf=.map),--build-id=none,-z,noexecstack,--gc-sections $(PHONE_OBJECTS) -lgcc -o $@
build/%/kernel.img: build/%/kernel.elf
	$(ARM32_OBJCOPY) -O binary $< $@

# arm64: the shared kernel with AArch64's entry, exceptions, MMU and cache code.
# Translation is off until vm_init, so data accesses are Device memory: never
# unaligned, and no exclusive or outline atomics (kernel data stays uncached).
ARM64_FLAGS := $(subst -Iarch/arm/include,-Iarch/arm64/include,$(filter-out -marm -mfloat-abi=soft -mno-unaligned-access,$(COMMON_FLAGS))) \
    -mcpu=cortex-a53 -mstrict-align -mno-outline-atomics -mfix-cortex-a53-835769 -mfix-cortex-a53-843419
ARM64_SOURCES := $(filter-out arch/arm/mmu.c arch/arm/boot_cache.c,$(QEMU_SOURCES)) arch/arm64/mmu.c arch/arm64/boot_cache.c
ARM64_OBJECTS := $(patsubst %.c,build/resources-arm64/%.o,$(ARM64_SOURCES)) \
    $(patsubst %,build/resources-arm64/arch/arm64/%.o,boot entry cache_ops psci)
build/resources-arm64/%.o: %.c Makefile tools/build.mk
	@mkdir -p $(@D)
	$(ARM64_CC) $(ARM64_FLAGS) -c $< -o $@
build/resources-arm64/%.o: %.S Makefile tools/build.mk
	@mkdir -p $(@D)
	$(ARM64_CC) $(ARM64_FLAGS) -c $< -o $@
build/resources-arm64/kernel.elf: $(ARM64_OBJECTS) arch/arm64/resources.ld
	$(ARM64_CC) $(ARM64_FLAGS) -nostdlib -Wl,-T,arch/arm64/resources.ld,-Map,$(@:.elf=.map),--build-id=none,-z,noexecstack,--fix-cortex-a53-843419 $(ARM64_OBJECTS) -lgcc -o $@
build/resources-arm64/kernel.img: build/resources-arm64/kernel.elf
	$(ARM64_OBJCOPY) -O binary $< $@

# The Galaxy Tab A7 Lite (SM-T220): the arm64 kernel with the tablet's
# interrupt controller, LK's framebuffer and its watchdog, its eMMC in place
# of virtio's disk (boards/gta7lite), and the phone's CPU-jitter randomness.
GTA7LITE_FLAGS := $(ARM64_FLAGS) -DSOS2_BOARD_GTA7LITE=1 -Iboards/gta7lite -Ithird_party/jitterentropy \
    $(WIFI_CRYPTO_FLAGS)
GTA7LITE_SOURCES := $(filter-out kernel/ramfb.c kernel/input.c kernel/virtio_gpu.c \
    kernel/protection/entropy.c kernel/protection/block.c kernel/net/virtio_net.c,$(ARM64_SOURCES)) \
    kernel/protection/jitter_entropy.c \
    $(wildcard boards/gta7lite/*.c third_party/jitterentropy/src/*.c) $(WIFI_SOURCES) \
    $(HOSTAP_SOURCES) $(WIFI_MBEDTLS_SOURCES)
GTA7LITE_OBJECTS := $(patsubst %.c,build/gta7lite-arm64/%.o,$(GTA7LITE_SOURCES)) \
    $(patsubst %,build/gta7lite-arm64/arch/arm64/%.o,boot entry cache_ops psci)
build/gta7lite-arm64/third_party/jitterentropy/%.o: third_party/jitterentropy/%.c Makefile tools/build.mk
	@mkdir -p $(@D)
	$(ARM64_CC) $(GTA7LITE_FLAGS) -O0 -fwrapv -c $< -o $@
build/gta7lite-arm64/%.o: %.c Makefile tools/build.mk
	@mkdir -p $(@D)
	$(ARM64_CC) $(GTA7LITE_FLAGS) -c $< -o $@
build/gta7lite-arm64/%.o: %.S Makefile tools/build.mk
	@mkdir -p $(@D)
	$(ARM64_CC) $(GTA7LITE_FLAGS) -c $< -o $@
build/gta7lite-arm64/kernel.elf: $(GTA7LITE_OBJECTS) arch/arm64/resources.ld
	$(ARM64_CC) $(GTA7LITE_FLAGS) -nostdlib -Wl,-T,arch/arm64/resources.ld,-Map,$(@:.elf=.map),--build-id=none,-z,noexecstack,--fix-cortex-a53-843419,--gc-sections $(GTA7LITE_OBJECTS) -lgcc -o $@
build/gta7lite-arm64/kernel.img: build/gta7lite-arm64/kernel.elf
	$(ARM64_OBJCOPY) -O binary $< $@

build/redmi2/boot.dtb: boards/redmi2/boot.dts tools/dtc.py
	@mkdir -p $(@D)
	$(PYTHON) tools/dtc.py -I dts -O dtb -o $@ $<

build/host/mkfs: tools/mkfs.c third_party/littlefs/lfs.c third_party/littlefs/lfs.h include/sos2_lfs_config.h
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -Iinclude -Ithird_party/littlefs -DLFS_CONFIG=sos2_lfs_config.h -DSOS2_LFS_HOST tools/mkfs.c third_party/littlefs/lfs.c -o $@
build/host/fs-image: tools/fs_image.c include/apps.h include/runtime_files.h third_party/littlefs/lfs.c third_party/littlefs/lfs.h include/sos2_lfs_config.h
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -Iinclude -Ithird_party/littlefs -DLFS_CONFIG=sos2_lfs_config.h -DSOS2_LFS_HOST tools/fs_image.c third_party/littlefs/lfs.c -o $@

-include $(QEMU_OBJECTS:.o=.d) $(PHONE_OBJECTS:.o=.d) $(ARM64_OBJECTS:.o=.d) $(GTA7LITE_OBJECTS:.o=.d)
