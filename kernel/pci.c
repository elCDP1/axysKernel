#include "axys/pci.h"
#include "axys/io.h"
#include "axys/printf.h"

/* PCI configuration space lives behind a single 32-bit port pair: write
 * 0x80000000 | bus<<16 | dev<<11 | func<<8 | (offset & 0xfc) to 0xCF8, then
 * read or write the payload at 0xCFC. Offsets must be dword aligned. */

#define PCI_MAX_DEVICES 64
#define PCI_MAX_BUS_DEPTH 4

static struct axys_pci_device devices[PCI_MAX_DEVICES];
static int device_count;

static axys_uint32_t cfg_address(axys_uint8_t bus, axys_uint8_t device,
                                 axys_uint8_t function, axys_uint32_t offset)
{
    return 0x80000000u | ((axys_uint32_t)bus << 16) | ((axys_uint32_t)device << 11) |
           ((axys_uint32_t)function << 8) | (offset & 0xfcu);
}

static axys_uint32_t cfg_read32(axys_uint8_t bus, axys_uint8_t device,
                                axys_uint8_t function, axys_uint32_t offset)
{
    axys_outd(AXYS_PCI_CFG_ADDRESS, cfg_address(bus, device, function, offset));
    return axys_ind(AXYS_PCI_CFG_DATA);
}

static void cfg_write32(axys_uint8_t bus, axys_uint8_t device, axys_uint8_t function,
                        axys_uint32_t offset, axys_uint32_t value)
{
    axys_outd(AXYS_PCI_CFG_ADDRESS, cfg_address(bus, device, function, offset));
    axys_outd(AXYS_PCI_CFG_DATA, value);
}

axys_uint32_t axys_pci_read32(const struct axys_pci_device *device, axys_uint32_t offset)
{
    return cfg_read32(device->bus, device->device, device->function, offset);
}

void axys_pci_write32(const struct axys_pci_device *device, axys_uint32_t offset,
                      axys_uint32_t value)
{
    cfg_write32(device->bus, device->device, device->function, offset, value);
}

void axys_pci_enable_bus_master(const struct axys_pci_device *device)
{
    axys_uint32_t command = axys_pci_read32(device, AXYS_PCI_ID_COMMAND) & 0xffffu;

    command |= AXYS_PCI_CMD_MEMORY | AXYS_PCI_CMD_MASTER;
    cfg_write32(device->bus, device->device, device->function, AXYS_PCI_ID_COMMAND, command);
}

/* BARs are 32-bit slots whose meaning depends on bits 2:1 of the slot. A 64-bit
 * memory BAR consumes two consecutive slots, and the low half carries bit 0 set
 * to mark the pair; the upper half contains address bits only. Memory BAR
 * attributes occupy the low four bits, so they must all be masked from the
 * address (masking only bits 1:0 leaves the 64-bit type bit in the result). */
static void decode_bars(struct axys_pci_device *device, axys_uint8_t bus, axys_uint8_t slot,
                        axys_uint8_t function)
{
    for (axys_uint8_t i = 0; i < AXYS_PCI_BAR_COUNT; ++i) {
        axys_uint32_t raw = cfg_read32(bus, slot, function, AXYS_PCI_ID_BAR0 + i * 4u);
        axys_uint32_t type = (raw >> 1) & 3u;

        device->bars[i] = AXYS_PCI_BAR_NONE;
        if (raw == 0u || raw == 0xffffffffu) {
            continue; /* unimplemented BAR */
        }
        if (type == AXYS_PCI_BAR_MEM32) {
            device->bars[i] = raw & 0xfffffff0u;
        } else if (type == AXYS_PCI_BAR_MEM64) {
            axys_uint32_t high = cfg_read32(bus, slot, function, AXYS_PCI_ID_BAR0 + (i + 1u) * 4u);

            /* The 64-bit address spans this BAR and the next one. Read the upper
             * half even when this is the last slot: the register exists in
             * configuration space regardless of how many BARs are tracked. */
            device->bars[i] = ((axys_uint64_t)high << 32) | (raw & 0xfffffff0u);
            if (i + 1u < AXYS_PCI_BAR_COUNT) {
                device->bars[i + 1u] = AXYS_PCI_BAR_NONE;
                ++i; /* the upper half is not an independent BAR */
            }
        }
        /* I/O space and the reserved encoding stay AXYS_PCI_BAR_NONE: nothing
         * in the kernel maps or dereferences them. */
    }
}

static void record(axys_uint8_t bus, axys_uint8_t slot, axys_uint8_t function)
{
    axys_uint32_t id;
    struct axys_pci_device *device;

    if (device_count >= PCI_MAX_DEVICES) {
        return;
    }
    id = cfg_read32(bus, slot, function, AXYS_PCI_ID_VENDOR);
    if ((id & 0xffffu) == 0xffffu) {
        return; /* no device responds */
    }
    device = &devices[device_count++];
    device->bus = bus;
    device->device = slot;
    device->function = function;
    device->vendor_id = (axys_uint16_t)(id & 0xffffu);
    device->device_id = (axys_uint16_t)(id >> 16);
    {
        axys_uint32_t class_reg = cfg_read32(bus, slot, function, AXYS_PCI_ID_CLASS);

        device->prog_if = (axys_uint8_t)(class_reg >> 8);
        device->subclass = (axys_uint8_t)(class_reg >> 16);
        device->class_code = (axys_uint8_t)(class_reg >> 24);
        device->revision = (axys_uint8_t)class_reg;
    }
    device->header_type = (axys_uint8_t)((cfg_read32(bus, slot, function, AXYS_PCI_ID_HEADER) >> 16) & 0xffu);
    decode_bars(device, bus, slot, function);
}

static void scan_bus(axys_uint8_t bus, int depth);

/* A bridge forwards to the bus number in its secondary bus register, so
 * following one is all it takes to reach devices on nested buses. */
static void scan_bridges(axys_uint8_t bus, int depth)
{
    for (axys_uint8_t slot = 0; slot < 32u; ++slot) {
        axys_uint8_t functions = 1u;
        axys_uint8_t header;

        if (cfg_read32(bus, slot, 0, AXYS_PCI_ID_VENDOR) == 0xffffffffu) {
            continue;
        }
        header = (axys_uint8_t)((cfg_read32(bus, slot, 0, AXYS_PCI_ID_HEADER) >> 16) & 0x80u);
        if (header != 0u) {
            functions = 8u;
        }
        for (axys_uint8_t function = 0; function < functions; ++function) {
            axys_uint32_t class_reg;
            axys_uint8_t secondary;

            if (cfg_read32(bus, slot, function, AXYS_PCI_ID_VENDOR) == 0xffffffffu) {
                continue;
            }
    class_reg = cfg_read32(bus, slot, function, AXYS_PCI_ID_CLASS);
    /* Subclass in bits 7:0, base class in bits 15:8: match both halves, or the
     * comparison against a single combined value can never succeed. */
    if ((axys_uint8_t)(class_reg >> 24) != AXYS_PCI_CLASS_BRIDGE ||
        (axys_uint8_t)(class_reg >> 16) != AXYS_PCI_SUBCLASS_PCI_BRIDGE) {
                continue;
            }
            secondary = (axys_uint8_t)((cfg_read32(bus, slot, function, 0x19u) >> 8) & 0xffu);
            if (secondary != 0u && secondary != bus) {
                scan_bus(secondary, depth + 1);
            }
        }
    }
}

static void scan_bus(axys_uint8_t bus, int depth)
{
    if (depth > PCI_MAX_BUS_DEPTH) {
        return;
    }
    for (axys_uint8_t slot = 0; slot < 32u; ++slot) {
        axys_uint8_t functions = 1u;

        if (cfg_read32(bus, slot, 0, AXYS_PCI_ID_VENDOR) == 0xffffffffu) {
            continue; /* nothing at this slot at all */
        }
        /* Bit 7 of the header type says whether the other seven functions of
         * this slot can exist; a plain device reports 0 and functions 1..7 are
         * then not even decoded. */
        if (((cfg_read32(bus, slot, 0, AXYS_PCI_ID_HEADER) >> 16) & 0x80u) != 0u) {
            functions = 8u;
        }
        for (axys_uint8_t function = 0; function < functions; ++function) {
            if (cfg_read32(bus, slot, function, AXYS_PCI_ID_VENDOR) == 0xffffffffu) {
                continue;
            }
            record(bus, slot, function);
        }
    }
    scan_bridges(bus, depth);
}

int axys_pci_scan(void)
{
    axys_uint32_t probe;

    device_count = 0;
    /* A machine with no PCI host bridge answers 0xffffffff for every slot; that
     * is a real outcome (the kernel still runs, just without a bus), not an
     * error, so it is reported as zero devices rather than a failure. */
    probe = cfg_read32(0, 0, 0, AXYS_PCI_ID_VENDOR);
    if (probe == 0xffffffffu) {
        return 0;
    }
    scan_bus(0, 0);
    return device_count;
}

int axys_pci_count(void)
{
    return device_count;
}

const struct axys_pci_device *axys_pci_device_at(int index)
{
    if (index < 0 || index >= device_count) {
        return AXYS_NULL;
    }
    return &devices[index];
}

const struct axys_pci_device *axys_pci_find(axys_uint8_t class_code, axys_uint8_t subclass)
{
    for (int i = 0; i < device_count; ++i) {
        if (devices[i].class_code == class_code && devices[i].subclass == subclass) {
            return &devices[i];
        }
    }
    return AXYS_NULL;
}
