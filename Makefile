PYTHON ?= python3
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

# Public interface. Internal build rules use file paths, not command aliases.
# ARCH=arm64 builds and runs QEMU's AArch64 system (tools/arch.py). The
# devices have one architecture each, so their targets set ARCH themselves:
# phone ARM32 (the Redmi 2), tablet arm64 (the Galaxy Tab A7 Lite), nexus5
# ARM32.
DEVICE_ARCH := $(sort $(if $(filter phone nexus5,$(MAKECMDGOALS)),arm32) \
                      $(if $(filter tablet,$(MAKECMDGOALS)),arm64))
ifneq ($(word 2,$(DEVICE_ARCH)),)
$(error phone is ARM32 and tablet arm64: run them separately)
endif
ifneq ($(DEVICE_ARCH),)
ifeq ($(origin ARCH),command line)
ifneq ($(ARCH),$(DEVICE_ARCH))
$(error $(filter phone tablet nexus5,$(MAKECMDGOALS)) builds for $(DEVICE_ARCH): leave out ARCH=$(ARCH))
endif
endif
ARCH := $(DEVICE_ARCH)
endif
ARCH ?= arm32
ifeq ($(filter $(ARCH),arm32 arm64),)
$(error ARCH must be arm32 or arm64)
endif
ENGINE ?= build/native-webkit$(if $(filter arm64,$(ARCH)),-arm64)/native-web.bundle
STORAGE ?= ram
JOBS ?= 8
RUN_ARGS ?=
REDMI2_WIFI_FIRMWARE ?= third_party/redmi2-firmware
# The GPU's firmware, once make fetch has put it here: without it the phone
# composes the screen on the CPU.
REDMI2_ADRENO_FIRMWARE ?= $(if $(wildcard build/redmi2-adreno/a300_pm4.fw),build/redmi2-adreno)
# The tablet's touch firmware (docs/gta7lite.md): without it there is no touch.
GTA7LITE_TOUCH_FIRMWARE ?= $(if $(GTA7LITE_STOCK),$(GTA7LITE_STOCK)/touch-firmware)
GTA7LITE_WIFI_FIRMWARE ?= $(if $(GTA7LITE_STOCK),$(GTA7LITE_STOCK)/wifi-firmware)
# The Nexus 5's Wi-Fi firmware, once copied from its Android (docs/nexus5.md):
# without it there is no Wi-Fi.
NEXUS5_STOCK ?= $(if $(wildcard build/nexus5-stock/wifi-firmware),build/nexus5-stock)
NEXUS5_WIFI_FIRMWARE ?= $(if $(NEXUS5_STOCK),$(NEXUS5_STOCK)/wifi-firmware)
include tools/build.mk
export SOS2_ARCH := $(ARCH)
export ARM32_PREFIX ARM64_PREFIX REDMI2_WIFI_FIRMWARE REDMI2_WIFI_PROFILE REDMI2_ADRENO_FIRMWARE REDMI2_JSC GTA7LITE_STOCK GTA7LITE_TOUCH_FIRMWARE GTA7LITE_WIFI_FIRMWARE GTA7LITE_WIFI_PROFILE GTA7LITE_JSC NEXUS5_STOCK NEXUS5_WIFI_FIRMWARE NEXUS5_WIFI_PROFILE NEXUS5_JSC PUBLISH

.PHONY: all help fetch engine run phone publish tablet nexus5 clean
all: build/resources-$(ARCH)/kernel.img build/host/mkfs build/host/fs-image

help:
	@$(PYTHON) tools/project.py help

# Downloads are explicit; building or packaging never fetches dependencies.
fetch:
	$(PYTHON) tools/build_native_runtime.py fetch
	$(PYTHON) tools/build_native_render.py fetch
	$(PYTHON) tools/build_native_webkit.py fetch
	$(PYTHON) tools/build_native_mesa.py fetch
	$(PYTHON) tools/adreno_firmware.py fetch build/redmi2-adreno

engine:
	$(PYTHON) tools/build_native_runtime.py newlib -j $(JOBS)
	$(PYTHON) tools/build_native_runtime.py sdk
	$(PYTHON) tools/build_native_runtime.py libstdcxx -j $(JOBS)
	$(PYTHON) tools/build_native_runtime.py icu -j $(JOBS)
	$(PYTHON) tools/build_native_render.py build -j $(JOBS)
	$(PYTHON) tools/build_native_webkit.py build -j $(JOBS)

run: all
	$(PYTHON) tools/project.py run --engine "$(ENGINE)" --storage "$(STORAGE)" -- $(RUN_ARGS)

phone: build/redmi2-protected-arm32/kernel.img build/redmi2/boot.dtb
	$(PYTHON) tools/project.py phone --engine "$(ENGINE)" --storage "$(STORAGE)"

# The Galaxy Tab A7 Lite, arm64: GTA7LITE_STOCK=folder for boot.img.
tablet: build/gta7lite-arm64/kernel.img
	$(PYTHON) tools/project.py tablet --engine "$(ENGINE)"

# The Nexus 5 (LG hammerhead), ARM32: a RAM image for fastboot boot and the
# installed system.
nexus5: build/nexus5-arm32/kernel.img build/nexus5/boot.dtb
	$(PYTHON) tools/project.py nexus5 --engine "$(ENGINE)"

# Publishing is explicit and pushes only what make phone, make tablet and
# make nexus5 last wrote; PUBLISH=gta7lite (redmi2, nexus5) limits it to that
# device.
publish:
	$(PYTHON) tools/project.py publish

clean:
	$(PYTHON) tools/project.py clean
