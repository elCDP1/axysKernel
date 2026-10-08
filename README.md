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
make check-usb-hotplug
make check-pci
make check-net
make check-drivers   # every driver scenario above in one run
make fuzz            # ASan/UBSan host fuzzing (FUZZ_SEED, FUZZ_ITERS)
make check-fuzz      # in-guest syscall fuzzing under QEMU (SEED, ITERS, QEMU_EXTRA)
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
`make check-usb-hotplug` plugs and unplugs a USB keyboard through QMP, directly
and behind a hub, and requires that every cycle delivers keys and that free
frames, kernel heap and VFS nodes return to their starting values.
`make check-pci` compares the kernel's `/proc/pci` (BAR kind, address and size
of every function) with QEMU's own `info pci` on an i440FX and a q35 machine.
`make check-net` obtains a DHCPv4 lease and pings the gateway over the
controller. `make check-highmem` needs a host able to give QEMU 5 GiB.

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
- USB support is a polled xHCI host controller with hub traversal, boot
  keyboards, and bulk-only mass storage usable as the system disk. Devices can
  be hot-plugged and removed (root ports and hub ports): slots, rings and
  contexts are released and a re-plugged device enumerates again. A mass-storage
  disk that disappears makes I/O fail with `ENODEV`; a replacement disk is not
  adopted at run time. Other USB host controllers, HID classes, and audio are
  absent.
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

## Shell

`/sbin/init` is an interactive shell with embedded-linux builtins plus
`/bin` programs; `help <cmd>` prints usage inside the guest. Paths may be
absolute or relative to the working directory (`cd`, `pwd`).

```text
files & dirs:  ls cat echo mkdir rm mv rmtree touch cp head tail wc grep
               stat du find cut uniq tr strings hexdump cmp basename dirname
               pwd cd tee clear
system & users: meminfo free uptime date random pid id su chmod chown sync
               sleep kill hostname uname whoami which seq time run poweroff
               halt shutdown reboot exit
processes & drivers: ps dmesg lscpu lsblk lspci lsusb sysinfo df ip ifconfig
               (/bin has ping, dhcp)
/bin programs: hello crash heap fileio perms probe fuzz ping dhcp
```

Commands accept several paths (`cat`, `rm`, `touch`, `mkdir`, `ls`), `cp`
/`chmod`/`chown` take `-r`/`-R`, `cat`/`head`/`tail`/`grep`/`cut` stream
without loading whole files, and `;` chains commands on one line (no
quoting, no pipes yet).

Driver commands read live state, not canned text: `ip` uses the `NET_STAT`
syscall (e1000 link/MAC/counters), `free` uses `MEMINFO`, `ps` uses the new
`PS` syscall (pid, ppid, uid, state, name), `dmesg` streams the retained
console log via `DMESG`, and `lscpu`, `lspci`, `lsusb`, `lsblk`, `sysinfo`,
`df`, `uname` read the `/proc/cpu`, `/proc/pci`, `/proc/usb`, `/proc/disk`,
`/proc/net`, `/proc/acpi` and `/proc/version` snapshots the kernel publishes
at boot (RAM-only, never persisted). Deliberately absent (no kernel support
yet): `mount`, `ln`, `tar`, `vi`, `top`, pipes.

## Source layout

- `arch/x86_64/`: early boot, interrupt entry, context switching, and syscall
  assembly.
- `kernel/`: memory management, process isolation, devices, firmware, and VFS.
- `include/axys/`: kernel interfaces and fixed-width types.
- `user/`: small freestanding user programs and the current syscall wrapper.
- `tests/`: host-side parser and subsystem regression tests.
- `tools/`: QEMU integration, fuzz, USB and PCI test drivers, and
  interrupt-stub generators.

This remains an early kernel, not a general-purpose operating system. The
support matrix and audit report are the source of truth for tested behavior
and known gaps.
