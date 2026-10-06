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
| USB | xHCI host controller (PCI 0c/03/0x30, polled command/event rings, port reset, full/low/high-speed address/configure); USB hubs with route-string routing up to 5 levels deep; boot keyboards (HID); bulk-only mass storage (512-byte sectors, READ/WRITE(10), TEST UNIT READY) usable as the system disk; /proc/pci inventory; `make check-usb`, `check-usb-hub`, `check-usb-storage`, `check-usb-storage-hub` | Polled only (no event-interrupt path), no isochronous/interrupt endpoints, no HID beyond boot keyboards, no SCSI pass-through or multiple LUNs, USB mass storage is used only at boot (no removal or hotplug), hub hotplug scan covers the 16 ports it reports but nothing above 5 levels deep, no UHCI/OHCI/EHCI |
| Keyboard | PS/2 set-1 through i8042; USB HID boot keyboards through xHCI polling, directly or behind hubs; serial input | Native USB host controllers other than xHCI are absent; USB HID mice and non-boot HID reports are not supported |
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
4. USB: isochronous and interrupt endpoints, HID beyond boot keyboards,
   UHCI/OHCI/EHCI alongside xHCI, and removal/hotplug for mass storage.
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

## Verification

`make test` (host units incl. PCI and USB HID), `make fuzz` (ASan/UBSan host
fuzzing), `make check` (boot, shell, `probe`, persistence), `check-highmem`,
`check-ahci`, `check-nvme`, `check-fuzz`, `check-usb`, `check-usb-hub`,
`check-usb-storage`, `check-usb-storage-hub`, and `check-net` all pass. Every
added driver is covered by an automated QEMU scenario; `tools/qemu-usb.py`
takes `TOPOLOGY=direct|hub` and the storage/net checks reuse the persistence
and network stages of `tools/qemu-test.sh`.
