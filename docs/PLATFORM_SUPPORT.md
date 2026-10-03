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
| Display | VGA text console; bootloader framebuffer metadata is parsed and reserved | No universal framebuffer console, modesetting, or Intel/AMD/NVIDIA GPU driver |
| Keyboard | PS/2 set-1 through i8042; serial input | USB HID and native USB host controllers are absent; firmware legacy emulation is firmware-dependent |
| Storage | PCI NVMe polling (first controller, namespace 1, 512-byte LBAs, one outstanding command); primary-channel ATA PIO (LBA28); PCI AHCI polling; QEMU tests for NVMe/AHCI/IDE persistence | No secondary ATA channel, DMA for ATA PIO, NCQ, interrupt-driven queues, USB mass storage, other NVMe namespaces/controllers, or RAID |
| Network | None | Ethernet and Wi-Fi are not implemented |
| Audio | None | No HDA, AC'97, USB audio, or audio stack |

The QEMU integration test covers its configured x86-64 firmware, serial
console, serial-driven shell input, and IDE/AHCI disks. A separate manual QEMU
check has exercised the PS/2 right-Ctrl release path. These tests do not
validate physical machines or unrelated controller models.

`make check-highmem` runs `MEM=5G tools/qemu-test.sh -machine
pc,max-ram-below-4g=4G` to place usable RAM above the 4 GiB boundary. It checks
the detected physical range and runs the same process, shell, and disk
persistence scenarios as the standard integration test.

PCI BAR registers used by the current AHCI and NVMe drivers are mapped
uncached after checking that the firmware memory map does not report the range
as usable RAM. The page tables split the affected 2 MiB identity pages into
4 KiB entries when required. DMA buffers currently reside in the low kernel
image; there is no general DMA allocator, IOMMU setup, or MSI/MSI-X routing.

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
   interrupt-driven multi-queue devices.
3. Complete PCI BAR sizing and resource ownership, then add a general DMA/IOMMU
   policy, MSI/MSI-X routing, and a device/driver lifecycle before adding more
   PCI drivers.
4. Add xHCI and USB HID first, then USB mass storage; add NVMe using the same
   PCI, DMA, and interrupt foundations.
5. Add a small, explicit set of network devices for QEMU and common hardware,
   with a packet buffer and network stack before attempting Wi-Fi.
6. Treat GPU support as separate vendor/device families. Begin with a generic
   boot framebuffer and 2D console; 3D acceleration requires vendor-specific
   command submission, memory management, and extensive validation.

The project will publish concrete device IDs and tested QEMU models as drivers
land. “All hardware” is not a finite testable support target.
