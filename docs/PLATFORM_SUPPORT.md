# Platform support and hardware roadmap

This document distinguishes implemented hardware paths from planned ones.
“Detected” means firmware or PCI enumeration found a device; it does not mean
the kernel has a functional driver for it.

## Current support

| Area | Implemented and exercised | Important limits |
|---|---|---|
| CPU | x86-64 long mode; CPUID feature discovery; NX is mandatory; SMEP is conditional | No SMP/AP startup, microcode loading, power/performance states, or 32-bit CPUs |
| Firmware | Multiboot2 entry; ACPI RSDP/RSDT/XSDT/FADT parsing; conventional PM1 poweroff | No general AML interpreter, hardware-reduced ACPI sleep, UEFI runtime services, or broad firmware matrix |
| RAM | Multiboot2 memory map; 4 KiB frame allocator; sparse identity map and runtime-sized metadata up to 512 GiB (also limited by CPU physical-address width); QEMU-tested across 4 GiB | No NUMA, hotplug, ECC reporting, general high ACPI-table mapping, or memory-controller driver |
| Display | VGA text console; bootloader framebuffer metadata is parsed and reserved | No framebuffer console: GRUB's multiboot2 loader hands over an EGA-text framebuffer tag even with `gfxpayload=keep`, so a console would need a bootloader change or kernel-side VBE modesetting. No Intel/AMD/NVIDIA GPU driver |
| USB | xHCI host controller (PCI 0c/03/0x30, polled command/event rings, port reset, full/low/high-speed address/configure); USB hubs with route-string routing up to 5 levels deep; boot keyboards (HID); bulk-only mass storage (512-byte sectors, READ/WRITE(10), TEST UNIT READY) usable as the system disk; /proc/pci, /proc/usb, /proc/disk, /proc/net, /proc/acpi, /proc/version inventories; `make check-usb`, `check-usb-hub`, `check-usb-storage`, `check-usb-storage-hub` | Polled only (no event-interrupt path), no isochronous endpoints, no HID beyond boot keyboards, no SCSI pass-through or multiple LUNs; devices are removed and re-enumerated at run time (root ports every 8 ms, hub ports on a ~2 s scan, nothing above 5 levels deep) but a mass-storage disk is adopted only at boot: if it is pulled, I/O fails with ENODEV and a replacement is not attached; no UHCI/OHCI/EHCI |
| Keyboard | PS/2 set-1 through i8042; USB HID boot keyboards through xHCI polling, directly or behind hubs; serial input | Native USB host controllers other than xHCI are absent; USB HID mice and non-boot HID reports are not supported |
| Shell | Busybox-style builtins (files incl. `cut`/`uniq`/`tr`/`strings`/`hexdump`/`cmp`, users, `ps`/`dmesg`/`lscpu`/`lsblk`/`lspci`/`lsusb`/`sysinfo`/`df`/`ip` over live kernel state, `help <cmd>`, `;` chaining, multi-path args, `cp -r`, `chmod -R`), relative paths with `cd`/`pwd`, /bin programs (ping, dhcp, probe, ...) | No pipes, quoting, symlinks, `mount`, `ln`, `tar`, `vi`, `top` |
| Storage | PCI NVMe polling (first controller, namespace 1, 512-byte LBAs, one outstanding command); primary-channel ATA PIO (LBA28); PCI AHCI polling; USB BOT mass storage; QEMU tests for NVMe/AHCI/IDE/USB persistence across two boots | No secondary ATA channel, DMA for ATA PIO, NCQ, interrupt-driven queues, USB device removal, other NVMe namespaces/controllers, or RAID |
| Network | Intel 82540EM-compatible PCI Ethernet, polled (TX/RX rings, link detection, factory MAC); ARP + ICMP echo in /bin/ping and DHCPv4 (DISCOVER/OFFER/REQUEST/ACK, option 3 router) in /bin/dhcp over QEMU user-network defaults; `make check-net` leases 10.0.2.15 and pings the gateway | Raw Ethernet in user space only: no kernel socket layer, TCP/UDP, IPv6, DNS, Wi-Fi, VLANs, or other NIC models (virtio-net included) |
| Audio | None | No HDA, AC'97, USB audio, or audio stack |

The QEMU integration test covers its configured x86-64 firmware, serial
console, serial-driven shell input, and IDE/AHCI/USB disks. A separate manual QEMU
check has exercised the PS/2 right-Ctrl release path. These tests do not
validate physical machines or unrelated controller models.

`make check-highmem` runs `MEM=5G tools/qemu-test.sh -machine
pc,max-ram-below-4g=4G` to place usable RAM above the 4 GiB boundary. It checks
the detected physical range and runs the same process, shell, and disk
persistence scenarios as the standard integration test.

PCI BAR registers used by the current AHCI, NVMe, xHCI, and Ethernet drivers are
mapped uncached after checking that the firmware memory map does not report the
range as usable RAM. The page tables split the affected 2 MiB identity pages into
4 KiB entries when required. DMA buffers come from a dedicated allocator
(`kernel/dma.c`) that returns cache-coherent, naturally aligned, zeroed
contiguous blocks below the 4 GiB boundary; there is no general IOMMU setup or
MSI/MSI-X routing.

## Memory-generation boundary

DDR3/DDR4/DDR5 initialization, timings, and training are performed by the
platform firmware and integrated memory controller before the bootloader
starts the kernel. AxysKernel should consume the firmware memory map and
present RAM independently of the DIMM generation. It currently maps usable
RAM above 4 GiB sparsely, up to 512 GiB or the CPU's physical-address width.
This does not define a general high-MMIO/ACPI mapping policy, NUMA, hotplug, or
device DMA address handling; adding a “DDR driver” would not solve those limits.

## Driver implementation order

1. Replace the identity map with a managed direct map and explicit virtual
   regions; define mapping and ownership for high ACPI tables, MMIO, and
   physical addresses above the current 512 GiB ceiling.
2. Add ACPI MADT, IOAPIC, local APIC, and SMP bring-up before enabling
   interrupt-driven multi-queue devices (USB, NVMe, Ethernet all poll today).
3. Complete PCI resource ownership and hotplug, then add a general IOMMU
   policy, MSI/MSI-X routing, and a device/driver lifecycle.
4. USB: isochronous endpoints, HID beyond boot keyboards, UHCI/OHCI/EHCI
   alongside xHCI, and run-time adoption of a hot-plugged mass-storage disk
   (needs a block-device hot-plug policy in the disk layer).
5. Networking: move the packet path into the kernel with a socket layer
   (UDP/TCP, IPv4 routing, DNS) before attempting Wi-Fi, whose driver, MAC
   layer, and power save need that base.
6. Treat GPU support as separate vendor/device families. A framebuffer console
   first requires a bootloader that hands over a linear framebuffer tag (GRUB's
   multiboot2 loader does not for QEMU VBE modes) or kernel-side VBE
   modesetting; 3D acceleration requires vendor-specific command submission,
   memory management, and extensive validation.

The project will publish concrete device IDs and tested QEMU models as drivers
land. “All hardware” is not a finite testable support target.

## Driver audit notes (2026-10-05)

Existing drivers were re-read against the AHCI 1.3, NVMe 1.x/2.0, ATA-ATAPI,
PCI 3.0 and 8042 behaviors and against the Linux equivalents (`ahci`,
`nvme`, `ata_piix`, `i8042`). Findings fixed:

- AHCI `PxCMD` bits were misnamed (`FISRE`/`FISRX`/`CRQ`): bit 3 is CLO,
  bit 4 is FRE, bit 28 is ICC. The run value is now `ST|FRE|ICC_ACTIVE`
  (as in Linux `PORT_CMD_*`), the per-command ICC set/clear no-op is gone,
  `PxSERR` clears use all-ones (W1C), and a failed command COMRESETs the
  port instead of leaving `PxCI` wedged.
- AHCI `IDENTIFY` gated LBA48 on word 100 bit 10 with a 12-bit mask; the
  capability bit is word 83 bit 10 and words 100-103 are used whole.
  Disks under 128 GiB never noticed; larger ones could lose capacity.
- NVMe serialized commands under an irqsave spinlock across polls of up to
  120 s bounded by the IRQ0 tick: a dead controller would freeze the clock
  and hang forever with interrupts off. Now a busy-flag + yield, AHCI-style.
- NVMe refused major version 2.x although the used subset (admin queues,
  Identify, Read/Write/Flush over PRPs) is unchanged in 2.0. Now accepts
  1.x and 2.x.
- PCI BAR decode could mistake an I/O BAR with type bits 00 for a 32-bit
  memory BAR at a truncated address; I/O BARs are now skipped by bit 0 first.
- ATA PIO now checks `ERR|DF` after each transfer and before `FLUSH`
  (faults used to be confirmed as success); `disk_read`/`disk_write`
  reject `NULL` buffers.

Deliberately not added: UHCI/OHCI/EHCI, USB audio and isochronous endpoints,
HID classes beyond boot keyboards, virtio-net, Wi-Fi (802.11 + regulatory +
firmware), GPU modesetting/acceleration, SMP, audio. Each is thousands of lines
plus a stack the kernel does not have yet (MSI/MSI-X and hotplug lifecycle for
USB and NICs; a kernel network stack for Wi-Fi; per-vendor command submission
and memory management for GPUs). A framebuffer console was implemented and then
removed because GRUB's multiboot2 loader reports only an EGA-text framebuffer
tag for the VBE modes it sets, leaving it untestable here; shipping it would
contradict the audit above, so it remains a roadmap item, not a shipped stub.

## Audit notes for the driver additions (2026-10-06)

PCI, DMA, xHCI/HID, USB hubs, USB mass storage, Ethernet and DHCPv4 were added
against their specifications (PCI 3.0, xHCI 1.2, USB 2.0/3.2, SCSI SBC-3,
Intel 8254x, RFC 2131/2132) and the Linux equivalents (`xhci-hcd`,
`usb-storage`, `e1000e`, `ping`, `udhcpc`). Bugs found and fixed while testing:

- PCI: 64-bit BARs reported only their low half, a zeroed `command` write
  silently cleared W1C status bits, and a zero raw read was treated as a read
  failure; memory BARs below 1 MiB are no longer decoded as I/O.
- xHCI: the input context addressed the wrong slot offset, Setup TRTs had IN
  and OUT swapped, the doorbell used the wrong stream, `Configure` was called
  with A1 set, polling ran with interrupts enabled, and HID completions
  arriving during hub control transfers were dropped instead of rearmed.
- USB mass storage: CBW and CSW signatures were compared byte-swapped, the
  reset/ready protocol constants were wrong, and no Bulk-Only Reset preceded
  the first command (sticky BOT reset state).
- Ethernet: the RDLEN/TDLEN fields collided with the ring base addresses and
  the TX wait polled with interrupts disabled.
- Syscalls: `NET_RECV` dequeued a frame before validating the user pointer, so
  a bad destination returned `EAGAIN` and dropped the packet.

## Audit notes: driver review (2026-10-07)

A second review of the driver commit, driven by QEMU stress tests (400-1000 key
bursts, 20 hot-plug cycles, fuzzing with live USB devices) found and fixed:

- xHCI event ring: a Link TRB had been planted in the last slot and that slot
  skipped. An event ring has no Link TRBs (xHCI 4.9.4): the controller walks
  the segment table and, with one 256-entry segment, wraps on its own. The
  skipped slot lost one event per wrap and desynchronised the cycle bit, so the
  keyboard went silent after ~255 events (110 of 600 keys arrived; 600 of 600
  now). The transfer-ring Link TRB fix in the same commit was correct and kept.
- Endpoint context: Interval was written to bits 7:0 of dword 0 (the EP State
  field) instead of bits 23:16, bInterval was not converted to the xHCI
  exponent (a 10 ms full-speed keyboard would poll every 128 ms), and Average
  TRB Length / Max ESIT Payload were left zero (real controllers reject
  periodic endpoints that do). All three are fixed in `ep_interval_field()` and
  `build_input_context()`.
- Hubs: wPortStatus bit 8 is PORT_POWER, bit 9 LOW_SPEED, bit 10 HIGH_SPEED.
  Treating bit 8 as low speed classified every full-speed device behind a hub
  as low-speed. Children were also identified by their Transaction-Translator
  parent fields, which are zero under a full-speed hub, so the same port was
  reset and re-enumerated forever (killing the working device). Each device now
  records the hub and port it hangs off in `hub_slot`/`hub_port`. Empty hub
  ports are rescanned every pass; only a port whose device failed is throttled.
- `release_slot()` issued Disable Slot without the Slot ID (bits 31:24), so the
  controller rejected it and every slot ever opened stayed allocated; after a
  handful of plug cycles no device could enumerate. Fixed, and every failure
  path now goes through `abandon_device()`, which also frees the EP0, interrupt
  and bulk rings, the report buffer and the Output Device Context.
- Removal: nothing detected a disconnect, so an unplugged keyboard kept its
  slot and rings forever and the port was never enumerated again. Root-port and
  hub-port disconnects now remove the device and everything below it.
- Concurrency: control, bulk and command transfers, enumeration and the 8 ms
  poller all consumed the single event ring without a common lock, and an
  event that is not the caller's own is dropped, so a disk write could time out
  because the poller stole its completion. They now run under one transaction
  mutex (`xhci_acquire()`), held across waits without masking interrupts, since
  the deadlines read the PIT clock.
- Descriptors: `kernel/usb_desc.c` now parses configuration descriptors. A
  descriptor shorter than its type needs (an interface of length 2, a short
  endpoint) ends the walk instead of having fixed offsets read past it, and an
  endpoint is only accepted if it belongs to the interface being set up (the
  first interrupt-IN endpoint of a *later* interface used to be taken).
  A boot keyboard or mass-storage interface no longer has to be interface 0.
  `tests/test_usb_desc.c` covers the malformed shapes and fuzzes 300k blobs in
  exactly-sized buffers under AddressSanitizer.
- Raw Ethernet (`NET_SEND`/`NET_RECV`) is now root-only, like CAP_NET_RAW:
  receiving dequeues every frame on the wire and sending bypasses all source
  address checks. `NET_SET_ADDR` already was.
- `xhci-dbg` traces printed on every event were removed.

## Verification

Run here, on this tree: `make test` (host units: PCI, USB HID, USB descriptors,
heap, VFS, persistence, paths), `make fuzz` (ASan/UBSan; 7 seeds x 20000),
`make check`, `check-ahci`, `check-nvme`, `check-fuzz` (7 seeds x 6000, also
with an xHCI keyboard and USB disk attached), `check-usb` and `check-usb-hub`
(400 keys at 25 keys/s each), `check-usb-hotplug`, `check-usb-storage`,
`check-usb-storage-hub`, `check-pci` and `check-net`. `check-highmem` needs a
host that can give QEMU 5 GiB and was not run in the 4 GiB environment used for
this review.

Test-rate note: a full-speed keyboard delivers at most one report per poll
interval (8-10 ms), so `tools/qemu-usb.py` injects keys at 25 keys/s by
default. Faster injection overflows QEMU's own 16-entry HID queue and loses
keys regardless of the guest driver (the same burst over PS/2 is lossless).
`tools/qemu-usb-hotplug.py` and `tools/qemu-pci-check.py` are the QMP and
monitor drivers behind `check-usb-hotplug` and `check-pci`.
