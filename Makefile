PYTHON ?= python3
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

# Public interface. Internal build rules use file paths, not command aliases.
ENGINE ?= build/native-webkit/native-web.bundle
STORAGE ?= ram
JOBS ?= 8
RUN_ARGS ?=
REDMI2_WIFI_FIRMWARE ?= third_party/redmi2-firmware
# The GPU's firmware, once make fetch has put it here: without it the phone
# composes the screen on the CPU.
REDMI2_ADRENO_FIRMWARE ?= $(if $(wildcard build/redmi2-adreno/a300_pm4.fw),build/redmi2-adreno)
include tools/build.mk
export ARM32_PREFIX HOST_CC HOST_CFLAGS JOBS REDMI2_WIFI_FIRMWARE REDMI2_WIFI_PROFILE REDMI2_RTC_ANCHOR REDMI2_ADRENO_FIRMWARE

.PHONY: all help fetch engine run phone publish clean
all: build/resources-arm32/kernel.img build/host/mkfs build/host/fs-image

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

# Publishing is explicit and pushes only what make phone last wrote.
publish:
	$(PYTHON) tools/project.py publish

clean:
	$(PYTHON) tools/project.py clean
