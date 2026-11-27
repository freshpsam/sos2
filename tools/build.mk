# One browser kernel per board and architecture; no embedded experiments or
# sample apps. ARM32 serves QEMU, the Redmi 2 and the Nexus 5; arm64 serves
# QEMU and the Galaxy Tab A7 Lite; x64 serves QEMU's microvm and PCs.
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
# What sets the devices apart, for the code they share (QEMU's machines have
# none of these):
#   SOS2_DEVICE              a device, not a QEMU machine
#   SOS2_PHONE               the ARM32 Qualcomm phones: an LK bootloader starts an
#                            Android boot image whose ramdisk holds the resources,
#                            expanded under the boot cache's MMU into the browser's
#                            arena, beside a framebuffer the kernel maps; the PMIC
#                            keeps the time
#   SOS2_LOADER_FRAMEBUFFER  the bootloader's framebuffer is the screen, which the
#                            CPU draws
#   SOS2_NAVIGATION_BUTTONS  no Home or Back key: the top bar has HOME and BACK
#   SOS2_CPU_SLEEP           idle secondary CPUs power down in standby
REDMI2_FAMILY := -DSOS2_DEVICE=1 -DSOS2_PHONE=1 -DSOS2_CPU_SLEEP=1
GTA7LITE_FAMILY := -DSOS2_DEVICE=1 -DSOS2_LOADER_FRAMEBUFFER=1 -DSOS2_NAVIGATION_BUTTONS=1 -DSOS2_CPU_SLEEP=1
NEXUS5_FAMILY := -DSOS2_DEVICE=1 -DSOS2_PHONE=1 -DSOS2_LOADER_FRAMEBUFFER=1 -DSOS2_NAVIGATION_BUTTONS=1
REDMI2_PROTECTED_CFLAGS := $(COMMON_FLAGS) -mcpu=cortex-a53 -DSOS2_BOARD_REDMI2=1 $(REDMI2_FAMILY) -Iboards/redmi2 -Iboards/qcom -Ithird_party/jitterentropy
include tools/wifi.mk
REDMI2_PROTECTED_CFLAGS += $(WIFI_CRYPTO_FLAGS)

LIBRARIES := third_party/littlefs/lfs.c third_party/lzma/LzmaDec.c \
    $(wildcard third_party/lwip/src/core/*.c third_party/lwip/src/core/ipv4/*.c) third_party/lwip/src/netif/ethernet.c
ARM_SOURCES := arch/arm/mmu.c arch/arm/machine.c arch/arm/timer.c arch/arm/boot_cache.c arch/arm/smp.c
# QEMU's randomness is virtio's (entropy.c), its disk virtio's (block.c) and
# its sound virtio's (virtio_snd.c); the devices measure CPU jitter, keep
# their data on the eMMC (emmc_store.c) and play sound through their boards'
# drivers.
QEMU_SOURCES := $(wildcard kernel/*.c) \
    $(filter-out kernel/protection/jitter_entropy.c kernel/protection/emmc_store.c,$(wildcard kernel/protection/*.c kernel/net/*.c)) \
    kernel/audio/audio.c kernel/audio/virtio_snd.c $(ARM_SOURCES) $(LIBRARIES)
# An ARM32 phone's kernel ($(call PHONE_KERNEL,board)): the shared kernel
# without QEMU's virtio devices, the board's drivers, the Qualcomm ones the
# phones share (boards/qcom), Wi-Fi on the shared station and CPU-jitter
# randomness.
PHONE_KERNEL = $(filter-out kernel/ramfb.c kernel/input.c kernel/virtio_gpu.c kernel/qemu_board.c,$(wildcard kernel/*.c)) \
    $(filter-out kernel/protection/block.c kernel/protection/entropy.c,$(wildcard kernel/protection/*.c)) \
    $(wildcard boards/$(1)/*.c) boards/qcom/qup_i2c.c boards/qcom/scm.c boards/qcom/sdhci.c boards/qcom/smd.c kernel/net/network.c kernel/net/network_time.c kernel/net/strings.c kernel/audio/audio.c \
    $(ARM_SOURCES) $(LIBRARIES) $(wildcard third_party/jitterentropy/src/*.c) $(WIFI_SOURCES) \
    $(HOSTAP_SOURCES) $(WIFI_MBEDTLS_SOURCES)
PHONE_SOURCES := $(call PHONE_KERNEL,redmi2)
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

# x64: the shared kernel with x64's entry, exceptions, MMU, local APIC
# and SMP code (arch/x64), for QEMU's microvm, which keeps the ARM machines'
# virtio-mmio devices. Kernel code runs with interrupts masked and general
# registers only; -mno-red-zone keeps the convention all the same. QEMU loads
# the ELF itself through its PVH note, so there is no binary image.
X64_PREFIX ?= $(if $(shell command -v x86_64-elf-gcc 2>/dev/null),x86_64-elf-,$(CURDIR)/.tools/x86_64-elf/bin/x86_64-elf-)
X64_CC := $(X64_PREFIX)gcc
X64_FLAGS := $(subst -Iarch/arm/include,-Iarch/x64/include,$(filter-out -marm -mfloat-abi=soft -mno-unaligned-access,$(COMMON_FLAGS))) \
    -m64 -march=x86-64-v2 -mtune=generic -mno-red-zone -mcmodel=small
X64_SOURCES := $(filter-out $(ARM_SOURCES),$(QEMU_SOURCES)) $(wildcard arch/x64/*.c)
X64_OBJECTS := $(patsubst %.c,build/resources-x64/%.o,$(X64_SOURCES)) \
    $(patsubst %,build/resources-x64/arch/x64/%.o,boot entry)
build/resources-x64/%.o: %.c Makefile tools/build.mk
	@mkdir -p $(@D)
	$(X64_CC) $(X64_FLAGS) -c $< -o $@
build/resources-x64/%.o: %.S Makefile tools/build.mk
	@mkdir -p $(@D)
	$(X64_CC) $(X64_FLAGS) -c $< -o $@
build/resources-x64/kernel.elf: $(X64_OBJECTS) arch/x64/kernel.ld
	$(X64_CC) $(X64_FLAGS) -nostdlib -Wl,-T,arch/x64/kernel.ld,-Map,$(@:.elf=.map),--build-id=none,-z,noexecstack,-z,max-page-size=4096 $(X64_OBJECTS) -lgcc -o $@

# A PC (SOS2_BOARD_PC), in QEMU's q35 with OVMF so far: the x64 kernel,
# entered at uefi_start by SOS2's UEFI loader from a USB stick, with the
# loader's GOP framebuffer as the screen and the PC's drivers (boards/pc)
# in place of microvm's virtio display and input.
PC_FAMILY := -DSOS2_DEVICE=1 -DSOS2_LOADER_FRAMEBUFFER=1 -DSOS2_NAVIGATION_BUTTONS=1
PC_FLAGS := $(X64_FLAGS) -DSOS2_BOARD_PC=1 $(PC_FAMILY) -Iboards/pc
PC_SOURCES := $(filter-out kernel/ramfb.c kernel/input.c kernel/virtio_gpu.c kernel/qemu_board.c \
                kernel/protection/block.c kernel/protection/entropy.c kernel/net/virtio_net.c \
                kernel/audio/virtio_snd.c,$(X64_SOURCES)) \
    kernel/protection/emmc_store.c $(wildcard boards/pc/*.c)
PC_OBJECTS := $(patsubst %.c,build/pc-x64/%.o,$(PC_SOURCES)) \
    $(patsubst %,build/pc-x64/arch/x64/%.o,boot entry)
build/pc-x64/%.o: %.c Makefile tools/build.mk
	@mkdir -p $(@D)
	$(X64_CC) $(PC_FLAGS) -c $< -o $@
build/pc-x64/%.o: %.S Makefile tools/build.mk
	@mkdir -p $(@D)
	$(X64_CC) $(PC_FLAGS) -c $< -o $@
build/pc-x64/kernel.elf: $(PC_OBJECTS) arch/x64/kernel.ld
	$(X64_CC) $(PC_FLAGS) -nostdlib -Wl,-T,arch/x64/kernel.ld,-e,uefi_start,-Map,$(@:.elf=.map),--build-id=none,-z,noexecstack,-z,max-page-size=4096 $(PC_OBJECTS) -lgcc -o $@

# SOS2's UEFI loader (boot/uefi): a position-independent ELF that
# tools/elf2efi.py turns into the PE32+ EFI application on the stick. Calls
# into the firmware use Microsoft's convention (boot/uefi/efi.h).
UEFI_FLAGS := -std=c11 -O2 -g -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-stack-protector \
    -fpie -mno-red-zone -mgeneral-regs-only -m64 -march=x86-64-v2 -fno-unwind-tables \
    -fno-asynchronous-unwind-tables -Wall -Wextra -Werror -MMD -MP -Iinclude/boot -Iboot/uefi
UEFI_OBJECTS := build/pc-x64/boot/uefi/loader.o
build/pc-x64/boot/uefi/%.o: boot/uefi/%.c Makefile tools/build.mk
	@mkdir -p $(@D)
	$(X64_CC) $(UEFI_FLAGS) -c $< -o $@
build/pc-x64/loader.elf: $(UEFI_OBJECTS) boot/uefi/loader.ld
	$(X64_PREFIX)ld -pie --no-dynamic-linker -z text -z noexecstack -z max-page-size=4096 \
	    -T boot/uefi/loader.ld -Map $(@:.elf=.map) -o $@ $(UEFI_OBJECTS)
build/pc-x64/BOOTX64.EFI: build/pc-x64/loader.elf tools/elf2efi.py
	$(PYTHON) tools/elf2efi.py $< $@

# The Galaxy Tab A7 Lite (SM-T220): the arm64 kernel with the tablet's
# interrupt controller, LK's framebuffer and its watchdog, its eMMC in place
# of virtio's disk (boards/gta7lite), and the phone's CPU-jitter randomness.
GTA7LITE_FLAGS := $(ARM64_FLAGS) -DSOS2_BOARD_GTA7LITE=1 $(GTA7LITE_FAMILY) -Iboards/gta7lite -Ithird_party/jitterentropy \
    $(WIFI_CRYPTO_FLAGS)
GTA7LITE_SOURCES := $(filter-out kernel/ramfb.c kernel/input.c kernel/virtio_gpu.c kernel/qemu_board.c \
    kernel/protection/entropy.c kernel/protection/block.c kernel/net/virtio_net.c \
    kernel/audio/virtio_snd.c,$(ARM64_SOURCES)) \
    kernel/protection/jitter_entropy.c kernel/protection/emmc_store.c \
    $(wildcard boards/gta7lite/*.c third_party/jitterentropy/src/*.c) kernel/usb/fastboot.c $(WIFI_SOURCES) \
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

# The Nexus 5 (LG hammerhead, MSM8974): the ARM32 kernel for its Krait cores,
# ARMv7 like QEMU's Cortex-A15, with the stock bootloader's splash as its
# screen (boards/nexus5), the QUP I2C and secure-call drivers it shares with
# the Redmi 2 (boards/qcom), Wi-Fi on the shared station (kernel/wifi), the
# phone's CPU-jitter randomness and its eMMC.
NEXUS5_CFLAGS := $(COMMON_FLAGS) -mcpu=cortex-a15 -DSOS2_BOARD_NEXUS5=1 $(NEXUS5_FAMILY) -Iboards/nexus5 -Iboards/qcom -Ithird_party/jitterentropy \
    $(WIFI_CRYPTO_FLAGS)
NEXUS5_SOURCES := $(call PHONE_KERNEL,nexus5) kernel/usb/fastboot.c
NEXUS5_OBJECTS := $(patsubst %.c,build/nexus5-arm32/%.o,$(NEXUS5_SOURCES)) \
    $(patsubst %,build/nexus5-arm32/arch/arm/%.o,boot process scm cache_ops psci)
build/nexus5-arm32/third_party/jitterentropy/%.o: third_party/jitterentropy/%.c Makefile tools/build.mk
	@mkdir -p $(@D)
	$(ARM32_CC) $(NEXUS5_CFLAGS) -O0 -fwrapv -c $< -o $@
build/nexus5-arm32/%.o: %.c Makefile tools/build.mk
	@mkdir -p $(@D)
	$(ARM32_CC) $(NEXUS5_CFLAGS) -c $< -o $@
build/nexus5-arm32/%.o: %.S Makefile tools/build.mk
	@mkdir -p $(@D)
	$(ARM32_CC) $(NEXUS5_CFLAGS) -c $< -o $@
build/nexus5-arm32/kernel.elf: $(NEXUS5_OBJECTS) boards/nexus5/kernel.ld
	$(ARM32_CC) $(NEXUS5_CFLAGS) -nostdlib -Wl,-T,boards/nexus5/kernel.ld,-Map,$(@:.elf=.map),--build-id=none,-z,noexecstack,--gc-sections $(NEXUS5_OBJECTS) -lgcc -o $@
build/nexus5/boot.dtb: boards/nexus5/boot.dts tools/dtc.py
	@mkdir -p $(@D)
	$(PYTHON) tools/dtc.py -I dts -O dtb -o $@ $<

build/host/mkfs: tools/mkfs.c third_party/littlefs/lfs.c third_party/littlefs/lfs.h include/sos2_lfs_config.h
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -Iinclude -Ithird_party/littlefs -DLFS_CONFIG=sos2_lfs_config.h -DSOS2_LFS_HOST tools/mkfs.c third_party/littlefs/lfs.c -o $@
build/host/fs-image: tools/fs_image.c include/apps.h include/runtime_files.h third_party/littlefs/lfs.c third_party/littlefs/lfs.h include/sos2_lfs_config.h
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -Iinclude -Ithird_party/littlefs -DLFS_CONFIG=sos2_lfs_config.h -DSOS2_LFS_HOST tools/fs_image.c third_party/littlefs/lfs.c -o $@

-include $(QEMU_OBJECTS:.o=.d) $(PHONE_OBJECTS:.o=.d) $(ARM64_OBJECTS:.o=.d) $(X64_OBJECTS:.o=.d) $(GTA7LITE_OBJECTS:.o=.d) \
    $(NEXUS5_OBJECTS:.o=.d) $(PC_OBJECTS:.o=.d) $(UEFI_OBJECTS:.o=.d)
