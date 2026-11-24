.DELETE_ON_ERROR:
.DEFAULT_GOAL := mocha

CROSS_COMPILE ?= arm-linux-gnueabihf-
PYTHON ?= python3
QEMU ?= qemu-system-arm
# Exercise the same 225 MiB protected-RAM layout on QEMU virt.
QEMU_KCFLAGS ?= -DCFG_PRAM=0x38400
UBOOT_VERSION := 2025.10
UBOOT_ARCHIVE := u-boot_$(UBOOT_VERSION).orig.tar.bz2
UBOOT_SHA256 := b4f032848e56cc8f213ad59f9132c084dbbb632bc29176d024e58220e0efdf4a
UBOOT_URL ?= https://archive.ubuntu.com/ubuntu/pool/main/u/u-boot/$(UBOOT_ARCHIVE)

ROOT := $(CURDIR)
BUILD := $(ROOT)/build
ARCHIVE := $(ROOT)/.deps/sources/$(UBOOT_ARCHIVE)
UBOOT := $(BUILD)/u-boot-src
PRAM_PATCH := $(ROOT)/patches/0001-reserve-chainload-pram.patch
WINPROBE_PATCH := $(ROOT)/patches/0002-trace-winprobe.patch
ARM_RELOC_PATCH := $(ROOT)/patches/0003-arm-thumb-mov32-relocation.patch
WINPROBE_STACK_PATCH := $(ROOT)/patches/0004-winprobe-mocha-early-stack.patch
WINPROBE_HANDOFF_PATCH := $(ROOT)/patches/0005-winprobe-arm-handoff-diagnostics.patch
WINPROBE_FB32_PATCH := $(ROOT)/patches/0006-winprobe-mocha-bgra.patch
MOCHA := $(BUILD)/mocha
QEMU_ARM := $(BUILD)/qemu-arm
COMMON := $(ROOT)/uefi.config
CHAINLOAD := $(ROOT)/mocha-chainload.config
QEMU_CONFIG := $(ROOT)/qemu-arm.config
SD := $(BUILD)/sd
BOOTARM := $(SD)/EFI/BOOT/BOOTARM.EFI

WINPROBE_CONFIG := $(ROOT)/winprobe.config
WINPROBE_EXTRA_CONFIG ?=
WINPROBE_SOURCES := $(addprefix $(UBOOT)/cmd/,mocha_winprobe.c mocha_winacpi.c mocha_winacpi.h)
WINPROBE_OUT ?= $(BUILD)/winprobe
WINPROBE_CONTROL := $(BUILD)/winprobe-control.img.gz
WINPROBE_DISK ?= $(WINPROBE_CONTROL)
MTOOLS_DIR ?=

.PHONY: mocha qemu sd-files test winprobe-qemu winprobe-mocha

mocha: $(MOCHA)/u-boot-dtb.bin $(MOCHA)/mocha-probe.efi

qemu: $(QEMU_ARM)/u-boot.bin

sd-files: $(BOOTARM)

test: qemu sd-files
	$(PYTHON) qemu_smoke.py --qemu "$(QEMU)" --firmware "$(QEMU_ARM)/u-boot.bin" --media "$(SD)"

$(ARCHIVE):
	mkdir -p "$(@D)"
	curl --fail --location --retry 2 --output "$@" "$(UBOOT_URL)"

$(UBOOT)/.source: $(ARCHIVE) $(PRAM_PATCH) $(WINPROBE_PATCH) $(ARM_RELOC_PATCH) $(WINPROBE_STACK_PATCH) $(WINPROBE_HANDOFF_PATCH) $(WINPROBE_FB32_PATCH)
	printf '%s  %s\n' '$(UBOOT_SHA256)' "$<" | sha256sum -c -
	mkdir -p "$(@D)"
	tar -xf "$<" --strip-components=1 -C "$(@D)"
	patch --batch --forward --fuzz=0 -d "$(@D)" -p1 < "$(PRAM_PATCH)"
	patch --batch --forward --fuzz=0 -d "$(@D)" -p1 < "$(WINPROBE_PATCH)"
	patch --batch --forward --fuzz=0 -d "$(@D)" -p1 < "$(ARM_RELOC_PATCH)"
	patch --batch --forward --fuzz=0 -d "$(@D)" -p1 < "$(WINPROBE_STACK_PATCH)"
	patch --batch --forward --fuzz=0 -d "$(@D)" -p1 < "$(WINPROBE_HANDOFF_PATCH)"
	patch --batch --forward --fuzz=0 -d "$(@D)" -p1 < "$(WINPROBE_FB32_PATCH)"
	touch "$@"

# Reuse U-Boot's PE/COFF toolchain and embed the payload through bootefi hello.
$(UBOOT)/.probe: $(ROOT)/mocha_probe.c $(UBOOT)/.source
	cp "$<" "$(UBOOT)/lib/efi_loader/helloworld.c"
	touch "$@"

# $(1) = output directory, $(2) = upstream defconfig, $(3) = optional fragments.
define configure
	mkdir -p "$(1)"
	$(MAKE) -C "$(UBOOT)" O="$(1)" CROSS_COMPILE="$(CROSS_COMPILE)" $(2)
	"$(UBOOT)/scripts/kconfig/merge_config.sh" -m -O "$(1)" "$(1)/.config" "$(COMMON)" $(3)
	$(MAKE) -C "$(UBOOT)" O="$(1)" CROSS_COMPILE="$(CROSS_COMPILE)" olddefconfig
	$(PYTHON) "$(ROOT)/check_config.py" "$(1)/.config" "$(COMMON)" $(3)
	touch "$@"
endef

$(MOCHA)/.configured: $(UBOOT)/.source $(COMMON) $(CHAINLOAD) Makefile check_config.py
	$(call configure,$(MOCHA),mocha_defconfig,"$(CHAINLOAD)")

$(QEMU_ARM)/.configured: $(UBOOT)/.source $(COMMON) $(QEMU_CONFIG) Makefile check_config.py
	$(call configure,$(QEMU_ARM),qemu_arm_defconfig,"$(QEMU_CONFIG)")

$(MOCHA)/u-boot-dtb.bin: $(MOCHA)/.configured $(UBOOT)/.probe
	$(MAKE) -C "$(UBOOT)" O="$(MOCHA)" CROSS_COMPILE="$(CROSS_COMPILE)" u-boot-dtb.bin

$(MOCHA)/mocha-probe.efi: $(MOCHA)/u-boot-dtb.bin
	cp "$(MOCHA)/lib/efi_loader/helloworld.efi" "$@"

$(BOOTARM): $(MOCHA)/mocha-probe.efi
	mkdir -p "$(@D)"
	cp "$<" "$@"

$(QEMU_ARM)/u-boot.bin: $(QEMU_ARM)/.configured $(UBOOT)/.probe
	$(MAKE) -C "$(UBOOT)" O="$(QEMU_ARM)" CROSS_COMPILE="$(CROSS_COMPILE)" KCFLAGS="$(QEMU_KCFLAGS)" u-boot.bin

$(WINPROBE_CONTROL): $(BOOTARM) $(ROOT)/make_winprobe_disk.py
	$(PYTHON) "$(ROOT)/make_winprobe_disk.py" --media "$(SD)" --output "$@" $(if $(MTOOLS_DIR),--mtools-dir "$(MTOOLS_DIR)")

$(WINPROBE_SOURCES): $(UBOOT)/cmd/%: $(ROOT)/% $(UBOOT)/.source
	cp "$<" "$@"

$(WINPROBE_OUT)/mocha/.configured: $(UBOOT)/.source $(COMMON) $(CHAINLOAD) $(WINPROBE_CONFIG) $(WINPROBE_EXTRA_CONFIG) Makefile check_config.py
	$(call configure,$(@D),mocha_defconfig,"$(CHAINLOAD)" "$(WINPROBE_CONFIG)" $(if $(WINPROBE_EXTRA_CONFIG),"$(WINPROBE_EXTRA_CONFIG)"))

$(WINPROBE_OUT)/qemu/.configured: $(UBOOT)/.source $(COMMON) $(QEMU_CONFIG) $(WINPROBE_CONFIG) $(WINPROBE_EXTRA_CONFIG) Makefile check_config.py
	$(call configure,$(@D),qemu_arm_defconfig,"$(QEMU_CONFIG)" "$(WINPROBE_CONFIG)" $(if $(WINPROBE_EXTRA_CONFIG),"$(WINPROBE_EXTRA_CONFIG)"))

# Keep each build's embedded payload independent. Kbuild tracks changes to it.
define stage_winprobe
	mkdir -p "$(1)/cmd"
	cmp -s "$(WINPROBE_DISK)" "$(1)/cmd/mocha_winprobe_disk.gz" || cp "$(WINPROBE_DISK)" "$(1)/cmd/mocha_winprobe_disk.gz"
endef

winprobe-qemu: $(WINPROBE_OUT)/qemu/.configured $(UBOOT)/.probe $(WINPROBE_SOURCES) $(WINPROBE_DISK)
	$(call stage_winprobe,$(WINPROBE_OUT)/qemu)
	$(MAKE) -C "$(UBOOT)" O="$(WINPROBE_OUT)/qemu" CROSS_COMPILE="$(CROSS_COMPILE)" KCFLAGS="$(QEMU_KCFLAGS)" u-boot.bin

winprobe-mocha: $(WINPROBE_OUT)/mocha/.configured $(UBOOT)/.probe $(WINPROBE_SOURCES) $(WINPROBE_DISK)
	$(call stage_winprobe,$(WINPROBE_OUT)/mocha)
	$(MAKE) -C "$(UBOOT)" O="$(WINPROBE_OUT)/mocha" CROSS_COMPILE="$(CROSS_COMPILE)" u-boot-dtb.bin
