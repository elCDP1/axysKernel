#include "axys/e1000.h"
#include "axys/cpu.h"
#include "axys/dma.h"
#include "axys/pci.h"
#include "axys/pit.h"
#include "axys/printf.h"
#include "axys/sched.h"
#include "axys/spinlock.h"
#include "axys/string.h"
#include "axys/vmm.h"

/* Intel 82540EM PCI Ethernet, polled link layer.
 *
 * Register map (BAR0 MMIO, little-endian dwords): CTRL/STATUS for reset and
 * link, RAL/RAH for the factory MAC (programmed by firmware/QEMU, kept),
 * RCTL/RDLEN/RDH/RDT + RDBA for receive, TCTL/TIPG/TDLEN/TDH/TDT + TDBA for
 * transmit, MTA (zeroed: no multicast) and IMC (all masked: polled).
 * Descriptors are the 16-byte legacy layout; payloads live in dma buffers.
 * Short frames are padded to 60 bytes in software (the chip only appends the
 * 4-byte CRC via IFCS, and RCTL.SECRC strips it back off on receive). */

/* Control / status. */
#define REG_CTRL 0x0000u
#define REG_STATUS 0x0008u
#define CTRL_RST (1u << 26)
#define CTRL_SLU (1u << 6)
#define STATUS_LU (1u << 1)
#define STATUS_FD (1u << 0)
#define STATUS_SPEED_SHIFT 6u
#define STATUS_SPEED_MASK (3u << 6)

/* Interrupts (all masked: polled). */
#define REG_IMC 0x00d8u

/* Receive. */
#define REG_RCTL 0x0100u
#define RCTL_EN (1u << 1)
#define RCTL_BAM (1u << 15)
#define RCTL_SECRC (1u << 26)
#define REG_RDLEN 0x2808u
#define REG_RDH 0x2810u
#define REG_RDT 0x2818u
#define REG_RDBAL 0x2800u
#define REG_RDBAH 0x2804u
#define REG_RAL0 0x5400u
#define REG_RAH0 0x5404u
#define RAH_AV (1u << 31)
#define REG_MTA 0x5200u
#define MTA_ENTRIES 128u

/* Transmit. */
#define REG_TCTL 0x0400u
#define TCTL_EN (1u << 1)
#define TCTL_PSP (1u << 3)
#define TCTL_CT (0x0fu << 4)
#define TCTL_COLD (0x200u << 12)
#define REG_TIPG 0x0410u
#define TIPG_COPPER ((10u << 20) | (10u << 10) | 10u)
#define REG_TDLEN 0x3808u
#define REG_TDH 0x3810u
#define REG_TDT 0x3818u
#define REG_TDBAL 0x3800u
#define REG_TDBAH 0x3804u

/* Descriptor bits. */
#define TX_CMD_EOP (1u << 0)
#define TX_CMD_IFCS (1u << 1)
#define TX_CMD_RS (1u << 3)
#define TX_STATUS_DD (1u << 0)
#define RX_STATUS_DD (1u << 0)
#define RX_STATUS_EOP (1u << 1)

#define RING_COUNT 128u
#define RX_BUFFER 2048u
#define TX_BUFFER 2048u
#define MIN_FRAME 60u

struct tx_desc {
    axys_uint64_t address;
    axys_uint16_t length;
    axys_uint8_t cso;
    axys_uint8_t command;
    axys_uint8_t status;
    axys_uint8_t css;
    axys_uint16_t special;
} __attribute__((packed));

struct rx_desc {
    axys_uint64_t address;
    axys_uint16_t length;
    axys_uint16_t checksum;
    axys_uint8_t status;
    axys_uint8_t errors;
    axys_uint16_t special;
} __attribute__((packed));

AXYS_STATIC_ASSERT(sizeof(struct tx_desc) == 16u, tx_desc_is_16_bytes);
AXYS_STATIC_ASSERT(sizeof(struct rx_desc) == 16u, rx_desc_is_16_bytes);

static axys_uint64_t mmio;
static int present;
static char summary[192];
static struct axys_spinlock net_lock;

static struct tx_desc *tx_ring;
static axys_uint64_t tx_ring_phys;
static struct rx_desc *rx_ring;
static axys_uint64_t rx_ring_phys;
static axys_uint8_t *tx_buffers;
static axys_uint8_t *rx_buffers;
static axys_uint32_t tx_tail;
static axys_uint32_t rx_next;

static axys_uint8_t mac[6];
static axys_uint32_t link_mbps;
static int tx_busy;

/* Our IPv4 address for user-space stacks. QEMU user-network default until
 * /bin/dhcp (or root) changes it; the link layer never looks at it. */
static axys_uint8_t our_ip[4] = {10, 0, 2, 15};

/* Kernel RX frame queue for NET_RECV. */
static axys_uint8_t rx_queue[AXYS_NET_QUEUE][AXYS_NET_FRAME_MAX];
static axys_uint16_t rx_queue_len[AXYS_NET_QUEUE];
static axys_uint32_t rx_queue_head;
static axys_uint32_t rx_queue_count;
static axys_uint64_t tx_count;
static axys_uint64_t rx_count;
static axys_uint64_t rx_drop_count;

static axys_uint32_t reg(axys_uint32_t offset)
{
    axys_uint32_t v = *(volatile axys_uint32_t *)(axys_uintptr_t)(mmio + offset);

    __sync_synchronize();
    return v;
}

static void reg_write(axys_uint32_t offset, axys_uint32_t v)
{
    *(volatile axys_uint32_t *)(axys_uintptr_t)(mmio + offset) = v;
    __sync_synchronize();
}

static axys_uint64_t deadline_ms(axys_uint64_t ms)
{
    return axys_pit_millis() + ms;
}

static int expired(axys_uint64_t deadline)
{
    return (axys_int64_t)(deadline - axys_pit_millis()) <= 0;
}

/* Move newly completed receive descriptors into the frame queue. */
static void drain_rx(void)
{
    for (;;) {
        struct rx_desc *d = &rx_ring[rx_next];

        __sync_synchronize();
        if ((d->status & RX_STATUS_DD) == 0u) {
            return; /* nothing more completed */
        }
        if ((d->status & RX_STATUS_EOP) != 0u && d->length >= 14u &&
            d->length <= RX_BUFFER) {
            if (rx_queue_count < AXYS_NET_QUEUE) {
                axys_uint32_t slot = (rx_queue_head + rx_queue_count) % AXYS_NET_QUEUE;

                axys_memcpy(rx_queue[slot], rx_buffers + (axys_size_t)rx_next * RX_BUFFER,
                            d->length);
                rx_queue_len[slot] = d->length;
                ++rx_queue_count;
                ++rx_count;
            } else {
                ++rx_drop_count;
            }
        } else {
            ++rx_drop_count; /* runt, oversize or fragmented: not ours to keep */
        }
        d->status = 0;
        __sync_synchronize();
        reg_write(REG_RDT, rx_next);
        rx_next = (rx_next + 1u) % RING_COUNT;
    }
}

static void poll_task(void *arg)
{
    (void)arg;
    for (;;) {
        axys_uint64_t flags;

        axys_task_sleep_ms(10u);
        flags = axys_spin_lock_irqsave(&net_lock);
        if (present) {
            drain_rx();
        }
        axys_spin_unlock_irqrestore(&net_lock, flags);
    }
}

int axys_net_present(void)
{
    return present;
}

const char *axys_e1000_summary(void)
{
    return summary;
}

int axys_net_send(const void *frame, axys_size_t length)
{
    struct tx_desc *d;
    axys_uint32_t slot;
    axys_uint64_t deadline;

    if (!present || frame == AXYS_NULL || length == 0 || length > AXYS_NET_FRAME_MAX) {
        return -1;
    }
    /* One in-flight transmission at a time, without ever masking interrupts
     * across the completion wait: the DD deadline is read off the IRQ-driven
     * PIT clock, which freezes under an irqsave lock and would turn a dead
     * transmitter into a whole-machine hang. */
    for (;;) {
        axys_uint64_t flags = axys_spin_lock_irqsave(&net_lock);

        if (!tx_busy) {
            tx_busy = 1;
            axys_spin_unlock_irqrestore(&net_lock, flags);
            break;
        }
        axys_spin_unlock_irqrestore(&net_lock, flags);
        axys_yield();
    }
    slot = tx_tail;
    d = &tx_ring[slot];
    if ((d->status & TX_STATUS_DD) == 0u) {
        /* Still owned by hardware: with a 128-deep FIFO ring this means a
         * stuck transmitter; refuse rather than overwrite live descriptors. */
        axys_uint64_t flags = axys_spin_lock_irqsave(&net_lock);

        tx_busy = 0;
        axys_spin_unlock_irqrestore(&net_lock, flags);
        return -1;
    }
    axys_memcpy(tx_buffers + (axys_size_t)slot * TX_BUFFER, frame, length);
    if (length < MIN_FRAME) {
        axys_memset(tx_buffers + (axys_size_t)slot * TX_BUFFER + length, 0, MIN_FRAME - length);
        length = MIN_FRAME;
    }
    d->length = (axys_uint16_t)length;
    d->cso = 0;
    d->command = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    d->status = 0;
    d->css = 0;
    d->special = 0;
    __sync_synchronize();
    tx_tail = (slot + 1u) % RING_COUNT;
    reg_write(REG_TDT, tx_tail);
    deadline = deadline_ms(100u);
    while ((d->status & TX_STATUS_DD) == 0u) {
        if (expired(deadline)) {
            axys_uint64_t flags = axys_spin_lock_irqsave(&net_lock);

            tx_busy = 0;
            axys_spin_unlock_irqrestore(&net_lock, flags);
            return -1;
        }
        axys_cpu_relax();
    }
    ++tx_count;
    {
        axys_uint64_t flags = axys_spin_lock_irqsave(&net_lock);

        tx_busy = 0;
        axys_spin_unlock_irqrestore(&net_lock, flags);
    }
    return (int)length;
}

int axys_net_recv(void *out, axys_size_t capacity)
{
    axys_uint64_t flags;
    int rc = -1; /* empty */

    if (!present || out == AXYS_NULL) {
        return -1;
    }
    flags = axys_spin_lock_irqsave(&net_lock);
    drain_rx();
    if (rx_queue_count != 0) {
        axys_uint16_t len = rx_queue_len[rx_queue_head];
        axys_size_t n = len <= capacity ? (axys_size_t)len : capacity;

        /* POSIX default: a small buffer truncates, it does not drop. */
        axys_memcpy(out, rx_queue[rx_queue_head], n);
        rc = (int)n;
        rx_queue_head = (rx_queue_head + 1u) % AXYS_NET_QUEUE;
        --rx_queue_count;
    }
    axys_spin_unlock_irqrestore(&net_lock, flags);
    return rc;
}

void axys_net_stat(struct axys_net_stat *out)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&net_lock);

    if (out != AXYS_NULL) {
        for (unsigned i = 0; i < 6; ++i) {
            out->mac[i] = mac[i];
        }
        out->link = present ? 1u : 0u;
        out->pad = 0;
        out->speed_mbps = link_mbps;
        for (unsigned i = 0; i < 4; ++i) {
            out->ip[i] = our_ip[i];
            out->ip_pad[i] = 0;
        }
        out->tx_packets = tx_count;
        out->rx_packets = rx_count;
        out->rx_dropped = rx_drop_count;
    }
    axys_spin_unlock_irqrestore(&net_lock, flags);
}

void axys_net_set_addr(const axys_uint8_t ip[4])
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&net_lock);

    if (ip != AXYS_NULL) {
        for (unsigned i = 0; i < 4; ++i) {
            our_ip[i] = ip[i];
        }
    }
    axys_spin_unlock_irqrestore(&net_lock, flags);
}

int axys_e1000_init(void)
{
    const struct axys_pci_device *device;
    axys_uint64_t bar;
    axys_uint64_t bar_size;
    axys_uint32_t ral;
    axys_uint32_t rah;
    axys_uint64_t deadline;
    axys_uint64_t phys;

    present = 0;
    summary[0] = '\0';
    device = axys_pci_find(0x02u, 0x00u);
    if (device == AXYS_NULL || device->bars[0] == AXYS_PCI_BAR_NONE) {
        axys_snprintf(summary, sizeof(summary), "no Ethernet controller on the bus");
        return 0; /* absent is fine */
    }
    bar = device->bars[0];
    bar_size = device->bar_size[0] != 0 ? device->bar_size[0] : 0x20000u;
    if (bar_size > 0x20000u) {
        bar_size = 0x20000u;
    }
    if (bar > axys_vmm_physical_limit() - bar_size ||
        axys_vmm_map_mmio_range(bar, (axys_uint32_t)bar_size) != 0) {
        axys_snprintf(summary, sizeof(summary), "NIC BAR0 is not a reserved, mappable range");
        return -1;
    }
    axys_pci_enable_bus_master(device);
    mmio = bar;

    reg_write(REG_IMC, 0xffffffffu); /* polled: no interrupts */
    reg_write(REG_CTRL, reg(REG_CTRL) | CTRL_RST);
    deadline = deadline_ms(1000u);
    while ((reg(REG_CTRL) & CTRL_RST) != 0u) {
        if (expired(deadline)) {
            axys_snprintf(summary, sizeof(summary), "NIC reset timed out");
            return -1;
        }
        axys_cpu_relax();
    }
    ral = reg(REG_RAL0);
    rah = reg(REG_RAH0);
    if (rah == 0xffffffffu) {
        axys_snprintf(summary, sizeof(summary), "NIC has no factory MAC");
        return -1;
    }
    mac[0] = (axys_uint8_t)ral;
    mac[1] = (axys_uint8_t)(ral >> 8);
    mac[2] = (axys_uint8_t)(ral >> 16);
    mac[3] = (axys_uint8_t)(ral >> 24);
    mac[4] = (axys_uint8_t)rah;
    mac[5] = (axys_uint8_t)(rah >> 8);
    if ((mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) == 0u) {
        axys_snprintf(summary, sizeof(summary), "NIC reports a zero MAC");
        return -1;
    }
    reg_write(REG_RAH0, rah | RAH_AV); /* keep receiving our own address */

    /* Link up, then wait for it. */
    reg_write(REG_CTRL, reg(REG_CTRL) | CTRL_SLU);
    deadline = deadline_ms(3000u);
    while ((reg(REG_STATUS) & STATUS_LU) == 0u) {
        if (expired(deadline)) {
            axys_snprintf(summary, sizeof(summary), "NIC link stayed down");
            return -1;
        }
        axys_cpu_relax();
    }
    {
        axys_uint32_t speed = (reg(REG_STATUS) & STATUS_SPEED_MASK) >> STATUS_SPEED_SHIFT;

        link_mbps = speed == 2u ? 1000u : (speed == 1u ? 100u : 10u);
    }

    for (unsigned i = 0; i < MTA_ENTRIES; ++i) {
        reg_write(REG_MTA + i * 4u, 0); /* no multicast groups */
    }

    /* Receive ring: every descriptor points at a ready 2 KiB buffer. */
    rx_ring = axys_dma_alloc(RING_COUNT * sizeof(struct rx_desc), 16, 0, AXYS_DMA_32BIT,
                             &rx_ring_phys);
    rx_buffers = axys_dma_alloc(RING_COUNT * RX_BUFFER, 4096, 0, AXYS_DMA_32BIT, &phys);
    if (rx_ring == AXYS_NULL || rx_buffers == AXYS_NULL) {
        axys_snprintf(summary, sizeof(summary), "NIC has no memory for RX");
        return -1;
    }
    for (unsigned i = 0; i < RING_COUNT; ++i) {
        rx_ring[i].address = phys + (axys_uint64_t)i * RX_BUFFER;
        rx_ring[i].length = 0;
        rx_ring[i].checksum = 0;
        rx_ring[i].status = 0;
        rx_ring[i].errors = 0;
        rx_ring[i].special = 0;
    }
    rx_next = 0;
    reg_write(REG_RDBAL, (axys_uint32_t)rx_ring_phys);
    reg_write(REG_RDBAH, (axys_uint32_t)(rx_ring_phys >> 32));
    reg_write(REG_RDLEN, RING_COUNT * (axys_uint32_t)sizeof(struct rx_desc));
    reg_write(REG_RDH, 0);
    reg_write(REG_RDT, RING_COUNT - 1u);
    reg_write(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

    /* Transmit ring, idle. */
    tx_ring = axys_dma_alloc(RING_COUNT * sizeof(struct tx_desc), 16, 0, AXYS_DMA_32BIT,
                             &tx_ring_phys);
    tx_buffers = axys_dma_alloc(RING_COUNT * TX_BUFFER, 4096, 0, AXYS_DMA_32BIT, &phys);
    if (tx_ring == AXYS_NULL || tx_buffers == AXYS_NULL) {
        axys_snprintf(summary, sizeof(summary), "NIC has no memory for TX");
        return -1;
    }
    for (unsigned i = 0; i < RING_COUNT; ++i) {
        tx_ring[i].address = phys + (axys_uint64_t)i * TX_BUFFER;
        tx_ring[i].length = 0;
        tx_ring[i].cso = 0;
        tx_ring[i].command = 0;
        tx_ring[i].status = TX_STATUS_DD; /* free: owned by software */
        tx_ring[i].css = 0;
        tx_ring[i].special = 0;
    }
    tx_tail = 0;
    reg_write(REG_TDBAL, (axys_uint32_t)tx_ring_phys);
    reg_write(REG_TDBAH, (axys_uint32_t)(tx_ring_phys >> 32));
    reg_write(REG_TDLEN, RING_COUNT * (axys_uint32_t)sizeof(struct tx_desc));
    reg_write(REG_TDH, 0);
    reg_write(REG_TDT, 0);
    reg_write(REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);
    reg_write(REG_TIPG, TIPG_COPPER);

    present = 1;
    {
        struct axys_task *task;

        axys_snprintf(summary, sizeof(summary), "%02x:%02x.%u %02x:%02x:%02x:%02x:%02x:%02x %uMb/s",
                      device->bus, device->device, device->function, mac[0], mac[1], mac[2],
                      mac[3], mac[4], mac[5], link_mbps);
        task = axys_task_create("netpoll", poll_task, AXYS_NULL);
        if (task != AXYS_NULL) {
            axys_task_detach(task);
        }
    }
    return 0;
}
