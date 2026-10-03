#ifndef AXYS_PCI_H
#define AXYS_PCI_H

#include "axys/types.h"

/* PCI configuration space, reached through the 0xCF8/0xCFC window. Enough of
 * PCI to find a device, enable its memory decoding and bus mastering, and read
 * a base address. No interrupts: the drivers here poll their status registers,
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
    axys_uint8_t header_type;
    axys_uint64_t bars[AXYS_PCI_BAR_COUNT];
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

/* Turn on memory decoding and bus mastering (the HBA DMAs over PCIe, so it
 * needs both). A no-op if they are already set. */
void axys_pci_enable_bus_master(const struct axys_pci_device *device);

#endif
