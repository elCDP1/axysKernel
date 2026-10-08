ifeq ($(OS),Windows_NT)
MKDIR = powershell -NoProfile -Command "New-Item -ItemType Directory -Force -Path '$(dir $@)' | Out-Null"
CLEAN = powershell -NoProfile -Command "if (Test-Path -LiteralPath 'build') { Remove-Item -LiteralPath 'build' -Recurse -Force }"
CAT = type
else
SHELL := /bin/sh
MKDIR = mkdir -p $(dir $@)
CLEAN = rm -rf build
CAT = cat
endif

ARCH ?= x86_64
CC ?= gcc
LD ?= ld
OBJCOPY ?= objcopy

CONFIG ?= config/axys_defconfig
-include $(CONFIG)
CONFIG_AXYS_CONSOLE_SERIAL ?= y
CONFIG_AXYS_CONSOLE_VGA ?= y

config_to_bool = $(if $(filter y yes 1,$($(1))),1,0)
KERNEL_CONFIG_DEFINES := -DCONFIG_AXYS_CONSOLE_SERIAL=$(call config_to_bool,CONFIG_AXYS_CONSOLE_SERIAL) \
                         -DCONFIG_AXYS_CONSOLE_VGA=$(call config_to_bool,CONFIG_AXYS_CONSOLE_VGA)
KERNEL_CFLAGS := -std=c17 -O2 -g -ffreestanding -fno-builtin -fstack-protector-strong -mstack-protector-guard=global -fno-pic -fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident -ffunction-sections -fdata-sections -mno-red-zone -mcmodel=large -mno-80387 -mno-sse -mno-sse2 -mno-mmx -mgeneral-regs-only -Wall -Wextra -Werror -Iinclude -Iarch/$(ARCH) $(KERNEL_CONFIG_DEFINES)
KERNEL_ASFLAGS := -ffreestanding -fno-stack-protector -Iinclude -Iarch/$(ARCH) $(KERNEL_CONFIG_DEFINES)
KERNEL_LDFLAGS := -n -T scripts/linker.ld --gc-sections --build-id=none -z max-page-size=0x1000

KERNEL_C_SOURCES := $(wildcard kernel/*.c lib/*.c arch/$(ARCH)/*.c)
KERNEL_AS_SOURCES := $(wildcard arch/$(ARCH)/*.S)
# User programs (ELF64 static-PIE) embedded into the kernel image as the initial
# file system; each is installed into the VFS at boot. Format: name:/install/path
USER_PROGS := init:/sbin/init hello:/bin/hello crash:/bin/crash heap:/bin/heap fileio:/bin/fileio perms:/bin/perms probe:/bin/probe fuzz:/bin/fuzz ping:/bin/ping dhcp:/bin/dhcp
USER_ELFS := $(foreach p,$(USER_PROGS),build/user/$(word 1,$(subst :, ,$(p))).elf)
USER_CFLAGS := -std=c17 -O2 -ffreestanding -fno-builtin -fno-stack-protector -fPIE -fno-asynchronous-unwind-tables -mno-red-zone -mgeneral-regs-only -Wall -Wextra -Werror -Iuser
USER_LDFLAGS := -pie --no-dynamic-linker -z max-page-size=0x1000 -z noseparate-code -z norelro --build-id=none -e _start

KERNEL_OBJECTS := $(patsubst %.c,build/%.o,$(KERNEL_C_SOURCES)) $(patsubst %.S,build/%.o,$(KERNEL_AS_SOURCES)) build/initrd.o

HOST_CC ?= gcc
HOST_CFLAGS := -std=c17 -O2 -Wall -Wextra -Werror -Iinclude -DAXYS_HOST_TEST
GRUB_MKRESCUE ?= grub-mkrescue
QEMU ?= qemu-system-x86_64
HOST_STRING_TEST_SOURCES := tests/test_string.c tests/console_stub.c lib/string.c lib/printf.c
HOST_MB2_TEST_SOURCES := tests/test_multiboot2.c lib/string.c kernel/multiboot2.c
HOST_FRAME_TEST_SOURCES := tests/test_interrupt_frame.c
HOST_EXCEPTION_TEST_SOURCES := tests/test_exceptions.c tests/exception_stub.c tests/console_stub.c kernel/exceptions.c kernel/idt.c lib/string.c lib/printf.c
HOST_VFS_TEST_SOURCES := tests/test_vfs.c tests/vfs_stub.c kernel/vfs.c lib/string.c lib/printf.c
HOST_STRING_TEST_OBJECTS := $(patsubst %.c,build/host/%.o,$(HOST_STRING_TEST_SOURCES))
HOST_MB2_TEST_OBJECTS := $(patsubst %.c,build/host/%.o,$(HOST_MB2_TEST_SOURCES))
HOST_FRAME_TEST_OBJECTS := $(patsubst %.c,build/host/%.o,$(HOST_FRAME_TEST_SOURCES))
HOST_EXCEPTION_TEST_OBJECTS := $(patsubst %.c,build/host/%.o,$(HOST_EXCEPTION_TEST_SOURCES))
HOST_VFS_TEST_OBJECTS := $(patsubst %.c,build/host/%.o,$(HOST_VFS_TEST_SOURCES))
HOST_PERSIST_TEST_SOURCES := tests/test_persist.c tests/ramdisk.c tests/vfs_stub.c kernel/persist.c kernel/vfs.c lib/string.c lib/printf.c
HOST_ACPI_TEST_SOURCES := tests/test_acpi.c kernel/acpi_aml.c
HOST_ELF_TEST_SOURCES := tests/test_elf.c kernel/elf.c lib/string.c
HOST_PATH_TEST_SOURCES := tests/test_path.c lib/string.c lib/path.c
HOST_FUZZ_SOURCES := tests/fuzz.c tests/vfs_stub.c kernel/vfs.c kernel/elf.c lib/string.c lib/printf.c lib/path.c
HOST_PERSIST_TEST_OBJECTS := $(patsubst %.c,build/host/%.o,$(HOST_PERSIST_TEST_SOURCES))
HOST_ACPI_TEST_OBJECTS := $(patsubst %.c,build/host/%.o,$(HOST_ACPI_TEST_SOURCES))
HOST_ELF_TEST_OBJECTS := $(patsubst %.c,build/host/%.o,$(HOST_ELF_TEST_SOURCES))
HOST_PATH_TEST_OBJECTS := $(patsubst %.c,build/host/%.o,$(HOST_PATH_TEST_SOURCES))

.PHONY: all objects kernel test clean config-print iso run check check-highmem check-nvme check-ahci fuzz check-fuzz check-usb check-usb-hub check-usb-storage check-usb-storage-hub check-usb-hotplug check-pci check-drivers check-net

all: kernel

kernel: build/axys.elf

# Regenerate objects if any header changed, not just the .c file itself.
-include $(KERNEL_OBJECTS:.o=.d) $(HOST_STRING_TEST_OBJECTS:.o=.d) $(HOST_MB2_TEST_OBJECTS:.o=.d) $(HOST_FRAME_TEST_OBJECTS:.o=.d) $(HOST_EXCEPTION_TEST_OBJECTS:.o=.d) $(HOST_VFS_TEST_OBJECTS:.o=.d) $(HOST_PERSIST_TEST_OBJECTS:.o=.d) $(HOST_ACPI_TEST_OBJECTS:.o=.d) $(HOST_ELF_TEST_OBJECTS:.o=.d) $(HOST_PATH_TEST_OBJECTS:.o=.d)

build/axys.elf: $(KERNEL_OBJECTS) scripts/linker.ld
	$(LD) $(KERNEL_LDFLAGS) -o $@ $(KERNEL_OBJECTS)

objects: $(KERNEL_OBJECTS)

build/user/%.o: user/%.c user/axys.h
	@$(MKDIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/crt0.o: user/crt0.S
	@$(MKDIR)
	$(CC) -c $< -o $@

build/user/%.elf: build/user/%.o build/user/lib.o build/user/crt0.o
	$(LD) $(USER_LDFLAGS) -o $@ build/user/crt0.o $< build/user/lib.o

# The initrd is a generated assembly file that .incbin's every program ELF and
# exports a table of (path, start, end) triples for kernel/initrd.c.
build/initrd.S: Makefile
	@$(MKDIR)
	@echo '.section .rodata' > $@
	@echo '.balign 16' >> $@
	@i=0; for p in $(USER_PROGS); do n=$${p%%:*}; d=$${p#*:}; \
	  echo ".balign 16" >> $@; echo "initrd_start_$$i: .incbin \"build/user/$$n.elf\"" >> $@; \
	  echo "initrd_end_$$i:" >> $@; echo "initrd_path_$$i: .asciz \"$$d\"" >> $@; i=$$((i+1)); done
	@echo '.balign 8' >> $@
	@echo '.global axys_initrd_table' >> $@
	@echo 'axys_initrd_table:' >> $@
	@i=0; for p in $(USER_PROGS); do echo "  .quad initrd_path_$$i, initrd_start_$$i, initrd_end_$$i" >> $@; i=$$((i+1)); done
	@echo '  .quad 0, 0, 0' >> $@
	@echo '.section .note.GNU-stack,"",@progbits' >> $@

build/initrd.o: build/initrd.S $(USER_ELFS)
	@$(MKDIR)
	$(CC) $(KERNEL_ASFLAGS) -c $< -o $@

build/%.o: %.c
	@$(MKDIR)
	$(CC) $(KERNEL_CFLAGS) -MMD -MP -c $< -o $@

build/%.o: %.S
	@$(MKDIR)
	$(CC) $(KERNEL_ASFLAGS) -MMD -MP -c $< -o $@

test: build/test_string.exe build/test_multiboot2.exe build/test_interrupt_frame.exe build/test_exceptions.exe build/test_vfs.exe build/test_persist.exe build/test_acpi.exe build/test_elf.exe build/test_path.exe build/test_heap.exe build/test_pci.exe build/test_usb.exe build/test_usb_desc.exe
	./build/test_string.exe
	./build/test_multiboot2.exe
	./build/test_interrupt_frame.exe
	./build/test_exceptions.exe
	./build/test_vfs.exe
	./build/test_persist.exe
	./build/test_acpi.exe
	./build/test_elf.exe
	./build/test_path.exe
	./build/test_heap.exe
	./build/test_usb_desc.exe
	./build/test_pci.exe
	./build/test_usb.exe

build/test_string.exe: $(HOST_STRING_TEST_OBJECTS)
	@$(MKDIR)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(HOST_STRING_TEST_OBJECTS)

build/test_multiboot2.exe: $(HOST_MB2_TEST_OBJECTS)
	@$(MKDIR)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(HOST_MB2_TEST_OBJECTS)

build/test_interrupt_frame.exe: $(HOST_FRAME_TEST_OBJECTS)
	@$(MKDIR)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(HOST_FRAME_TEST_OBJECTS)

build/test_exceptions.exe: $(HOST_EXCEPTION_TEST_OBJECTS)
	@$(MKDIR)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(HOST_EXCEPTION_TEST_OBJECTS)

build/test_persist.exe: $(HOST_PERSIST_TEST_OBJECTS)
	@$(MKDIR)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(HOST_PERSIST_TEST_OBJECTS)

build/test_acpi.exe: $(HOST_ACPI_TEST_OBJECTS)
	@$(MKDIR)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(HOST_ACPI_TEST_OBJECTS)

build/test_elf.exe: $(HOST_ELF_TEST_OBJECTS)
	@$(MKDIR)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(HOST_ELF_TEST_OBJECTS)

build/test_vfs.exe: $(HOST_VFS_TEST_OBJECTS)
	@$(MKDIR)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(HOST_VFS_TEST_OBJECTS)

build/test_usb_desc.exe: tests/test_usb_desc.c kernel/usb_desc.c
	@$(MKDIR)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ tests/test_usb_desc.c kernel/usb_desc.c

build/test_heap.exe: tests/test_heap.c kernel/heap.c lib/string.c
	@$(MKDIR)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ tests/test_heap.c lib/string.c

build/test_pci.exe: tests/test_pci.c kernel/pci.c kernel/dma.c lib/string.c lib/printf.c tests/console_stub.c
	@$(MKDIR)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ tests/test_pci.c lib/string.c lib/printf.c tests/console_stub.c

build/test_usb.exe: tests/test_usb.c kernel/usb_hid.c
	@$(MKDIR)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ tests/test_usb.c kernel/usb_hid.c

build/test_path.exe: $(HOST_PATH_TEST_OBJECTS)
	@$(MKDIR)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(HOST_PATH_TEST_OBJECTS)

# Host fuzzer: always sanitized, never -Werror (bounded strncpy/strncat
# patterns warn under -Wstringop-truncation by design here).
FUZZ_CC ?= gcc
FUZZ_CFLAGS := -std=c17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Iinclude -DAXYS_HOST_TEST
FUZZ_SEED ?= 1
FUZZ_ITERS ?= 4000

fuzz: build/fuzz.exe
	./build/fuzz.exe $(FUZZ_SEED) $(FUZZ_ITERS)

build/fuzz.exe: $(HOST_FUZZ_SOURCES)
	@$(MKDIR)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -o $@ $(HOST_FUZZ_SOURCES)

check-fuzz: iso
	@sh tools/qemu-fuzz.sh

# USB keyboard through xHCI: enumeration at boot plus injected keystrokes.
check-usb: iso
	@python3 tools/qemu-usb.py

# Same keyboard behind a USB hub, which also exercises route-string handling.
# Plug and unplug a USB keyboard many times through QMP (directly and behind a
# hub): every cycle must be detected, deliver keys and be noticed on removal,
# and physical frames, kernel heap and VFS nodes must return to the baseline.
check-usb-hotplug: iso
	@python3 tools/qemu-usb-hotplug.py --cycles 12
	@python3 tools/qemu-usb-hotplug.py --cycles 6 --topology hub

check-usb-hub: iso
	@TOPOLOGY=hub python3 tools/qemu-usb.py

# USB mass storage (BOT) with persistence across two boots, on a root port.
check-usb-storage: test iso
	@EXPECT_USB=1 DISK_IF=if=none DISK_DEV='-device qemu-xhci,id=xhci -device usb-storage,drive=d0,bus=xhci.0' sh tools/qemu-test.sh

# The same BOT disk behind a USB hub, which also exercises route strings for
# bulk transfers.
check-usb-storage-hub: test iso
	@EXPECT_USB=1 DISK_IF=if=none DISK_DEV='-device qemu-xhci,id=xhci -device usb-hub,bus=xhci.0 -device usb-storage,drive=d0,port=1.1' sh tools/qemu-test.sh

# DHCPv4 lease over e1000 followed by an ICMP ping to the SLIRP gateway.
check-net: test iso
	@EXPECT_NET=1 sh tools/qemu-test.sh

build/host/%.o: %.c
	@$(MKDIR)
	$(HOST_CC) $(HOST_CFLAGS) -MMD -MP -c $< -o $@

config-print:
	@echo "configuration: $(CONFIG)"
	@$(CAT) $(CONFIG)

iso: build/axys.iso

build/axys.iso: build/axys.elf scripts/grub.cfg
	mkdir -p build/isodir/boot/grub
	cp build/axys.elf build/isodir/boot/axys.elf
	cp scripts/grub.cfg build/isodir/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ build/isodir 2>/dev/null

run: build/axys.iso
	$(QEMU) -cdrom build/axys.iso -m 128M -display none -serial stdio -no-reboot

clean:
	$(CLEAN)

# Host tests plus a headless QEMU boot that must reach every milestone.
check: test iso
	@sh tools/qemu-test.sh

# Exercise expanded PMM metadata and sparse identity mapping across 4 GiB.
check-highmem: test iso
	@EXPECT_HIGHMEM=1 MEM=5G sh tools/qemu-test.sh -machine pc,max-ram-below-4g=4G

# Exercise PCI NVMe identify, read/write, flush, and persistence across reboot.
check-nvme: test iso
	@EXPECT_NVME=1 DISK_IF=if=none DISK_DEV='-device nvme,id=nvme0,serial=axys-test-0001 -device nvme-ns,drive=d0,bus=nvme0,nsid=1' sh tools/qemu-test.sh

# Exercise PCI BAR decoding and AHCI persistence on the highest ICH9 port.
check-ahci: test iso
	@EXPECT_AHCI=1 DISK_IF=if=none DISK_DEV='-device ich9-ahci,id=s0 -device ide-hd,drive=d0,bus=s0.5' sh tools/qemu-test.sh

# Compare the kernel's /proc/pci with QEMU's own `info pci` on an i440FX board
# and on a q35 board with xHCI, NVMe, virtio devices and a PCIe root port.
check-pci: iso
	python3 tools/qemu-pci-check.py
	python3 tools/qemu-pci-check.py -- -machine q35 -device qemu-xhci \
		-device nvme,drive=nv,serial=axys -drive id=nv,if=none,file=@DISK2@,format=raw \
		-device virtio-net-pci -device virtio-rng-pci \
		-device pcie-root-port,id=rp1,chassis=1,slot=1 -device qemu-xhci,bus=rp1 -device usb-ehci

# Every driver scenario in one go (several minutes: each boot test also reboots).
check-drivers: check-pci check-usb check-usb-hub check-usb-hotplug check-usb-storage check-usb-storage-hub check-ahci check-nvme check-net
