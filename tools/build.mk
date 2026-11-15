# One ARM32 browser kernel per board; no embedded experiments or sample apps.
ARM32_PREFIX ?= arm-none-eabi-
ARM32_CC := $(ARM32_PREFIX)gcc
ARM32_OBJCOPY := $(ARM32_PREFIX)objcopy
HOST_CC ?= cc
HOST_CFLAGS ?= -std=c11 -O1 -g -Wall -Wextra -Werror
COMMON_FLAGS := -marm -mfloat-abi=soft -mgeneral-regs-only -mno-unaligned-access \
    -std=c11 -O2 -g -ffreestanding -fno-builtin -fno-stack-protector \
    -fno-pic -fno-pie -fno-unwind-tables -fno-asynchronous-unwind-tables \
    -Wall -Wextra -Werror -MMD -MP -Iinclude -Iinclude/boot -Ithird_party/lzma \
    -Ithird_party/littlefs -DLFS_CONFIG=sos2_lfs_config.h \
    -Iinclude/lwip -Ithird_party/lwip/src/include
QEMU_FLAGS := $(COMMON_FLAGS) -mcpu=cortex-a15 -DSOS2_RESOURCES=1
REDMI2_PROTECTED_CFLAGS := $(COMMON_FLAGS) -mcpu=cortex-a53 -DSOS2_BOARD_REDMI2=1 -Ithird_party/jitterentropy
include tools/wifi.mk
REDMI2_PROTECTED_CFLAGS += $(WIFI_CRYPTO_FLAGS)

PHONE_UI := $(wildcard kernel/redmi2*.c kernel/msm8916*.c) kernel/phone_input.c kernel/handoff.c
LIBRARIES := third_party/littlefs/lfs.c third_party/lzma/LzmaDec.c \
    $(wildcard third_party/lwip/src/core/*.c third_party/lwip/src/core/ipv4/*.c) third_party/lwip/src/netif/ethernet.c
ARM_SOURCES := arch/arm/mmu.c arch/arm/machine.c arch/arm/timer.c arch/arm/boot_cache.c arch/arm/smp.c
QEMU_SOURCES := $(filter-out $(PHONE_UI),$(wildcard kernel/*.c)) \
    $(wildcard kernel/protection/*.c kernel/net/*.c) $(ARM_SOURCES) $(LIBRARIES)
PHONE_SOURCES := $(filter-out kernel/ramfb.c kernel/input.c kernel/virtio_gpu.c,$(wildcard kernel/*.c)) \
    $(filter-out kernel/protection/block.c kernel/protection/entropy.c,$(wildcard kernel/protection/*.c)) \
    $(wildcard boards/redmi2/*.c) kernel/net/network.c kernel/net/network_time.c kernel/net/strings.c \
    $(ARM_SOURCES) $(LIBRARIES) $(wildcard third_party/jitterentropy/src/*.c) $(HOSTAP_SOURCES) $(WIFI_MBEDTLS_SOURCES)
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
build/redmi2-protected-arm32/kernel.elf: $(PHONE_OBJECTS) arch/arm/redmi2_protected.ld
	$(ARM32_CC) $(REDMI2_PROTECTED_CFLAGS) -nostdlib -Wl,-T,arch/arm/redmi2_protected.ld,-Map,$(@:.elf=.map),--build-id=none,-z,noexecstack,--gc-sections $(PHONE_OBJECTS) -lgcc -o $@
build/%/kernel.img: build/%/kernel.elf
	$(ARM32_OBJCOPY) -O binary $< $@

build/redmi2/boot.dtb: boards/redmi2/boot.dts tools/dtc.py
	@mkdir -p $(@D)
	$(PYTHON) tools/dtc.py -I dts -O dtb -o $@ $<

build/host/mkfs: tools/mkfs.c third_party/littlefs/lfs.c third_party/littlefs/lfs.h include/sos2_lfs_config.h
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -Iinclude -Ithird_party/littlefs -DLFS_CONFIG=sos2_lfs_config.h -DSOS2_LFS_HOST tools/mkfs.c third_party/littlefs/lfs.c -o $@
build/host/fs-image: tools/fs_image.c include/apps.h third_party/littlefs/lfs.c third_party/littlefs/lfs.h include/sos2_lfs_config.h
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -Iinclude -Ithird_party/littlefs -DLFS_CONFIG=sos2_lfs_config.h -DSOS2_LFS_HOST tools/fs_image.c third_party/littlefs/lfs.c -o $@

-include $(QEMU_OBJECTS:.o=.d) $(PHONE_OBJECTS:.o=.d)
