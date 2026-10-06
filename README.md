# AxysKernel

AxysKernel is an experimental, original x86-64 kernel. Its current focus is a
small bootable system with isolated ring-3 processes, a VFS, persistence, and
careful validation of firmware and executable input. Linux, BSD, and Unix are
inspirations; this project does not aim to be a Linux clone.

## Build and validate

The intended build environment is Arch Linux/WSL2 with GCC, GNU binutils,
GRUB, xorriso, and QEMU installed:

```sh
make kernel
make iso
make test
make check
make check-highmem
make check-nvme
make check-ahci
make check-usb
make check-usb-hub
make check-usb-storage
make check-usb-storage-hub
make check-net
```

`make check` runs the host regression tests and drives a headless QEMU boot,
shell, shutdown, and storage-persistence scenario. It validates the devices
configured by that test only; it is not a hardware compatibility guarantee.
`make check-highmem` boots a 5 GiB QEMU machine with usable RAM above 4 GiB
and exercises physical allocation, process startup, shell input, and storage
persistence.
`make check-nvme` exercises a QEMU NVMe controller through identify, read,
write, flush, and reboot persistence.
`make check-ahci` places the test disk on the highest ICH9 AHCI port and checks
the selected backend and reboot persistence.
`make check-usb` boots with a USB keyboard behind xHCI and injects keystrokes;
`make check-usb-hub` repeats it behind a USB hub, and `make check-usb-storage`
and `make check-usb-storage-hub` use a USB mass-storage disk as the system
disk on a root port and behind a hub, checking persistence across two boots.
`make check-net` obtains a DHCPv4 lease and pings the gateway over the
controller.

## Current platform

- x86-64, entered through a Multiboot2 loader such as GRUB.
- Long mode and NX are required. CPU hardening features are enabled only when
  CPUID reports them.
- Firmware-reported usable RAM is identity-mapped sparsely above the static
  4 GiB bootstrap map and managed up to 512 GiB (or the CPU's physical-address
  width, if lower). High ACPI tables are not generally mapped; PCI BARs are
  mapped only on demand when the firmware marks their pages reserved.
- PCI BAR MMIO is mapped uncached only when the firmware map marks the requested
  pages reserved; device DMA/IOMMU policy is still limited to the current drivers.
- Console output is serial and VGA text. Keyboard input is serial, a PS/2
  set-1 keyboard through the i8042 controller, or a USB boot keyboard through
  xHCI, directly or behind a USB hub.
- USB support is an polled xHCI host controller with hub traversal, boot
  keyboards, and bulk-only mass storage usable as the system disk. Other USB
  host controllers, HID classes, and audio are absent.
- Storage support includes a polled PCI NVMe driver for a 512-byte-LBA
  namespace, PCI AHCI, primary-channel ATA PIO, and USB bulk-only mass storage.
  All four backends are tested in QEMU, including reboot persistence.
- Ethernet support is a polled Intel 82540EM-compatible controller with raw
  user-space networking: `ping` speaks ARP/ICMP and `dhcp` performs a full
  DHCPv4 handshake. There is no kernel socket layer, so TCP/UDP, DNS, Wi-Fi,
  audio, SMP, and vendor GPU acceleration are not implemented.
- DDR initialization belongs to motherboard firmware and the CPU memory
  controller. The kernel consumes the firmware memory map; it does not train
  DDR3, DDR4, or DDR5 memory.

See [the platform support matrix](docs/PLATFORM_SUPPORT.md) for details and
[the executable ABI](docs/EXECUTABLE_ABI.md) for the supported user binary
format and compiler constraints.

## Source layout

- `arch/x86_64/`: early boot, interrupt entry, context switching, and syscall
  assembly.
- `kernel/`: memory management, process isolation, devices, firmware, and VFS.
- `include/axys/`: kernel interfaces and fixed-width types.
- `user/`: small freestanding user programs and the current syscall wrapper.
- `tests/`: host-side parser and subsystem regression tests.
- `tools/`: QEMU integration test and interrupt-stub generators.

This remains an early kernel, not a general-purpose operating system. The
support matrix and audit report are the source of truth for tested behavior
and known gaps.
