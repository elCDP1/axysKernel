#ifndef AXYS_PCI_H
#define AXYS_PCI_H

#include "axys/types.h"

/* PCI configuration space, reached through the 0xCF8/0xCFC window. Enough of
 * PCI to find a device, enable its memory decoding and bus mastering, read a
 * base address, size BARs, walk capabilities and describe the bus for drivers
 * and diagnostics. No interrupts: the drivers here poll their status registers,
 * so no GSI or MSI routing is needed. */

#define AXYS_PCI_CFG_ADDRESS 0xcf8u
#define AXYS_PCI_CFG_DATA 0xcfc

#define AXYS_PCI_ID_VENDOR 0x00u
#define AXYS_PCI_ID_COMMAND 0x04u
#define AXYS_PCI_ID_STATUS 0x06u
#define AXYS_PCI_ID_REVISION 0x08u
/* Bytes 0x08..0x0b are revision, class prog-if, subclass and base class. Read as
 * a dword: [0]=revision [1]=prog-if [2]=subclass [3]=base class. The class code
 * does not start at 0x0b -- 0x0b holds the base class on its own. */
#define AXYS_PCI_ID_CLASS 0x08u
#define AXYS_PCI_ID_HEADER 0x0eu
#define AXYS_PCI_ID_BAR0 0x10u
#define AXYS_PCI_BAR_COUNT 6u

/* BAR type, bits 2:1 of the BAR. Note that 00 is 32-bit memory and 01 is I/O:
 * reading a zero here does not mean the BAR is unused, and treating it as
 * unused hides every 32-bit memory BAR, which is what most controllers use. */
#define AXYS_PCI_BAR_MEM32 0u
#define AXYS_PCI_BAR_IO 1u
#define AXYS_PCI_BAR_MEM64 2u
#define AXYS_PCI_BAR_RESERVED 3u

/* What a BAR slot holds. A 64-bit memory BAR occupies two slots: the first is
 * AXYS_PCI_BARK_MEM64 and the second (its upper half) is AXYS_PCI_BARK_NONE. */
#define AXYS_PCI_BARK_NONE 0u
#define AXYS_PCI_BARK_MEM32 1u
#define AXYS_PCI_BARK_MEM64 2u
#define AXYS_PCI_BARK_IO 3u

/* Capability IDs (legacy list, reached through the 0x34 pointer). */
#define AXYS_PCI_CAP_PM 0x01u
#define AXYS_PCI_CAP_MSI 0x05u
#define AXYS_PCI_CAP_VENDOR 0x09u
#define AXYS_PCI_CAP_PCIE 0x10u
#define AXYS_PCI_CAP_MSIX 0x11u

#define AXYS_PCI_STATUS_CAP_LIST 0x0010u

#define AXYS_PCI_CMD_IO 0x0001u
#define AXYS_PCI_CMD_MEMORY 0x0002u
#define AXYS_PCI_CMD_MASTER 0x0004u

/* Class codes are the base-class byte on its own; the subclass is a separate
 * argument, so these are deliberately single bytes and not the packed
 * (class << 8) | subclass pair that the class/subclass dword holds. */
#define AXYS_PCI_CLASS_STORAGE 0x01u
#define AXYS_PCI_CLASS_BRIDGE 0x06u
#define AXYS_PCI_SUBCLASS_SATA 0x06u
#define AXYS_PCI_SUBCLASS_PCI_BRIDGE 0x04u

/* A function found on the bus. bars[] holds the decoded, 64-bit-safe base
 * addresses; AXYS_PCI_BAR_NONE marks a BAR that is absent, unimplemented or an
 * IO port rather than a memory range. */
#define AXYS_PCI_BAR_NONE (~(axys_uint64_t)0)

struct axys_pci_device {
    axys_uint8_t bus;
    axys_uint8_t device;
    axys_uint8_t function;
    axys_uint16_t vendor_id;
    axys_uint16_t device_id;
    axys_uint8_t class_code;
    axys_uint8_t subclass;
    axys_uint8_t prog_if;
    axys_uint8_t revision;
    axys_uint8_t header_type; /* raw byte: bit 7 = multifunction, bits 6:0 = layout */
    axys_uint64_t bars[AXYS_PCI_BAR_COUNT]; /* memory BAR bases only (see above) */

    /* Everything below was added for drivers that need more than a base. */
    axys_uint8_t bar_count;                    /* BAR slots this header layout has: 6, 2 or 0 */
    axys_uint8_t bar_kind[AXYS_PCI_BAR_COUNT]; /* AXYS_PCI_BARK_* */
    axys_uint8_t bar_prefetchable[AXYS_PCI_BAR_COUNT];
    axys_uint64_t bar_addr[AXYS_PCI_BAR_COUNT]; /* base, or the port number for an I/O BAR */
    axys_uint64_t bar_size[AXYS_PCI_BAR_COUNT]; /* 0 = unimplemented or could not be sized */
    axys_uint8_t cap_ptr;                      /* first capability offset, 0 = none */
    axys_uint8_t secondary_bus;                /* bridges only */
    axys_uint8_t interrupt_line;
    axys_uint8_t interrupt_pin;
    axys_uint16_t subsystem_vendor;
    axys_uint16_t subsystem_device;
};

/* Walks every bus reachable through the host bridge and its downstream
 * bridges. Returns the number of functions found (absent functions are not
 * counted), or -1 if the configuration mechanism reported no access at all. */
int axys_pci_scan(void);

int axys_pci_count(void);
const struct axys_pci_device *axys_pci_device_at(int index);

/* First device whose class/subclass matches, or AXYS_NULL. */
const struct axys_pci_device *axys_pci_find(axys_uint8_t class_code, axys_uint8_t subclass);

axys_uint32_t axys_pci_read32(const struct axys_pci_device *device, axys_uint32_t offset);
void axys_pci_write32(const struct axys_pci_device *device, axys_uint32_t offset,
                      axys_uint32_t value);

/* Byte / word accessors. Writes read-modify-write the containing dword, so the
 * neighbouring bytes are preserved. Writing the 16-bit status register (0x06)
 * this way would clear write-1-to-clear bits: use axys_pci_set_command() for the
 * command register and never write status through write16. */
axys_uint8_t axys_pci_read8(const struct axys_pci_device *device, axys_uint32_t offset);
axys_uint16_t axys_pci_read16(const struct axys_pci_device *device, axys_uint32_t offset);
void axys_pci_write8(const struct axys_pci_device *device, axys_uint32_t offset, axys_uint8_t value);
void axys_pci_write16(const struct axys_pci_device *device, axys_uint32_t offset, axys_uint16_t value);

/* Set and clear bits of the command register in one write that cannot touch the
 * status register above it. */
void axys_pci_set_command(const struct axys_pci_device *device, axys_uint16_t set,
                          axys_uint16_t clear);

/* Offset of the first capability with this ID, or 0. The walk is bounded, so a
 * malformed (looping or out-of-range) list ends the search instead of hanging. */
axys_uint8_t axys_pci_find_capability(const struct axys_pci_device *device, axys_uint8_t id);

/* Find a function by vendor and device ID, or by exact bus/device/function. */
const struct axys_pci_device *axys_pci_find_id(axys_uint16_t vendor, axys_uint16_t device_id);
const struct axys_pci_device *axys_pci_find_bdf(axys_uint8_t bus, axys_uint8_t slot,
                                                axys_uint8_t function);

/* Human-readable class and vendor ("Host bridge", "Intel"); never NULL. */
const char *axys_pci_class_name(axys_uint8_t class_code, axys_uint8_t subclass);
const char *axys_pci_vendor_name(axys_uint16_t vendor);

/* Render every discovered function (and its BARs and capabilities) as text, as
 * published in /proc/pci. Returns the number of bytes written (always
 * NUL-terminated; truncated if `capacity` is too small). */
axys_size_t axys_pci_format_all(char *buffer, axys_size_t capacity);

/* Turn on memory decoding and bus mastering (the HBA DMAs over PCIe, so it
 * needs both). A no-op if they are already set. */
void axys_pci_enable_bus_master(const struct axys_pci_device *device);

#endif
