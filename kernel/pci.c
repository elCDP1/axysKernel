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

#ifndef AXYS_HOST_TEST
static axys_uint32_t cfg_address(axys_uint8_t bus, axys_uint8_t device,
                                 axys_uint8_t function, axys_uint32_t offset)
{
    return 0x80000000u | ((axys_uint32_t)bus << 16) | ((axys_uint32_t)device << 11) |
           ((axys_uint32_t)function << 8) | (offset & 0xfcu);
}
#endif

#ifdef AXYS_HOST_TEST
/* Host unit tests cannot touch port 0xCF8 (userspace `in` faults), so the
 * whole mechanism below is redirected at a fake configuration space. Tests
 * populate it through axys_pci_test_reset() / axys_pci_test_cfg32(). */
#define FAKE_BUS_MAX 4u
static axys_uint32_t fake_cfg[FAKE_BUS_MAX][32][8][64];

/* Programmed BAR sizing masks: real hardware answers a BAR all-ones probe with
 * its size mask, but a plain array would just echo the all-ones back. Tests
 * install the mask per BAR slot; an all-ones write to that slot then arms the
 * probe (reads return the mask until any other value is written). Slots
 * without a programmed mask behave like the dumb array. */
static axys_uint32_t fake_bar_mask[FAKE_BUS_MAX][32][8][6];
static unsigned char fake_bar_has_mask[FAKE_BUS_MAX][32][8][6];
static unsigned char fake_bar_armed[FAKE_BUS_MAX][32][8][6];

void axys_pci_test_reset(void)
{
    for (axys_uint32_t b = 0; b < FAKE_BUS_MAX; ++b) {
        for (axys_uint32_t s = 0; s < 32u; ++s) {
            for (axys_uint32_t f = 0; f < 8u; ++f) {
                for (axys_uint32_t w = 0; w < 64u; ++w) {
                    fake_cfg[b][s][f][w] = 0xffffffffu; /* nothing answers */
                }
                for (axys_uint32_t bar = 0; bar < 6u; ++bar) {
                    fake_bar_mask[b][s][f][bar] = 0;
                    fake_bar_has_mask[b][s][f][bar] = 0;
                    fake_bar_armed[b][s][f][bar] = 0;
                }
            }
        }
    }
}

void axys_pci_test_cfg32(axys_uint8_t bus, axys_uint8_t slot, axys_uint8_t function,
                         axys_uint32_t offset, axys_uint32_t value)
{
    if (bus < FAKE_BUS_MAX && slot < 32u && function < 8u) {
        fake_cfg[bus][slot][function][(offset >> 2) & 63u] = value;
    }
}

void axys_pci_test_bar_mask(axys_uint8_t bus, axys_uint8_t slot, axys_uint8_t function,
                            axys_uint8_t bar, axys_uint32_t mask)
{
    if (bus < FAKE_BUS_MAX && slot < 32u && function < 8u && bar < 6u) {
        fake_bar_mask[bus][slot][function][bar] = mask;
        fake_bar_has_mask[bus][slot][function][bar] = 1;
        fake_bar_armed[bus][slot][function][bar] = 0;
    }
}

static int is_bar_offset(axys_uint32_t offset, axys_uint8_t *bar)
{
    if (offset >= AXYS_PCI_ID_BAR0 && offset < AXYS_PCI_ID_BAR0 + 24u) {
        *bar = (axys_uint8_t)((offset - AXYS_PCI_ID_BAR0) / 4u);
        return 1;
    }
    return 0;
}

static axys_uint32_t cfg_read32(axys_uint8_t bus, axys_uint8_t device,
                                axys_uint8_t function, axys_uint32_t offset)
{
    axys_uint8_t bar;

    if (bus < FAKE_BUS_MAX && device < 32u && function < 8u) {
        if (is_bar_offset(offset & ~3u, &bar) && fake_bar_armed[bus][device][function][bar]) {
            return fake_bar_mask[bus][device][function][bar];
        }
        return fake_cfg[bus][device][function][(offset >> 2) & 63u];
    }
    return 0xffffffffu;
}

static void cfg_write32(axys_uint8_t bus, axys_uint8_t device, axys_uint8_t function,
                        axys_uint32_t offset, axys_uint32_t value)
{
    axys_uint8_t bar;

    if (bus < FAKE_BUS_MAX && device < 32u && function < 8u) {
        if ((offset & ~3u) == AXYS_PCI_ID_COMMAND) {
            /* Emulate write-1-to-clear status like real hardware: the low
             * (command) half is a plain write, the high (status) half only
             * clears where ones are written. This is what makes the
             * set-command-rather-than-RMW discipline testable. */
            axys_uint32_t old = fake_cfg[bus][device][function][1];
            axys_uint32_t kept = old & 0xffff0000u & ~(value & 0xffff0000u);

            fake_cfg[bus][device][function][1] = kept | (value & 0xffffu);
            return;
        }
        if (is_bar_offset(offset & ~3u, &bar)) {
            fake_bar_armed[bus][device][function][bar] =
                (unsigned char)(value == 0xffffffffu && fake_bar_has_mask[bus][device][function][bar]);
        }
        fake_cfg[bus][device][function][(offset >> 2) & 63u] = value;
    }
}
#else
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
#endif

axys_uint32_t axys_pci_read32(const struct axys_pci_device *device, axys_uint32_t offset)
{
    return cfg_read32(device->bus, device->device, device->function, offset);
}

void axys_pci_write32(const struct axys_pci_device *device, axys_uint32_t offset,
                      axys_uint32_t value)
{
    cfg_write32(device->bus, device->device, device->function, offset, value);
}

axys_uint8_t axys_pci_read8(const struct axys_pci_device *device, axys_uint32_t offset)
{
    return (axys_uint8_t)(axys_pci_read32(device, offset & ~3u) >> ((offset & 3u) * 8u));
}

axys_uint16_t axys_pci_read16(const struct axys_pci_device *device, axys_uint32_t offset)
{
    if ((offset & 3u) == 3u) {
        return 0xffffu; /* would straddle two dwords; config space cannot do that */
    }
    return (axys_uint16_t)(axys_pci_read32(device, offset & ~3u) >> ((offset & 3u) * 8u));
}

void axys_pci_write8(const struct axys_pci_device *device, axys_uint32_t offset, axys_uint8_t value)
{
    axys_uint32_t shift = (offset & 3u) * 8u;
    axys_uint32_t dword = axys_pci_read32(device, offset & ~3u);

    dword = (dword & ~(0xffu << shift)) | ((axys_uint32_t)value << shift);
    axys_pci_write32(device, offset & ~3u, dword);
}

void axys_pci_write16(const struct axys_pci_device *device, axys_uint32_t offset, axys_uint16_t value)
{
    axys_uint32_t shift = (offset & 3u) * 8u;
    axys_uint32_t dword;

    if ((offset & 3u) == 3u) {
        return;
    }
    dword = axys_pci_read32(device, offset & ~3u);
    dword = (dword & ~(0xffffu << shift)) | ((axys_uint32_t)value << shift);
    axys_pci_write32(device, offset & ~3u, dword);
}

/* The command register shares a dword with the status register, whose bits are
 * write-1-to-clear: writing back what was read would acknowledge (and lose)
 * pending error bits. Always write zeros into the status half. */
static void cfg_set_command(axys_uint8_t bus, axys_uint8_t slot, axys_uint8_t function,
                            axys_uint16_t value)
{
    cfg_write32(bus, slot, function, AXYS_PCI_ID_COMMAND, (axys_uint32_t)value);
}

void axys_pci_set_command(const struct axys_pci_device *device, axys_uint16_t set,
                          axys_uint16_t clear)
{
    axys_uint16_t command = (axys_uint16_t)(cfg_read32(device->bus, device->device, device->function,
                                                       AXYS_PCI_ID_COMMAND) & 0xffffu);

    command = (axys_uint16_t)((command | set) & ~clear);
    cfg_set_command(device->bus, device->device, device->function, command);
}

void axys_pci_enable_bus_master(const struct axys_pci_device *device)
{
    axys_pci_set_command(device, AXYS_PCI_CMD_MEMORY | AXYS_PCI_CMD_MASTER, 0);
}

/* ---- BARs ------------------------------------------------------------------ */

/* Lowest set bit: for the value read back after writing all-ones to a BAR, the
 * set bits start at the BAR's size. */
static axys_uint64_t lowest_bit(axys_uint64_t value)
{
    return value & (~value + 1u);
}

/* Classify a BAR slot from its raw bits and fill in bases. Bit 0 decides I/O
 * versus memory; only then do bits 2:1 mean anything (reading them first is
 * what used to turn I/O BARs into bogus memory addresses). A 64-bit memory BAR
 * eats the next slot, whose contents are the upper address half and must not
 * be mistaken for a BAR of its own. Memory attribute bits occupy the low four,
 * so all four are masked from the address. Slots a header layout does not have
 * (a bridge has two BARs; offsets 0x18.. then hold bus numbers and windows) are
 * never read as BARs. */
static void decode_bars(struct axys_pci_device *device, axys_uint8_t bus, axys_uint8_t slot,
                        axys_uint8_t function)
{
    for (axys_uint8_t i = 0; i < AXYS_PCI_BAR_COUNT; ++i) {
        device->bars[i] = AXYS_PCI_BAR_NONE;
        device->bar_kind[i] = AXYS_PCI_BARK_NONE;
        device->bar_prefetchable[i] = 0;
        device->bar_addr[i] = 0;
        device->bar_size[i] = 0;
    }
    for (axys_uint8_t i = 0; i < device->bar_count; ++i) {
        axys_uint32_t raw = cfg_read32(bus, slot, function, AXYS_PCI_ID_BAR0 + i * 4u);
        axys_uint32_t type;

        if (raw == 0u || raw == 0xffffffffu) {
            continue; /* unimplemented BAR */
        }
        if ((raw & 1u) != 0u) {
            device->bar_kind[i] = AXYS_PCI_BARK_IO;
            device->bar_addr[i] = raw & 0xfffffffcu;
            continue; /* bars[] only ever held memory bases: drivers rely on that */
        }
        type = (raw >> 1) & 3u;
        device->bar_prefetchable[i] = (axys_uint8_t)((raw >> 3) & 1u);
        if (type == AXYS_PCI_BAR_MEM32) {
            device->bar_kind[i] = AXYS_PCI_BARK_MEM32;
            device->bar_addr[i] = raw & 0xfffffff0u;
            if (device->bar_addr[i] != 0u) {
                device->bars[i] = device->bar_addr[i];
            }
        } else if (type == AXYS_PCI_BAR_MEM64 && i + 1u < device->bar_count) {
            axys_uint32_t high = cfg_read32(bus, slot, function, AXYS_PCI_ID_BAR0 + (i + 1u) * 4u);
            axys_uint64_t base = ((axys_uint64_t)high << 32) | (raw & 0xfffffff0u);

            device->bar_kind[i] = AXYS_PCI_BARK_MEM64;
            device->bar_addr[i] = base;
            if (base != 0u) {
                device->bars[i] = base;
            }
            ++i; /* the upper half is not an independent BAR */
        }
        /* type 01 (below 1 MiB) and 11 are reserved: leave the slot NONE */
    }
}

/* Find each BAR's size the way the PCI spec prescribes: write all-ones, read
 * back which bits stuck, restore. The device must not decode while its BAR holds
 * a bogus address, so memory and I/O decoding are switched off meanwhile --
 * except on display controllers, whose legacy VGA window (the text console!) is
 * decoded through the same command bits. */
static void size_bars(struct axys_pci_device *device, axys_uint8_t bus, axys_uint8_t slot,
                      axys_uint8_t function)
{
    axys_uint16_t command = (axys_uint16_t)(cfg_read32(bus, slot, function, AXYS_PCI_ID_COMMAND) & 0xffffu);
    int quiesce = device->class_code != 0x03u;
    axys_uint8_t i = 0;

    if (quiesce) {
        cfg_set_command(bus, slot, function,
                        (axys_uint16_t)(command & ~(AXYS_PCI_CMD_IO | AXYS_PCI_CMD_MEMORY)));
    }
    while (i < device->bar_count) {
        axys_uint32_t offset = AXYS_PCI_ID_BAR0 + i * 4u;
        axys_uint32_t original = cfg_read32(bus, slot, function, offset);
        axys_uint32_t mask;
        axys_uint64_t size = 0;

        if (device->bar_kind[i] == AXYS_PCI_BARK_NONE) {
            /* Undecoded (zero or reserved) slot: probe once to tell a merely
             * unassigned 32-bit BAR from truly absent hardware. */
            if (original == 0u || original == 0xffffffffu || (original & 1u) != 0u ||
                ((original >> 1) & 3u) != 0u) {
                ++i;
                continue;
            }
            cfg_write32(bus, slot, function, offset, 0xffffffffu);
            mask = cfg_read32(bus, slot, function, offset);
            cfg_write32(bus, slot, function, offset, original);
            if (mask != 0u && mask != 0xffffffffu && (mask & 0xfffffff0u) != 0u) {
                device->bar_kind[i] = AXYS_PCI_BARK_MEM32;
                device->bar_prefetchable[i] = (axys_uint8_t)((mask >> 3) & 1u);
                device->bar_size[i] = lowest_bit(mask & 0xfffffff0u);
            }
            ++i;
            continue;
        }
        cfg_write32(bus, slot, function, offset, 0xffffffffu);
        mask = cfg_read32(bus, slot, function, offset);
        cfg_write32(bus, slot, function, offset, original);

        if (device->bar_kind[i] == AXYS_PCI_BARK_IO) {
            mask &= 0xfffffffcu;
            if ((mask & 0xffff0000u) == 0u) {
                mask |= 0xffff0000u; /* a 16-bit I/O BAR: the top half does not exist */
            }
            size = lowest_bit(mask);
        } else if (device->bar_kind[i] == AXYS_PCI_BARK_MEM32) {
            if (mask != 0u && mask != 0xffffffffu) {
                size = lowest_bit(mask & 0xfffffff0u);
            }
        } else { /* MEM64: size spans this slot and the next (skipped below) */
            axys_uint32_t high_offset = offset + 4u;
            axys_uint32_t high_original = cfg_read32(bus, slot, function, high_offset);
            axys_uint32_t high_mask;

            cfg_write32(bus, slot, function, high_offset, 0xffffffffu);
            high_mask = cfg_read32(bus, slot, function, high_offset);
            cfg_write32(bus, slot, function, high_offset, high_original);
            size = lowest_bit(((axys_uint64_t)high_mask << 32) | (mask & 0xfffffff0u));
            device->bar_size[i] = size;
            i += 2u;
            continue;
        }
        device->bar_size[i] = size;
        ++i;
    }
    if (quiesce) {
        cfg_set_command(bus, slot, function, command);
    }
}

/* ---- capabilities ---------------------------------------------------------- */

/* The list is a chain of (id, next) bytes starting at the 0x34 pointer. A
 * hostile or broken device can make it loop or point into the standard header,
 * so the walk stops after at most 48 entries (the most that fit in the 192
 * bytes above 0x40), and ignores pointers below 0x40. */
axys_uint8_t axys_pci_find_capability(const struct axys_pci_device *device, axys_uint8_t id)
{
    axys_uint8_t pointer = device->cap_ptr;

    for (int hops = 0; hops < 48 && pointer >= 0x40u; ++hops) {
        axys_uint32_t header;

        pointer &= 0xfcu;
        header = axys_pci_read32(device, pointer);
        if ((header & 0xffu) == id) {
            return pointer;
        }
        pointer = (axys_uint8_t)((header >> 8) & 0xffu);
    }
    return 0;
}

static axys_uint8_t first_capability(axys_uint8_t bus, axys_uint8_t slot, axys_uint8_t function,
                                     axys_uint8_t layout)
{
    axys_uint32_t status_cmd = cfg_read32(bus, slot, function, AXYS_PCI_ID_COMMAND);
    axys_uint8_t pointer;

    if ((layout != 0u && layout != 1u) || (((status_cmd >> 16) & AXYS_PCI_STATUS_CAP_LIST) == 0u)) {
        return 0;
    }
    pointer = (axys_uint8_t)(cfg_read32(bus, slot, function, 0x34u) & 0xfcu);
    return pointer >= 0x40u ? pointer : 0;
}

/* ---- scanning -------------------------------------------------------------- */

static struct axys_pci_device previous[PCI_MAX_DEVICES];
static int previous_count;
static unsigned char bus_seen[256];

static const struct axys_pci_device *remembered(axys_uint8_t bus, axys_uint8_t slot,
                                                axys_uint8_t function, axys_uint16_t vendor,
                                                axys_uint16_t device_id)
{
    for (int i = 0; i < previous_count; ++i) {
        const struct axys_pci_device *old = &previous[i];

        if (old->bus == bus && old->device == slot && old->function == function &&
            old->vendor_id == vendor && old->device_id == device_id) {
            return old;
        }
    }
    return AXYS_NULL;
}

static void record(axys_uint8_t bus, axys_uint8_t slot, axys_uint8_t function)
{
    axys_uint32_t id;
    struct axys_pci_device *device;
    const struct axys_pci_device *old;
    axys_uint8_t layout;

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
    layout = (axys_uint8_t)(device->header_type & 0x7fu);
    device->bar_count = layout == 0u ? 6u : (layout == 1u ? 2u : 0u);
    decode_bars(device, bus, slot, function);

    /* Sizing briefly switches decoding off. A rescan after drivers are running
     * must not do that to live devices, so reuse what the first scan found. */
    old = remembered(bus, slot, function, device->vendor_id, device->device_id);
    if (old != AXYS_NULL) {
        for (axys_uint8_t i = 0; i < AXYS_PCI_BAR_COUNT; ++i) {
            device->bar_size[i] = old->bar_size[i];
            if (device->bar_kind[i] == AXYS_PCI_BARK_NONE && old->bar_kind[i] != AXYS_PCI_BARK_NONE) {
                device->bar_kind[i] = old->bar_kind[i];
            }
        }
    } else {
        size_bars(device, bus, slot, function);
    }

    device->cap_ptr = first_capability(bus, slot, function, layout);
    device->secondary_bus = layout == 1u ? (axys_uint8_t)((cfg_read32(bus, slot, function, 0x18u) >> 8) & 0xffu) : 0u;
    if (layout == 0u) {
        axys_uint32_t subsystem = cfg_read32(bus, slot, function, 0x2cu);

        device->subsystem_vendor = (axys_uint16_t)(subsystem & 0xffffu);
        device->subsystem_device = (axys_uint16_t)(subsystem >> 16);
    } else {
        device->subsystem_vendor = 0;
        device->subsystem_device = 0;
    }
    {
        axys_uint32_t irq = cfg_read32(bus, slot, function, 0x3cu);

        device->interrupt_line = (axys_uint8_t)irq;
        device->interrupt_pin = (axys_uint8_t)(irq >> 8);
    }
}

static void scan_bus(axys_uint8_t bus, int depth);

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
            if ((axys_uint8_t)(class_reg >> 24) != AXYS_PCI_CLASS_BRIDGE ||
                (axys_uint8_t)(class_reg >> 16) != AXYS_PCI_SUBCLASS_PCI_BRIDGE) {
                continue;
            }
            secondary = (axys_uint8_t)((cfg_read32(bus, slot, function, 0x18u) >> 8) & 0xffu);
            if (secondary != 0u && secondary != bus) {
                scan_bus(secondary, depth + 1);
            }
        }
    }
}

static void scan_bus(axys_uint8_t bus, int depth)
{
    if (depth > PCI_MAX_BUS_DEPTH || bus_seen[bus]) {
        return; /* too deep, or two bridges claimed the same secondary bus */
    }
    bus_seen[bus] = 1;
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

    for (int i = 0; i < device_count; ++i) {
        previous[i] = devices[i];
    }
    previous_count = device_count;
    for (int i = 0; i < 256; ++i) {
        bus_seen[i] = 0;
    }
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

const struct axys_pci_device *axys_pci_find_id(axys_uint16_t vendor, axys_uint16_t device_id)
{
    for (int i = 0; i < device_count; ++i) {
        if (devices[i].vendor_id == vendor && devices[i].device_id == device_id) {
            return &devices[i];
        }
    }
    return AXYS_NULL;
}

const struct axys_pci_device *axys_pci_find_bdf(axys_uint8_t bus, axys_uint8_t slot,
                                                axys_uint8_t function)
{
    for (int i = 0; i < device_count; ++i) {
        if (devices[i].bus == bus && devices[i].device == slot && devices[i].function == function) {
            return &devices[i];
        }
    }
    return AXYS_NULL;
}

/* ---- names and the /proc/pci text ----------------------------------------- */

static const struct {
    axys_uint8_t class_code;
    axys_uint8_t subclass;
    const char *name;
} class_names[] = {
    {0x00, 0x00, "Unclassified device"}, {0x01, 0x00, "SCSI storage controller"},
    {0x01, 0x01, "IDE interface"},       {0x01, 0x02, "Floppy controller"},
    {0x01, 0x04, "RAID controller"},     {0x01, 0x05, "ATA controller"},
    {0x01, 0x06, "SATA controller"},     {0x01, 0x07, "SAS controller"},
    {0x01, 0x08, "Non-volatile memory controller"},
    {0x01, 0x80, "Mass storage controller"},
    {0x02, 0x00, "Ethernet controller"}, {0x02, 0x80, "Network controller"},
    {0x03, 0x00, "VGA compatible controller"}, {0x03, 0x01, "XGA controller"},
    {0x03, 0x02, "3D controller"},       {0x03, 0x80, "Display controller"},
    {0x04, 0x00, "Multimedia video controller"}, {0x04, 0x01, "Multimedia audio controller"},
    {0x04, 0x03, "Audio device"},        {0x05, 0x00, "RAM memory"},
    {0x05, 0x01, "Flash memory"},        {0x06, 0x00, "Host bridge"},
    {0x06, 0x01, "ISA bridge"},          {0x06, 0x04, "PCI bridge"},
    {0x06, 0x80, "Bridge"},              {0x07, 0x00, "Serial controller"},
    {0x08, 0x00, "PIC"},                 {0x08, 0x80, "System peripheral"},
    {0x09, 0x00, "Keyboard controller"}, {0x0c, 0x00, "FireWire controller"},
    {0x0c, 0x03, "USB controller"},      {0x0c, 0x05, "SMBus"},
    {0x0d, 0x80, "Wireless controller"}, {0x10, 0x80, "Encryption controller"},
};

const char *axys_pci_class_name(axys_uint8_t class_code, axys_uint8_t subclass)
{
    for (axys_size_t i = 0; i < sizeof(class_names) / sizeof(class_names[0]); ++i) {
        if (class_names[i].class_code == class_code && class_names[i].subclass == subclass) {
            return class_names[i].name;
        }
    }
    return "Unknown class";
}

const char *axys_pci_vendor_name(axys_uint16_t vendor)
{
    switch (vendor) {
    case 0x8086: return "Intel";
    case 0x1022: return "AMD";
    case 0x1002: return "AMD/ATI";
    case 0x10de: return "NVIDIA";
    case 0x10ec: return "Realtek";
    case 0x1af4: return "Red Hat (virtio)";
    case 0x1b36: return "Red Hat (QEMU)";
    case 0x1234: return "QEMU/Bochs";
    case 0x15ad: return "VMware";
    case 0x1b21: return "ASMedia";
    case 0x144d: return "Samsung";
    case 0x1217: return "O2 Micro";
    default: return "unknown vendor";
    }
}

static const char *capability_name(axys_uint8_t id)
{
    switch (id) {
    case AXYS_PCI_CAP_PM: return "PM";
    case AXYS_PCI_CAP_MSI: return "MSI";
    case AXYS_PCI_CAP_VENDOR: return "vendor";
    case AXYS_PCI_CAP_PCIE: return "PCIe";
    case AXYS_PCI_CAP_MSIX: return "MSI-X";
    default: return "other";
    }
}

static const char *usb_kind(axys_uint8_t prog_if)
{
    switch (prog_if) {
    case 0x00: return "UHCI";
    case 0x10: return "OHCI";
    case 0x20: return "EHCI";
    case 0x30: return "xHCI";
    case 0x40: return "USB4";
    default: return "USB";
    }
}

struct text {
    char *buffer;
    axys_size_t capacity;
    axys_size_t length;
};

static void put(struct text *out, const char *format, ...)
{
    axys_va_list arguments;
    int n;

    if (out->length + 1u >= out->capacity) {
        return;
    }
    __builtin_va_start(arguments, format);
    n = axys_vsnprintf(out->buffer + out->length, out->capacity - out->length, format, arguments);
    __builtin_va_end(arguments);
    if (n > 0) {
        axys_size_t room = out->capacity - out->length - 1u;

        out->length += (axys_size_t)n < room ? (axys_size_t)n : room;
    }
}

static void put_size(struct text *out, axys_uint64_t size)
{
    if (size >= (1ull << 30) && (size & ((1ull << 30) - 1ull)) == 0u) {
        put(out, "%lluG", (unsigned long long)(size >> 30));
    } else if (size >= (1ull << 20) && (size & ((1ull << 20) - 1ull)) == 0u) {
        put(out, "%lluM", (unsigned long long)(size >> 20));
    } else if (size >= 1024u && (size & 1023u) == 0u) {
        put(out, "%lluK", (unsigned long long)(size >> 10));
    } else {
        put(out, "%llu", (unsigned long long)size);
    }
}

axys_size_t axys_pci_format_all(char *buffer, axys_size_t capacity)
{
    struct text out = {buffer, capacity, 0};

    if (buffer == AXYS_NULL || capacity == 0u) {
        return 0;
    }
    buffer[0] = '\0';
    for (int i = 0; i < device_count; ++i) {
        const struct axys_pci_device *d = &devices[i];

        put(&out, "%02x:%02x.%u %04x:%04x %s", d->bus, d->device, d->function, d->vendor_id,
            d->device_id, axys_pci_class_name(d->class_code, d->subclass));
        if (d->class_code == 0x0cu && d->subclass == 0x03u) {
            put(&out, " (%s)", usb_kind(d->prog_if));
        }
        put(&out, " [%s] rev %02x\n", axys_pci_vendor_name(d->vendor_id), d->revision);
        if ((d->header_type & 0x7fu) == 1u) {
            put(&out, "    secondary bus %02x\n", d->secondary_bus);
        }
        for (axys_uint8_t b = 0; b < d->bar_count; ++b) {
            if (d->bar_kind[b] == AXYS_PCI_BARK_NONE) {
                continue;
            }
            put(&out, "    BAR%u: %s%s %llx size ", b,
                d->bar_kind[b] == AXYS_PCI_BARK_IO ? "io" : (d->bar_kind[b] == AXYS_PCI_BARK_MEM64 ? "mem64" : "mem32"),
                d->bar_prefetchable[b] ? " prefetchable" : "", (unsigned long long)d->bar_addr[b]);
            put_size(&out, d->bar_size[b]);
            put(&out, "\n");
        }
        if (d->cap_ptr != 0u) {
            axys_uint8_t pointer = d->cap_ptr;

            put(&out, "    caps:");
            for (int hops = 0; hops < 48 && pointer >= 0x40u; ++hops) {
                axys_uint32_t header = axys_pci_read32(d, pointer & 0xfcu);

                put(&out, " %s", capability_name((axys_uint8_t)header));
                pointer = (axys_uint8_t)((header >> 8) & 0xffu);
            }
            put(&out, "\n");
        }
    }
    return out.length;
}
