/*
 * PCI + DMA host tests. kernel/pci.c and kernel/dma.c are included directly:
 * the PCI config mechanism is redirected at a fake space (AXYS_HOST_TEST) and
 * the PMM is a bump pool, so BAR sizing, capability walks, rescans, /proc/pci
 * text and DMA placement rules all run under ASan/UBSan on the host.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AXYS_SPINLOCK_H /* replace the privileged cli/sti spinlock */
#include "axys/types.h"
struct axys_spinlock {
    volatile axys_uint32_t word;
};
#define AXYS_SPINLOCK_INIT(name) {.word = 0}
static inline axys_uint64_t axys_spin_lock_irqsave(struct axys_spinlock *l) { (void)l; return 0; }
static inline void axys_spin_unlock_irqrestore(struct axys_spinlock *l, axys_uint64_t f) { (void)l; (void)f; }

#include "axys/pci.h"
#include "axys/dma.h"
#include "axys/pmm.h"

extern void axys_pci_test_reset(void);
extern void axys_pci_test_cfg32(axys_uint8_t bus, axys_uint8_t slot, axys_uint8_t function,
                                axys_uint32_t offset, axys_uint32_t value);
extern void axys_pci_test_bar_mask(axys_uint8_t bus, axys_uint8_t slot, axys_uint8_t function,
                                   axys_uint8_t bar, axys_uint32_t mask);

/* Bump-pool PMM stub: 64 MiB, never reused (the stub free is a no-op and the
 * DMA test allocates cumulatively, so the pool must hold every allocation). */
static unsigned char pool[64 << 20] __attribute__((aligned(4096)));
static size_t pool_off;

axys_uint64_t axys_pmm_alloc_contiguous(axys_uint64_t count)
{
    size_t bytes = (size_t)count * 4096;

    if (pool_off + bytes > sizeof(pool)) {
        return 0;
    }
    pool_off += bytes;
    return (axys_uint64_t)(uintptr_t)(pool + pool_off - bytes);
}

void axys_pmm_free_contiguous(axys_uint64_t address, axys_uint64_t count)
{
    (void)address;
    (void)count;
}

void axys_panic(const char *m)
{
    fprintf(stderr, "panic: %s\n", m);
    abort();
}

#include "../kernel/pci.c"
#include "../kernel/dma.c"

/* Build one fake function: vendor/device, class triple, header layout,
 * command+status, and optional BAR raws. */
static void fake_fn(unsigned bus, unsigned slot, unsigned func, unsigned vendor, unsigned dev,
                    unsigned cls, unsigned sub, unsigned prog, unsigned rev, unsigned layout,
                    unsigned multifunction)
{
    axys_pci_test_cfg32(bus, slot, func, 0x00, (dev << 16) | vendor);
    axys_pci_test_cfg32(bus, slot, func, 0x04, 0x00100000); /* status: cap list */
    axys_pci_test_cfg32(bus, slot, func, 0x08,
                        (cls << 24) | (sub << 16) | (prog << 8) | rev);
    axys_pci_test_cfg32(bus, slot, func, 0x0c,
                        ((layout | (multifunction ? 0x80u : 0u)) << 16));
}

static void test_empty_bus(void)
{
    axys_pci_test_reset();
    assert(axys_pci_scan() == 0);
    assert(axys_pci_count() == 0);
    assert(axys_pci_find(0x01, 0x06) == NULL);
}

/* Every scanning test needs a host bridge at 00:00.0: the scanner probes it
 * first to tell "no PCI at all" apart from "an empty bus". */
static void fake_host_bridge(void)
{
    fake_fn(0, 0, 0, 0x8086, 0x1237, 0x06, 0x00, 0x00, 0x00, 0, 0);
}

static void test_ahci_decode_and_size(void)
{
    const struct axys_pci_device *d;

    axys_pci_test_reset();
    fake_host_bridge();
    fake_fn(0, 5, 0, 0x8086, 0x2922, 0x01, 0x06, 0x01, 0x00, 0, 0);
    axys_pci_test_cfg32(0, 5, 0, 0x10 + 5 * 4, 0xfe000000u); /* BAR5 mem32 */
    axys_pci_test_bar_mask(0, 5, 0, 5, 0xffffc000u);        /* 16 KiB window */
    assert(axys_pci_scan() == 2);
    d = axys_pci_find(0x01, 0x06);
    assert(d != NULL && d->bus == 0 && d->device == 5);
    assert(d->bar_count == 6);
    assert(d->bar_kind[5] == AXYS_PCI_BARK_MEM32);
    assert(d->bar_addr[5] == 0xfe000000u && d->bars[5] == 0xfe000000u);
    assert(d->bar_size[5] == 0x4000u);
    assert(axys_pci_find_id(0x8086, 0x2922) == d);
    assert(axys_pci_find_bdf(0, 5, 0) == d);
    assert(axys_pci_find_bdf(0, 5, 1) == NULL);
}

static void test_mem64_and_io_bars(void)
{
    const struct axys_pci_device *d;

    axys_pci_test_reset();
    fake_host_bridge();
    fake_fn(0, 2, 0, 0x1b36, 0x0010, 0x02, 0x00, 0x00, 0x01, 0, 0);
    axys_pci_test_cfg32(0, 2, 0, 0x10, 0x00000004u); /* BAR0: 64-bit, base low */
    axys_pci_test_cfg32(0, 2, 0, 0x14, 0x00000001u); /* BAR1: high half (= 4 GiB) */
    axys_pci_test_cfg32(0, 2, 0, 0x18, 0x0000c001u); /* BAR2: I/O ports */
    axys_pci_test_bar_mask(0, 2, 0, 0, 0xfc000000u);
    axys_pci_test_bar_mask(0, 2, 0, 1, 0xffffffffu);
    assert(axys_pci_scan() == 2);
    d = axys_pci_find_bdf(0, 2, 0);
    assert(d != NULL);
    assert(d->bar_kind[0] == AXYS_PCI_BARK_MEM64);
    assert(d->bar_addr[0] == 0x100000000ull);
    assert(d->bars[0] == 0x100000000ull);
    assert(d->bar_kind[1] == AXYS_PCI_BARK_NONE); /* high half is not a BAR */
    assert(d->bar_size[0] == 0x4000000u); /* lowest set bit of the pair mask */
    assert(d->bar_kind[2] == AXYS_PCI_BARK_IO);
    assert(d->bar_addr[2] == 0xc000u && d->bars[2] == AXYS_PCI_BAR_NONE);
}

static void test_bridge_and_caps(void)
{
    const struct axys_pci_device *d;

    axys_pci_test_reset();
    fake_host_bridge();
    /* PCI bridge, header layout 1: only 2 BARs; 0x18 holds bus numbers. */
    fake_fn(0, 1, 0, 0x8086, 0x244e, 0x06, 0x04, 0x00, 0x00, 1, 0);
    axys_pci_test_cfg32(0, 1, 0, 0x10, 0xfe100000u); /* BAR0 mem32 */
    axys_pci_test_cfg32(0, 1, 0, 0x14, 0xfe200000u); /* BAR1 mem32 */
    axys_pci_test_cfg32(0, 1, 0, 0x18, 0x00010100u); /* primary 0, secondary 1 */
    axys_pci_test_cfg32(0, 1, 0, 0x34, 0x50u);       /* cap list at 0x50 */
    axys_pci_test_cfg32(0, 1, 0, 0x50, 0x00006005u); /* MSI -> 0x60 */
    axys_pci_test_cfg32(0, 1, 0, 0x60, 0x00000010u); /* PCIe -> end */
    axys_pci_test_bar_mask(0, 1, 0, 0, 0xfff00000u);
    axys_pci_test_bar_mask(0, 1, 0, 1, 0xfff00000u);
    /* Device behind the bridge on bus 1. */
    fake_fn(1, 0, 0, 0x1234, 0x1111, 0x03, 0x00, 0x00, 0x00, 0, 0);
    assert(axys_pci_scan() == 3);
    d = axys_pci_find_bdf(0, 1, 0);
    assert(d != NULL && d->bar_count == 2 && d->secondary_bus == 1);
    assert(d->bar_kind[0] == AXYS_PCI_BARK_MEM32 && d->bar_size[0] == 0x100000u);
    assert(axys_pci_find_capability(d, AXYS_PCI_CAP_MSI) == 0x50);
    assert(axys_pci_find_capability(d, AXYS_PCI_CAP_PCIE) == 0x60);
    assert(axys_pci_find_capability(d, AXYS_PCI_CAP_MSIX) == 0);
    assert(axys_pci_device_at(2)->bus == 1); /* bridge walk reached bus 1 */
}

static void test_cap_loop_terminates(void)
{
    const struct axys_pci_device *d;

    axys_pci_test_reset();
    fake_host_bridge();
    fake_fn(0, 9, 0, 0x8086, 0x1234, 0x02, 0x00, 0x00, 0x00, 0, 0);
    axys_pci_test_cfg32(0, 9, 0, 0x34, 0x50u);
    axys_pci_test_cfg32(0, 9, 0, 0x50, 0x00005009u); /* points at itself */
    assert(axys_pci_scan() == 2);
    d = axys_pci_find_bdf(0, 9, 0);
    assert(d != NULL);
    assert(d->cap_ptr == 0x50);
    assert(axys_pci_find_capability(d, AXYS_PCI_CAP_PM) == 0); /* terminates */
}

static void test_accessors_and_names(void)
{
    const struct axys_pci_device *d;

    axys_pci_test_reset();
    fake_host_bridge();
    fake_fn(0, 3, 0, 0x10ec, 0x8168, 0x02, 0x00, 0x00, 0x02, 0, 0);
    axys_pci_test_cfg32(0, 3, 0, 0x04, 0x00100007u); /* cmd 7, status cap-list */
    assert(axys_pci_scan() == 2);
    d = axys_pci_find_bdf(0, 3, 0);
    assert(d != NULL);
    assert(axys_pci_read8(d, 0x09) == 0x00); /* prog-if byte */
    assert(axys_pci_read16(d, 0x0a) == 0x0200); /* subclass:class little-endian */
    axys_pci_write8(d, 0x3c, 0x0au); /* interrupt line */
    assert(axys_pci_read8(d, 0x3c) == 0x0au);
    axys_pci_write16(d, 0x3c, 0x010au); /* line + pin are plain bytes */
    assert(axys_pci_read16(d, 0x3c) == 0x010au);
    /* The sanctioned command-register API preserves W1C status bits. */
    axys_pci_set_command(d, AXYS_PCI_CMD_IO | AXYS_PCI_CMD_MEMORY | AXYS_PCI_CMD_MASTER, 0);
    assert((axys_pci_read16(d, 0x04) & 0x0007u) == 0x0007u);
    axys_pci_set_command(d, 0, AXYS_PCI_CMD_IO);
    assert((axys_pci_read16(d, 0x04) & AXYS_PCI_CMD_IO) == 0u);
    assert((axys_pci_read16(d, 0x04) & (AXYS_PCI_CMD_MEMORY | AXYS_PCI_CMD_MASTER)) != 0u);
    assert((axys_pci_read16(d, 0x06) & 0x0010u) != 0u); /* status untouched */
    /* A raw dword write with status ones set clears them (W1C, as hardware). */
    axys_pci_write32(d, 0x04, 0xffff0007u);
    assert(axys_pci_read16(d, 0x06) == 0u);
    assert(axys_pci_class_name(0x02, 0x00) != NULL);
    assert(axys_pci_class_name(0xff, 0xff) != NULL);
    assert(axys_pci_vendor_name(0x10ec) != NULL && axys_pci_vendor_name(0xffff) != NULL);
}

static void test_format_and_rescan(void)
{
    char buf[2048];
    char tiny[16];
    axys_size_t n;

    axys_pci_test_reset();
    fake_host_bridge();
    fake_fn(0, 5, 0, 0x8086, 0x2922, 0x01, 0x06, 0x01, 0x00, 0, 0);
    axys_pci_test_cfg32(0, 5, 0, 0x24, 0xfe000000u);
    axys_pci_test_bar_mask(0, 5, 0, 5, 0xffffc000u);
    assert(axys_pci_scan() == 2);
    n = axys_pci_format_all(buf, sizeof(buf));
    assert(n > 0 && n < sizeof(buf) && buf[n] == '\0');
    assert(strstr(buf, "00:05.0") != NULL && strstr(buf, "SATA") != NULL);
    assert(strstr(buf, "BAR5") != NULL && strstr(buf, "16K") != NULL);
    n = axys_pci_format_all(tiny, sizeof(tiny));
    assert(n < sizeof(tiny) && tiny[sizeof(tiny) - 1] == '\0'); /* truncated, terminated */
    assert(axys_pci_format_all(NULL, 0) == 0);
    /* Rescan: same devices, sizes reused, command register restored. */
    assert(axys_pci_scan() == 2);
    assert(axys_pci_find_bdf(0, 5, 0)->bar_size[5] == 0x4000u);
}

static void test_dma(void)
{
    /* Invalid arguments fail closed. */
    assert(axys_dma_alloc(0, 4096, 0, 0, NULL) == NULL);
    {
        axys_uint64_t phys = 0;

        assert(axys_dma_alloc(0, 4096, 0, 0, &phys) == NULL);
        assert(axys_dma_alloc(100, 3, 0, 0, &phys) == NULL);   /* not a power of two */
        assert(axys_dma_alloc(100, 4096, 3000, 0, &phys) == NULL); /* bad boundary */
        assert(axys_dma_alloc(70000, 4096, 65536, 0, &phys) == NULL); /* size > boundary */
    }
    /* Placement rules across sizes/alignments/boundaries. */
    for (unsigned i = 0; i < 200; ++i) {
        axys_uint64_t phys = 0;
        axys_size_t size = 1 + (i * 7919u) % 9000u;
        axys_size_t align = 1u << (i % 13); /* 1 .. 4096 */
        void *p;

        if (align < 16) {
            align = 16;
        }
        p = axys_dma_alloc(size, align, 65536, 0, &phys);
        assert(p != NULL && phys != 0);
        assert(((uintptr_t)p & (align - 1u)) == 0u);
        assert(phys == (axys_uint64_t)(uintptr_t)p); /* identity */
        assert(phys / 65536u == (phys + size - 1u) / 65536u); /* one 64K block */
        for (axys_size_t k = 0; k < size; ++k) {
            assert(((unsigned char *)p)[k] == 0); /* zeroed */
        }
        ((unsigned char *)p)[0] = 0xa5;
        axys_dma_free(p);
    }
    axys_dma_free(NULL); /* ignored */
    {
        int x = 0;

        axys_dma_free(&x); /* unknown pointer: ignored */
    }
    assert(axys_dma_live_allocations() == 0);
}

int main(void)
{
    test_empty_bus();
    test_ahci_decode_and_size();
    test_mem64_and_io_bars();
    test_bridge_and_caps();
    test_cap_loop_terminates();
    test_accessors_and_names();
    test_format_and_rescan();
    test_dma();
    printf("test_pci: ok\n");
    return 0;
}
