#include "axys/usb_desc.h"
#include "axys/xhci.h"
#include "axys/cpu.h"
#include "axys/dma.h"
#include "axys/input.h"
#include "axys/pci.h"
#include "axys/pit.h"
#include "axys/printf.h"
#include "axys/sched.h"
#include "axys/spinlock.h"
#include "axys/string.h"
#include "axys/usb_hid.h"
#include "axys/vmm.h"

/* xHCI USB host controller, polled, with HID keyboard support.
 *
 * The register map below follows xHCI 1.x: capability registers at BAR0+0,
 * operational registers at BAR0+CAPLENGTH, one 0x10 port block per root-hub
 * port at +0x400, doorbells at +DBOFF (one dword per slot, endpoint in the
 * target field) and the runtime/interrupter registers at +RTSOFF.
 *
 * Everything the controller reads or writes is a physical address in
 * identity-mapped RAM (see axys_dma_alloc): rings are 16-byte TRBs, contexts
 * are little-endian dwords built with explicit stores like the AHCI driver.
 * Commands run one at a time during enumeration; interrupt transfers keep one
 * outstanding TRB per keyboard, completed through the event ring by a polling
 * kernel task. No interrupts, no isochronous, no hubs behind hubs, no USB
 * mass storage yet. */

/* Capability registers (BAR0 + offset). */
#define CAP_CAPLENGTH 0x00u
#define CAP_VERSION 0x02u
#define CAP_HCS1 0x04u
#define CAP_HCS2 0x08u
#define CAP_HCC1 0x10u
#define CAP_DBOFF 0x14u
#define CAP_RTSOFF 0x18u

#define HCS1_SLOTS(h) ((axys_uint32_t)(h)&0xffu)
#define HCS1_PORTS(h) ((axys_uint32_t)((h) >> 24) & 0xffu)

/* xHCI 1.1+ splits the scratchpad-buffer count across HCSPARAMS2 only:
 * bits 25:21 hold the high 5 bits, bits 31:27 the low 5 bits (the old
 * HCCPARAMS1[31:28] field is gone and reading it yields unrelated bits). */
#define HCS2_SCRATCH_HI(h) (((axys_uint32_t)(h) >> 21) & 0x1fu)
#define HCS2_SCRATCH_LO(h) (((axys_uint32_t)(h) >> 27) & 0x1fu)
#define HCC1_64BIT(h) (((h) & 1u) != 0u)

/* Operational registers (OP = BAR0 + caplength). */
#define OP_USBCMD 0x00u
#define OP_USBSTS 0x04u
#define OP_PAGESIZE 0x08u
#define OP_DNCTRL 0x14u
#define OP_CRCR 0x18u
#define OP_DCBAAP 0x30u
#define OP_CONFIG 0x38u
#define OP_PORTSC 0x400u
#define OP_PORT_STRIDE 0x10u

#define CMD_RS 0x00000001u
#define CMD_HCRST 0x00000002u
#define CMD_INTE 0x00000004u
#define STS_HALTED 0x00000001u
#define STS_CNR 0x00000800u
#define CRCR_RCS 0x00000001u

#define PORTSC_CCS 0x00000001u
#define PORTSC_PED 0x00000002u
#define PORTSC_PR 0x00000010u
#define PORTSC_PP 0x00000200u /* Port Power: a write of 0 de-powers the port */
#define PORTSC_SPEED_SHIFT 10u
#define PORTSC_SPEED_MASK 0x00003c00u
#define PORTSC_PRC 0x00200000u
#define PORTSC_CHANGES 0x00fe0000u /* CSC,PEC,WRC,OCC,PRC,PLC,CEC: all W1C */

#define SPEED_FS 1u
#define SPEED_LS 2u
#define SPEED_HS 3u
#define SPEED_SS 4u

/* Runtime registers (RT = BAR0 + rtsoff), interrupter 0. */
#define RT_IMAN 0x20u
#define RT_IMOD 0x24u
#define RT_ERSTSZ 0x28u
#define RT_ERSTBA 0x30u
#define RT_ERDP 0x38u

#define IMAN_IP 0x00000001u
#define ERDP_EHB 0x00000008u

/* TRB types. */
#define TRB_NORMAL 1u
#define TRB_SETUP 2u
#define TRB_DATA 3u
#define TRB_STATUS 4u
#define TRB_LINK 6u
#define TRB_ENABLE_SLOT 9u
#define TRB_DISABLE_SLOT 10u
#define TRB_ADDRESS_DEVICE 11u
#define TRB_CONFIGURE_EP 12u
#define TRB_RESET_EP 14u
#define TRB_TRANSFER_EVENT 32u
#define TRB_CMD_COMPLETE 33u

#define TRB_CYCLE 0x00000001u
#define TRB_CHAIN 0x00000010u
#define TRB_IOC 0x00000020u
#define TRB_ISP 0x00000004u
#define TRB_IDT 0x00000040u
#define TRB_TC 0x00000002u
#define TRB_DIR_IN 0x00010000u
/* Setup TRB Transfer Type (bits 17:16): 0 = no data, 2 = OUT stage, 3 = IN
 * stage. Swapping IN/OUT here makes every control transfer with a data stage
 * fail: the direction the controller expects is set in stone. */
#define TRB_TRT_IN 0x00030000u
#define TRB_TRT_OUT 0x00020000u

/* Completion codes. */
#define CC_SUCCESS 1u
#define CC_SHORT_PACKET 13u

/* USB standard requests. */
#define REQ_SET_CONFIGURATION 0x09u
#define DESC_DEVICE 0x01u
#define DESC_CONFIG 0x02u
#define DESC_HID_REPORT 0x22u
#define HID_REQ_GET_REPORT_DESC 0x06u
#define HID_REQ_SET_IDLE 0x0au
#define HID_REQ_SET_PROTOCOL 0x0bu

/* Endpoint context fields. */
#define EP_TYPE_CONTROL 4u
#define EP_TYPE_INTR_IN 7u
#define EP_CERR_3 (3u << 1)

#define MAX_SLOTS 8u
#define MAX_KBDS 4u
#define RING_TRBS 64u
#define EVENT_TRBS 256u
#define CTRL_BUFFER 512u
#define HID_BUFFER 64u

struct trb {
    axys_uint64_t param;
    axys_uint32_t status;
    axys_uint32_t control;
};

struct erst_entry {
    axys_uint64_t base;
    axys_uint32_t size;
    axys_uint32_t reserved;
};

struct ring {
    struct trb *trbs;      /* virtual */
    axys_uint64_t phys;
    axys_uint32_t enqueue; /* next slot to fill */
    axys_uint32_t cycle;   /* producer cycle bit */
};

/* One enumerated device. Keyboards additionally own an interrupt ring;
 * mass-storage devices own two bulk rings. */
struct xhci_dev {
    int used;
    int is_kbd;
    axys_uint8_t slot;
    void *dcbaa_buf; /* 1 KiB Output Device Context for `slot`, 0 = none */
    axys_uint8_t speed;
    axys_uint8_t root_port;
    axys_uint32_t route;
    axys_uint8_t parent_slot; /* Transaction Translator hub slot: only set under a high-speed hub */
    axys_uint8_t parent_port;
    axys_uint8_t hub_slot;    /* the hub this device hangs off (0 = root port): its identity in the tree */
    axys_uint8_t hub_port;
    unsigned depth;
    int is_hub;
    axys_uint8_t hub_ports;
    axys_uint32_t ep0_mps;
    struct ring ep0;
    /* Keyboard interrupt endpoint. */
    axys_uint8_t intr_dci;
    struct ring intr;
    axys_uint8_t *intr_buf;
    axys_uint64_t intr_phys;
    axys_uint64_t pending; /* TRB phys awaiting completion, 0 = none */
    struct axys_hid_kbd decoder;
    /* Bulk endpoints (mass storage). */
    int has_bulk;
    axys_uint8_t bulk_out_dci;
    axys_uint8_t bulk_in_dci;
    struct ring bulk_out;
    struct ring bulk_in;
    axys_uint32_t bulk_mps;
};

/* Configured Bulk-Only mass-storage slots (into devs[] by slot is enough,
 * but this keeps the disk layer from scanning). */
static struct xhci_dev devs[MAX_SLOTS];
static int kbd_count;

/* Configured Bulk-Only mass-storage endpoints, by slot. */
static struct usb_stor_slot {
    int used;
    axys_uint8_t slot;
    axys_uint8_t dci_out;
    axys_uint8_t dci_in;
} stor_slots[2];
static struct axys_spinlock xhci_lock;

/* Everything that submits TRBs and then waits for events -- control, bulk and
 * command transfers, enumeration, and the poller that drains the event ring --
 * runs under this transaction mutex. The event ring has a single consumer
 * position: if two contexts read it at once, each one throws away the other's
 * completion (it only recognises its own TRB), the owner times out, and a disk
 * write is reported failed. The flag is guarded by a spinlock held only for the
 * test-and-set: waits use the PIT clock, which stops under an irqsave lock, so
 * the lock itself must never be held across a wait. Not recursive: only public
 * entry points and the poller take it. */
static int xhci_busy;

static void xhci_acquire(void)
{
    for (;;) {
        axys_uint64_t flags = axys_spin_lock_irqsave(&xhci_lock);

        if (!xhci_busy) {
            xhci_busy = 1;
            axys_spin_unlock_irqrestore(&xhci_lock, flags);
            return;
        }
        axys_spin_unlock_irqrestore(&xhci_lock, flags);
        axys_yield();
    }
}

static void xhci_release(void)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&xhci_lock);

    xhci_busy = 0;
    axys_spin_unlock_irqrestore(&xhci_lock, flags);
}

static axys_uint64_t mmio;
static axys_uint64_t op_base;
static axys_uint64_t db_base;
static axys_uint64_t rt_base;
static axys_uint32_t max_ports;
static char summary[192];

static axys_uint64_t *dcbaa;
static axys_uint32_t dcbaa_count; /* highest slot id the array has room for */
static struct ring cmd_ring;
static struct erst_entry *erst;
static axys_uint64_t erst_phys;
static struct trb *event_seg;
static axys_uint64_t event_phys;
static axys_uint32_t event_dequeue;
static axys_uint32_t event_cycle;

static axys_uint32_t reg32(axys_uint64_t addr)
{
    axys_uint32_t v = *(volatile axys_uint32_t *)(axys_uintptr_t)addr;

    __sync_synchronize();
    return v;
}

static void reg32_write(axys_uint64_t addr, axys_uint32_t v)
{
    *(volatile axys_uint32_t *)(axys_uintptr_t)addr = v;
    __sync_synchronize();
}

static void reg64_write(axys_uint64_t addr, axys_uint64_t v)
{
    *(volatile axys_uint64_t *)(axys_uintptr_t)addr = v;
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

static void put32(axys_uint8_t *at, axys_size_t off, axys_uint32_t v)
{
    at[off] = (axys_uint8_t)v;
    at[off + 1] = (axys_uint8_t)(v >> 8);
    at[off + 2] = (axys_uint8_t)(v >> 16);
    at[off + 3] = (axys_uint8_t)(v >> 24);
}

/* ---- rings ------------------------------------------------------------ */

static int ring_init(struct ring *r)
{
    axys_uint64_t phys;
    struct trb *trbs =
        (struct trb *)axys_dma_alloc(RING_TRBS * sizeof(struct trb), 64, 0, AXYS_DMA_32BIT, &phys);

    if (trbs == AXYS_NULL) {
        return -1;
    }
    r->trbs = trbs;
    r->phys = phys;
    r->enqueue = 0;
    r->cycle = 1;
    /* Last slot links back with toggle-cycle so the producer wraps cleanly. */
    trbs[RING_TRBS - 1].param = phys;
    trbs[RING_TRBS - 1].status = 0;
    trbs[RING_TRBS - 1].control = (TRB_LINK << 10) | TRB_TC | r->cycle;
    return 0;
}

/* Fill the next TRB (never the link slot) and return its physical address. */
static axys_uint64_t ring_next(struct ring *r, const struct trb *t)
{
    axys_uint64_t at = r->phys + (axys_uint64_t)r->enqueue * sizeof(struct trb);

    r->trbs[r->enqueue] = *t;
    if (++r->enqueue == RING_TRBS - 1) {
        /* The link terminates *this* segment, so it must carry this
         * segment's cycle bit: the controller only toggles its expected
         * cycle after it has fetched a cycle-valid link (xHCI 1.2 4.11.5).
         * Writing the upcoming segment's bit here left the controller
         * waiting for a cycle that never arrives, wedging the ring forever
         * at the first wrap with no error event. */
        r->trbs[RING_TRBS - 1].control = (TRB_LINK << 10) | TRB_TC | r->cycle;
        r->enqueue = 0;
        r->cycle ^= 1u;
        __sync_synchronize();
    }
    return at;
}

static void ring_doorbell(axys_uint8_t slot, axys_uint8_t target)
{
    /* Target is the endpoint's DCI (1 for EP0), not DCI-1: the controller
     * indexes eps[target-1] and drops target 0 as a bad doorbell. */
    reg32_write(db_base + (axys_uint64_t)slot * 4u, target);
}

/* ---- events ------------------------------------------------------------ */

/* Consume one event TRB into *ev. Returns 1 with *ev filled, 0 when the ring
 * is empty. Advances the hardware dequeue pointer past consumed events. */
static int event_next(struct trb *ev)
{
    struct trb *slot = &event_seg[event_dequeue];

    __sync_synchronize();
    if (((slot->control & TRB_CYCLE) != 0u) != (event_cycle != 0u)) {
        return 0;
    }
    *ev = *slot;
    /* An event ring has no Link TRBs (xHCI 4.9.4): the controller walks the
     * Event Ring Segment Table, and with a single 256-entry segment it wraps
     * from the last entry to the first on its own, toggling its cycle bit. The
     * consumer does exactly the same. (An earlier version planted a Link TRB in
     * slot 255 and skipped that slot: the controller still writes events
     * there, so one event per wrap was lost and every wrap after the first
     * desynchronised the cycle bit -- the keyboard died after ~255 events.) */
    if (++event_dequeue == EVENT_TRBS) {
        event_dequeue = 0;
        event_cycle ^= 1u;
    }
    /* Publish the dequeue pointer without EHB (SeaBIOS-compatible): EHB tells
     * the controller the handler is still busy, which is wrong when idle. */
    reg64_write(rt_base + RT_ERDP, event_phys + (axys_uint64_t)event_dequeue * sizeof(struct trb));
    return 1;
}

static void event_ack_interrupt(void)
{
    axys_uint32_t iman = reg32(rt_base + RT_IMAN);

    if ((iman & IMAN_IP) != 0u) {
        reg32_write(rt_base + RT_IMAN, iman | IMAN_IP);
    }
}

static axys_uint32_t event_type(const struct trb *ev)
{
    return (ev->control >> 10) & 0x3fu;
}

static axys_uint8_t event_cc(const struct trb *ev)
{
    return (axys_uint8_t)(ev->status >> 24);
}

static axys_uint8_t event_slot(const struct trb *ev)
{
    return (axys_uint8_t)((ev->control >> 24) & 0xffu);
}

static void kbd_report(struct xhci_dev *dev);
static void kbd_arm(struct xhci_dev *dev);

/* Wait for the event whose TRB pointer is `match`. On success stores the
 * completion code and residual length. Returns 0, or -1 on timeout,
 * controller error, or a failed completion.
 *
 * Keyboard completions that arrive while waiting belong to someone else's
 * transfer: service them inline (report + re-arm) instead of dropping them,
 * or the keyboard would go silent the first time a hub status check overlaps
 * a keystroke. */
static int service_kbd_event(const struct trb *ev)
{
    if (event_type(ev) != TRB_TRANSFER_EVENT) {
        return 0;
    }
    for (unsigned i = 0; i < MAX_SLOTS; ++i) {
        struct xhci_dev *dev = &devs[i];

        if (dev->used && dev->is_kbd && dev->pending != 0 && ev->param == dev->pending) {
            dev->pending = 0;
            if (event_cc(ev) == CC_SUCCESS || event_cc(ev) == CC_SHORT_PACKET) {
                kbd_report(dev);
            }
            kbd_arm(dev);
            return 1;
        }
    }
    return 0;
}

static int wait_event(axys_uint64_t match, axys_uint8_t *code, axys_uint32_t *resid,
                      axys_uint64_t deadline)
{
    struct trb ev;

    for (;;) {
        while (event_next(&ev)) {
            axys_uint32_t type = event_type(&ev);

            event_ack_interrupt();
            if (type != TRB_CMD_COMPLETE && type != TRB_TRANSFER_EVENT) {
                continue;
            }
            if (service_kbd_event(&ev)) {
                continue;
            }
            if (ev.param != match) {
                continue;
            }
            *code = event_cc(&ev);
            *resid = type == TRB_TRANSFER_EVENT ? ev.status & 0xffffffu : 0u;
            if (*code == CC_SUCCESS || *code == CC_SHORT_PACKET) {
                return 0;
            }
            return -1;
        }
        if ((reg32(op_base + OP_USBSTS) & STS_CNR) != 0u) {
            return -1; /* controller went away mid-transfer */
        }
        if (expired(deadline)) {
            return -1;
        }
        axys_cpu_relax();
    }
}

/* ---- commands ------------------------------------------------------------ */

static int run_command(const struct trb *cmd, axys_uint64_t deadline)
{
    struct trb t = *cmd;
    axys_uint64_t at;
    axys_uint8_t code;
    axys_uint32_t resid;

    t.control = (t.control & ~TRB_CYCLE) | cmd_ring.cycle;
    at = ring_next(&cmd_ring, &t);
    __sync_synchronize();
    ring_doorbell(0, 0);
    return wait_event(at, &code, &resid, deadline);
}

/* Enable Slot completion carries the new slot ID in the event, so it needs
 * its own waiter rather than the pointer matcher above. */
static int enable_slot_cmd(axys_uint8_t *slot_out, axys_uint64_t deadline)
{
    struct trb t;
    axys_uint64_t at;
    struct trb ev;

    axys_memset(&t, 0, sizeof(t));
    t.control = (TRB_ENABLE_SLOT << 10) | cmd_ring.cycle;
    at = ring_next(&cmd_ring, &t);
    __sync_synchronize();
    ring_doorbell(0, 0);
    for (;;) {
        while (event_next(&ev)) {
            event_ack_interrupt();
            if (event_type(&ev) == TRB_CMD_COMPLETE && ev.param == at) {
                if (event_cc(&ev) != CC_SUCCESS) {
                    return -1;
                }
                *slot_out = event_slot(&ev);
                return *slot_out != 0 ? 0 : -1;
            }
            /* Keyboard reports that land while we wait belong to another
             * transfer: servicing them here is what wait_event() does, and
             * skipping it made the keyboard drop keystrokes (or go silent)
             * whenever an Enable Slot overlapped one. */
            (void)service_kbd_event(&ev);
        }
        if (expired(deadline)) {
            return -1;
        }
        axys_cpu_relax();
    }
}

/* Give a slot back after a failed enumeration. Without this every failed probe
 * left an enabled slot (and its 1 KiB DCBAA context) behind, so a few bogus
 * devices on the bus exhausted the controller's slot pool and USB died. */
static void release_slot(struct xhci_dev *dev, axys_uint64_t deadline)
{
    axys_uint8_t slot = dev->slot;
    void *ctx = dev->dcbaa_buf;

    dev->slot = 0;
    dev->dcbaa_buf = AXYS_NULL;
    if (slot != 0) {
        struct trb t;

        axys_memset(&t, 0, sizeof(t));
        /* The Slot ID lives in bits 31:24 of the command TRB. Without it the
         * controller is asked to disable slot 0, rejects it, and every slot
         * ever opened stays allocated until the pool runs dry. */
        t.control = (TRB_DISABLE_SLOT << 10) | ((axys_uint32_t)slot << 24) | cmd_ring.cycle;
        (void)run_command(&t, deadline); /* best effort: the slot is abandoned either way */
        if (dcbaa != AXYS_NULL && slot <= dcbaa_count) {
            dcbaa[slot] = 0;
        }
    }
    if (ctx != AXYS_NULL) {
        axys_dma_free(ctx);
    }
}

static void ring_free(struct ring *r)
{
    if (r->trbs != AXYS_NULL) {
        axys_dma_free(r->trbs);
    }
    r->trbs = AXYS_NULL;
    r->phys = 0;
    r->enqueue = 0;
    r->cycle = 1;
}

/* Give back everything a device owns and mark its table entry free: the slot
 * (Disable Slot also stops every endpoint, so freeing the rings afterwards is
 * safe), its Output Device Context, all transfer rings and the report buffer.
 * Every failure path of enumeration and every disconnect ends here, so a
 * device that comes and goes (or never works) cannot leak. */
static void abandon_device(struct xhci_dev *dev, axys_uint64_t deadline)
{
    release_slot(dev, deadline);
    ring_free(&dev->ep0);
    ring_free(&dev->intr);
    ring_free(&dev->bulk_out);
    ring_free(&dev->bulk_in);
    if (dev->intr_buf != AXYS_NULL) {
        axys_dma_free(dev->intr_buf);
        dev->intr_buf = AXYS_NULL;
    }
    dev->pending = 0;
    dev->is_kbd = 0;
    dev->is_hub = 0;
    dev->has_bulk = 0;
    dev->hub_ports = 0;
    dev->used = 0;
}

/* ---- contexts -------------------------------------------------------------- */

/* Endpoint Context "Interval" for an interrupt endpoint (xHCI 6.2.3.6). The
 * field is an exponent: the endpoint is serviced every 2^Interval microframes
 * (125 us each). A high-speed bInterval is already 2^(bInterval-1) microframes;
 * a full/low-speed bInterval counts 1 ms frames, i.e. 8 microframes each, and
 * the xHCI wants floor(log2(bInterval * 8)) limited to 3..10. Passing bInterval
 * through unchanged made a 10 ms keyboard poll every 128 ms. */
static axys_uint32_t ep_interval_field(axys_uint8_t speed, axys_uint32_t binterval)
{
    axys_uint32_t field = 0;

    if (binterval == 0u) {
        binterval = 1u;
    }
    if (speed == SPEED_HS || speed == SPEED_SS) {
        field = binterval - 1u;
        return field > 15u ? 15u : field;
    }
    for (axys_uint32_t v = binterval * 8u; v > 1u; v >>= 1) {
        ++field;
    }
    if (field < 3u) {
        field = 3u;
    }
    return field > 10u ? 10u : field;
}

static void build_input_context(axys_uint8_t *ctx, axys_uint8_t speed, axys_uint8_t root_port,
                                axys_uint32_t route, axys_uint8_t parent_slot,
                                axys_uint8_t parent_port, int is_hub, axys_uint8_t num_ports,
                                axys_uint8_t last_dci, axys_uint32_t ep0_mps,
                                axys_uint64_t ep0_dequeue, int with_slot, axys_uint32_t extra_dci,
                                axys_uint32_t extra_dw0, axys_uint32_t extra_dw1,
                                axys_uint64_t extra_tr_phys)
{
    /* DW0-1: drop/add masks. A0 = EP0 always; A1 = slot only for Address
     * Device (Configure Endpoint requires A0 plus the endpoint DCIs with A1
     * clear, or the controller rejects the command). */
    axys_memset(ctx, 0, 33u * 32u);
    put32(ctx, 4, (with_slot ? (1u << 1) : 0u) | (1u << 0) |
                  (extra_dci != 0 ? 1u << extra_dci : 0u));
    /* Slot context lives in the 32-byte unit at index 1 (offset 32), not
     * right after the 8-byte control pair: route string, speed, entries =
     * last DCI, root hub port (1-based on the wire). Putting it at offset 8
     * made the controller read zeros and reject every Address Device. */
    put32(ctx, 32, (route & 0xfffffu) | ((axys_uint32_t)speed << 20) |
                       (is_hub ? (1u << 26) : 0u) | ((axys_uint32_t)last_dci << 27));
    put32(ctx, 36, (axys_uint32_t)((axys_uint32_t)root_port + 1u) << 16 |
                       (is_hub ? (axys_uint32_t)num_ports << 24 : 0u));
    /* Parent hub TT fields, only below a HIGH-speed hub (split transactions);
     * under a full/low-speed hub there is no TT and they stay zero. */
    if (parent_slot != 0) {
        /* Caller passes a nonzero parent only for high-speed hubs. */
        put32(ctx, 40, (axys_uint32_t)parent_slot | ((axys_uint32_t)parent_port << 8));
    } else {
        put32(ctx, 40, 0);
    }
    /* EP0 context at DCI 1 (offset 2*32), with its current TR dequeue. */
    put32(ctx, 2u * 32u + 4, EP_CERR_3 | (EP_TYPE_CONTROL << 3) | (ep0_mps << 16));
    put32(ctx, 2u * 32u + 16, 8u); /* EP0 Average TRB Length: setup stages are 8 bytes */
    put32(ctx, 2u * 32u + 8, (axys_uint32_t)ep0_dequeue);
    put32(ctx, 2u * 32u + 12, (axys_uint32_t)(ep0_dequeue >> 32));
    if (extra_dci != 0) {
        axys_uint32_t off = (1u + extra_dci) * 32u;

        put32(ctx, off, extra_dw0);
        put32(ctx, off + 4, extra_dw1);
        /* TR dequeue pointer with DCS=1 in bit 0 (our first TRB carries
         * cycle 1). Rings sit below 4 GiB, so the high dword is zero. */
        put32(ctx, off + 8, ((axys_uint32_t)extra_tr_phys & ~0xfu) | 1u);
        put32(ctx, off + 12, (axys_uint32_t)(extra_tr_phys >> 32));
        {
            /* dword 4: Average TRB Length (15:0) and Max ESIT Payload Lo
             * (31:16). Real controllers reject periodic endpoints that leave
             * them zero; an interrupt endpoint moves at most one packet per
             * service interval, a bulk one is sized for typical transfers. */
            axys_uint32_t ep_type = (extra_dw1 >> 3) & 7u;
            axys_uint32_t mps = extra_dw1 >> 16;
            axys_uint32_t periodic = (ep_type == 3u || ep_type == 7u);

            put32(ctx, off + 16, (periodic ? mps : 1024u) | (periodic ? mps << 16 : 0u));
        }
    }
}

/* ---- control transfers ------------------------------------------------------- */

/* One setup [+ data] + status chain on `dev` EP0. `data_phys` is a DMA buffer
 * (from axys_dma_alloc) of at least `length` bytes, or ignored when length is
 * 0. Returns 0 on success. */
static int control_transfer(struct xhci_dev *dev, axys_uint8_t request_type, axys_uint8_t request,
                            axys_uint16_t value, axys_uint16_t index, axys_uint64_t data_phys,
                            axys_uint32_t length, int data_in, axys_uint64_t deadline)
{
    struct trb t;
    axys_uint8_t setup_bytes[8];
    axys_uint64_t status_at;
    axys_uint8_t code;
    axys_uint32_t resid;
    axys_uint32_t packets;

    setup_bytes[0] = request_type;
    setup_bytes[1] = request;
    setup_bytes[2] = (axys_uint8_t)value;
    setup_bytes[3] = (axys_uint8_t)(value >> 8);
    setup_bytes[4] = (axys_uint8_t)index;
    setup_bytes[5] = (axys_uint8_t)(index >> 8);
    setup_bytes[6] = (axys_uint8_t)length;
    setup_bytes[7] = (axys_uint8_t)(length >> 8);

    axys_memset(&t, 0, sizeof(t));
    for (unsigned i = 0; i < 8; ++i) {
        ((axys_uint8_t *)&t.param)[i] = setup_bytes[i];
    }
    t.status = 8;
    t.control = (TRB_SETUP << 10) | TRB_IDT | TRB_CHAIN |
                (length != 0 ? (data_in ? TRB_TRT_IN : TRB_TRT_OUT) : 0u);
    t.control = (t.control & ~TRB_CYCLE) | dev->ep0.cycle;
    ring_next(&dev->ep0, &t);
    if (length != 0) {
        packets = (length + dev->ep0_mps - 1u) / dev->ep0_mps;
        if (packets == 0) {
            packets = 1;
        }
        axys_memset(&t, 0, sizeof(t));
        t.param = data_phys;
        t.status = length | (packets << 17);
        t.control = (TRB_DATA << 10) | TRB_CHAIN | (data_in ? TRB_DIR_IN : 0u);
        t.control = (t.control & ~TRB_CYCLE) | dev->ep0.cycle;
        ring_next(&dev->ep0, &t);
    }
    axys_memset(&t, 0, sizeof(t));
    t.control = (TRB_STATUS << 10) | TRB_IOC | ((length == 0 || !data_in) ? TRB_DIR_IN : 0u);
    t.control = (t.control & ~TRB_CYCLE) | dev->ep0.cycle;
    status_at = ring_next(&dev->ep0, &t);
    __sync_synchronize();
    ring_doorbell(dev->slot, 1); /* DCI 1 = EP0 */
    return wait_event(status_at, &code, &resid, deadline);
}

/* ---- USB descriptors ----------------------------------------------------------- */

static axys_uint16_t desc_u16(const axys_uint8_t *p)
{
    return (axys_uint16_t)((axys_uint16_t)p[0] | ((axys_uint16_t)p[1] << 8));
}

/* Find a descriptor of `type` in a configuration blob (see usb_desc.h: every
 * length is validated, so the fixed offsets read afterwards stay in bounds). */
static int find_descriptor(const axys_uint8_t *blob, axys_size_t len, axys_uint8_t type, int instance)
{
    return axys_usb_desc_find(blob, len, type, instance);
}

/* ---- device bring-up -------------------------------------------------------------- */

static axys_uint32_t ep0_mps_for(axys_uint8_t speed)
{
    switch (speed) {
    case SPEED_LS: return 8;
    case SPEED_SS: return 512;
    default: return 64; /* FS and HS */
    }
}

static int address_device(struct xhci_dev *dev, axys_uint64_t deadline)
{
    axys_uint8_t *ctx;
    axys_uint64_t ctx_phys;
    struct trb cmd;
    int rc;

    ctx = axys_dma_alloc(33u * 32u, 64, 0, AXYS_DMA_32BIT, &ctx_phys);
    if (ctx == AXYS_NULL) {
        return -1;
    }
    /* Fresh EP0 ring: dequeue is the base with DCS = the producer cycle (1). */
    build_input_context(ctx, dev->speed, dev->root_port, dev->route, dev->parent_slot,
                        dev->parent_port, 0, 0, 1, dev->ep0_mps, dev->ep0.phys | 1u, 1, 0, 0, 0,
                        0);
    axys_memset(&cmd, 0, sizeof(cmd));
    cmd.param = ctx_phys;
    cmd.control = (TRB_ADDRESS_DEVICE << 10) | ((axys_uint32_t)dev->slot << 24);
    rc = run_command(&cmd, deadline);
    axys_dma_free(ctx);
    return rc;
}

/* Current EP0 TR dequeue, from the controller-owned output device context. */
static axys_uint64_t output_ep0_dequeue(axys_uint8_t slot)
{
    const axys_uint8_t *ctx = (const axys_uint8_t *)(axys_uintptr_t)dcbaa[slot];
    axys_uint32_t lo;
    axys_uint32_t hi;

    if (dcbaa[slot] == 0) {
        return 0;
    }
    lo = (axys_uint32_t)ctx[2u * 32u + 8] | ((axys_uint32_t)ctx[2u * 32u + 9] << 8) |
         ((axys_uint32_t)ctx[2u * 32u + 10] << 16) | ((axys_uint32_t)ctx[2u * 32u + 11] << 24);
    hi = (axys_uint32_t)ctx[2u * 32u + 12] | ((axys_uint32_t)ctx[2u * 32u + 13] << 8) |
         ((axys_uint32_t)ctx[2u * 32u + 14] << 16) | ((axys_uint32_t)ctx[2u * 32u + 15] << 24);
    return ((axys_uint64_t)hi << 32) | lo;
}

static int configure_ep(struct xhci_dev *dev, axys_uint32_t dci, axys_uint32_t dw0,
                         axys_uint32_t dw1, axys_uint64_t tr_phys, axys_uint64_t deadline)
{
    axys_uint8_t *ctx;
    axys_uint64_t ctx_phys;
    struct trb cmd;
    int rc;

    ctx = axys_dma_alloc(33u * 32u, 64, 0, AXYS_DMA_32BIT, &ctx_phys);
    if (ctx == AXYS_NULL) {
        return -1;
    }
    build_input_context(ctx, dev->speed, dev->root_port, dev->route, dev->parent_slot,
                        dev->parent_port, 0, 0, (axys_uint8_t)dci, dev->ep0_mps,
                        output_ep0_dequeue(dev->slot), 0, dci, dw0, dw1, tr_phys);
    axys_memset(&cmd, 0, sizeof(cmd));
    cmd.param = ctx_phys;
    cmd.control = (TRB_CONFIGURE_EP << 10) | ((axys_uint32_t)dev->slot << 24);
    rc = run_command(&cmd, deadline);
    axys_dma_free(ctx);
    return rc;
}

/* ---- public transport (mass storage, hubs) --------------------------------------- */

static struct xhci_dev *dev_by_slot(axys_uint8_t slot)
{
    for (unsigned i = 0; i < MAX_SLOTS; ++i) {
        if (devs[i].used && devs[i].slot == slot) {
            return &devs[i];
        }
    }
    return AXYS_NULL;
}

int axys_usb_control(axys_uint8_t slot, axys_uint8_t request_type, axys_uint8_t request,
                     axys_uint16_t value, axys_uint16_t index, axys_uint64_t data_phys,
                     axys_uint32_t length, int data_in, axys_uint32_t timeout_ms)
{
    struct xhci_dev *dev;
    int rc = -1;

    xhci_acquire();
    dev = dev_by_slot(slot);
    if (dev != AXYS_NULL) {
        rc = control_transfer(dev, request_type, request, value, index, data_phys, length, data_in,
                              deadline_ms(timeout_ms));
    }
    xhci_release();
    return rc;
}

/* One ≤4096-byte bulk transfer: a single TD so every completion matches
 * exactly one request (no chained-TD ambiguity on short packets). */
static int bulk_transfer_locked(axys_uint8_t slot, axys_uint8_t dci, axys_uint64_t data_phys,
                                axys_uint32_t length, int data_in, axys_uint32_t timeout_ms)
{
    struct xhci_dev *dev = dev_by_slot(slot);
    struct ring *ring;
    struct trb t;
    axys_uint64_t at;
    axys_uint8_t code;
    axys_uint32_t resid;

    if (dev == AXYS_NULL || !dev->has_bulk || length == 0 || length > 4096u) {
        return -1;
    }
    if (dci == dev->bulk_out_dci) {
        ring = &dev->bulk_out;
    } else if (dci == dev->bulk_in_dci) {
        ring = &dev->bulk_in;
    } else {
        return -1;
    }
    axys_memset(&t, 0, sizeof(t));
    t.param = data_phys;
    t.status = length | (1u << 17); /* TD size 1 */
    t.control = (TRB_NORMAL << 10) | TRB_IOC | (data_in ? (TRB_ISP | TRB_DIR_IN) : 0u);
    t.control = (t.control & ~TRB_CYCLE) | ring->cycle;
    at = ring_next(ring, &t);
    __sync_synchronize();
    ring_doorbell(slot, dci);
    return wait_event(at, &code, &resid, deadline_ms(timeout_ms));
}

int axys_usb_bulk_transfer(axys_uint8_t slot, axys_uint8_t dci, axys_uint64_t data_phys,
                           axys_uint32_t length, int data_in, axys_uint32_t timeout_ms)
{
    int rc;

    xhci_acquire();
    rc = bulk_transfer_locked(slot, dci, data_phys, length, data_in, timeout_ms);
    xhci_release();
    return rc;
}

static void reset_endpoint_locked(axys_uint8_t slot, axys_uint8_t dci)
{
    struct trb cmd;
    axys_uint64_t at;
    axys_uint8_t code;
    axys_uint32_t resid;

    if (dev_by_slot(slot) == AXYS_NULL || dci < 2u) {
        return;
    }
    axys_memset(&cmd, 0, sizeof(cmd));
    cmd.control = (TRB_RESET_EP << 10) | ((axys_uint32_t)slot << 24) |
                  ((axys_uint32_t)dci << 16);
    cmd.control = (cmd.control & ~TRB_CYCLE) | cmd_ring.cycle;
    at = ring_next(&cmd_ring, &cmd);
    __sync_synchronize();
    ring_doorbell(0, 0);
    (void)wait_event(at, &code, &resid, deadline_ms(2000u));
}

void axys_usb_reset_endpoint(axys_uint8_t slot, axys_uint8_t dci)
{
    xhci_acquire();
    reset_endpoint_locked(slot, dci);
    xhci_release();
}

int axys_usb_storage_slot(void)
{
    for (unsigned i = 0; i < 2u; ++i) {
        if (stor_slots[i].used) {
            return stor_slots[i].slot;
        }
    }
    return 0;
}

int axys_usb_storage_info(axys_uint8_t *slot, axys_uint8_t *dci_out, axys_uint8_t *dci_in)
{
    for (unsigned i = 0; i < 2u; ++i) {
        if (stor_slots[i].used) {
            if (slot != AXYS_NULL) {
                *slot = stor_slots[i].slot;
            }
            if (dci_out != AXYS_NULL) {
                *dci_out = stor_slots[i].dci_out;
            }
            if (dci_in != AXYS_NULL) {
                *dci_in = stor_slots[i].dci_in;
            }
            return 0;
        }
    }
    return -1;
}

/* Read a descriptor through EP0 into a DMA buffer. */
static int get_descriptor(struct xhci_dev *dev, axys_uint8_t reqtype, axys_uint8_t dtype,
                          axys_uint8_t dindex, axys_uint64_t buf_phys, axys_uint32_t length,
                          axys_uint16_t windex, axys_uint64_t deadline)
{
    return control_transfer(dev, reqtype, 0x06u, (axys_uint16_t)(((axys_uint16_t)dtype << 8) | dindex),
                            windex, buf_phys, length, 1, deadline);
}

/* Configure one HID keyboard behind `dev` (already addressed). Returns 0 when
 * the interrupt endpoint is up and polling. */
static int setup_keyboard(struct xhci_dev *dev, const axys_uint8_t *config, axys_size_t config_len,
                          axys_uint64_t deadline)
{
    struct axys_usb_kbd_desc kd;
    axys_uint8_t iface_no;
    axys_uint8_t ep_addr;
    axys_uint32_t ep_mps;
    axys_uint32_t ep_interval;
    axys_uint8_t *rep_buf;
    axys_uint64_t rep_phys;

    /* First boot-protocol keyboard interface (3/1/1) and the interrupt-IN
     * endpoint that belongs to it. */
    if (axys_usb_desc_keyboard(config, config_len, &kd) != 0) {
        return -1; /* not a boot keyboard */
    }
    iface_no = kd.iface;
    ep_addr = kd.ep_addr;
    ep_mps = kd.mps;
    ep_interval = kd.interval;
    /* Set Configuration (value from the config descriptor). */
    {
        struct trb t;
        axys_uint64_t status_at;
        axys_uint8_t code;
        axys_uint32_t resid;

        axys_memset(&t, 0, sizeof(t));
        {
            axys_uint8_t setup_bytes[8] = {0x00, 0x09, kd.config_value, 0x00, 0x00, 0x00, 0x00, 0x00};

            for (unsigned i = 0; i < 8; ++i) {
                ((axys_uint8_t *)&t.param)[i] = setup_bytes[i];
            }
        }
        t.status = 8;
        t.control = (TRB_SETUP << 10) | TRB_IDT | TRB_CHAIN;
        t.control = (t.control & ~TRB_CYCLE) | dev->ep0.cycle;
        ring_next(&dev->ep0, &t);
        axys_memset(&t, 0, sizeof(t));
        t.control = (TRB_STATUS << 10) | TRB_IOC | TRB_DIR_IN;
        t.control = (t.control & ~TRB_CYCLE) | dev->ep0.cycle;
        status_at = ring_next(&dev->ep0, &t);
        __sync_synchronize();
        ring_doorbell(dev->slot, 1); /* DCI 1 = EP0 */
        if (wait_event(status_at, &code, &resid, deadline) != 0) {
            return -1;
        }
    }
    /* HID Set Idle + fetch the report descriptor (best effort after this). */
    rep_buf = axys_dma_alloc(CTRL_BUFFER, 16, 0, AXYS_DMA_32BIT, &rep_phys);
    if (rep_buf == AXYS_NULL) {
        return -1;
    }
    /* Set Idle (ignore failure: some devices stall it and still work). */
    (void)control_transfer(dev, 0x21, HID_REQ_SET_IDLE, 0, iface_no, 0, 0, 0, deadline);
    (void)get_descriptor(dev, 0x81, DESC_HID_REPORT, 0, rep_phys, 64, iface_no, deadline);
    axys_dma_free(rep_buf);
    /* Interrupt ring + endpoint. DCI for EP1 IN is 3. */
    {
        axys_uint32_t dci = ((axys_uint32_t)(ep_addr & 0x0fu)) * 2u + 1u;
        axys_uint32_t dw0 = ep_interval_field(dev->speed, ep_interval) << 16;
        axys_uint32_t dw1 = EP_CERR_3 | (EP_TYPE_INTR_IN << 3) | (ep_mps << 16);

        if (ring_init(&dev->intr) != 0) {
            return -1;
        }
        dev->intr_buf = axys_dma_alloc(HID_BUFFER, 16, 0, AXYS_DMA_32BIT, &dev->intr_phys);
        if (dev->intr_buf == AXYS_NULL) {
            return -1;
        }
        if (configure_ep(dev, dci, dw0, dw1, dev->intr.phys, deadline) != 0) {
            return -1;
        }
        dev->intr_dci = (axys_uint8_t)dci;
    }
    return 0;
}

/* Configure the two bulk endpoints of a Bulk-Only mass-storage interface and
 * register the slot for the disk layer. Returns 0 on success. */
static int setup_storage(struct xhci_dev *dev, const axys_uint8_t *config, axys_size_t config_len,
                         axys_uint8_t iface_no, axys_uint64_t deadline)
{
    struct axys_usb_msc_desc md;
    axys_uint8_t out_ep;
    axys_uint8_t in_ep;
    axys_uint32_t out_mps;
    axys_uint32_t in_mps;
    axys_uint32_t out_dci;
    axys_uint32_t in_dci;
    axys_uint8_t *setup_buf;
    axys_uint64_t setup_phys;

    (void)iface_no;
    /* The Bulk-Only interface and its own pair of bulk endpoints. */
    if (axys_usb_desc_storage(config, config_len, &md) != 0) {
        return -1;
    }
    out_ep = md.out_ep;
    in_ep = md.in_ep;
    out_mps = md.out_mps;
    in_mps = md.in_mps;
    /* Set Configuration first: endpoints do not exist before it. */
    if (control_transfer(dev, 0x00, 0x09, (axys_uint16_t)md.config_value, 0, 0, 0, 0, deadline) != 0) {
        return -1;
    }
    out_dci = ((axys_uint32_t)(out_ep & 0x0fu)) * 2u;
    in_dci = ((axys_uint32_t)(in_ep & 0x0fu)) * 2u + 1u;
    if (out_dci < 2u || out_dci > 31u || in_dci < 2u || in_dci > 31u) {
        return -1;
    }
    if (ring_init(&dev->bulk_out) != 0 || ring_init(&dev->bulk_in) != 0) {
        return -1;
    }
    setup_buf = axys_dma_alloc(64, 16, 0, AXYS_DMA_32BIT, &setup_phys);
    if (setup_buf == AXYS_NULL) {
        return -1;
    }
    /* Get Max LUN (best effort: assume LUN 0 regardless). */
    (void)control_transfer(dev, 0xa1, 0xfe, 0, iface_no, setup_phys, 1, 1, deadline);
    axys_dma_free(setup_buf);
    /* One Configure Endpoint carrying EP0 plus both bulk endpoints. */
    {
        axys_uint8_t *ctx;
        axys_uint64_t ctx_phys;
        struct trb cmd;
        int rc;

        ctx = axys_dma_alloc(33u * 32u, 64, 0, AXYS_DMA_32BIT, &ctx_phys);
        if (ctx == AXYS_NULL) {
            return -1;
        }
        axys_memset(ctx, 0, 33u * 32u);
        put32(ctx, 4, (1u << 0) | (1u << out_dci) | (1u << in_dci));
        put32(ctx, 32,
              (dev->route & 0xfffffu) | ((axys_uint32_t)dev->speed << 20) |
                  ((in_dci > out_dci ? in_dci : out_dci) << 27));
        put32(ctx, 36, (axys_uint32_t)((axys_uint32_t)dev->root_port + 1u) << 16);
        put32(ctx, 40,
              (axys_uint32_t)dev->parent_slot | ((axys_uint32_t)dev->parent_port << 8));
        put32(ctx, 2u * 32u + 4,
              EP_CERR_3 | (EP_TYPE_CONTROL << 3) | (dev->ep0_mps << 16));
        put32(ctx, 2u * 32u + 8, (axys_uint32_t)output_ep0_dequeue(dev->slot));
        put32(ctx, 2u * 32u + 12, (axys_uint32_t)(output_ep0_dequeue(dev->slot) >> 32));
        put32(ctx, (1u + out_dci) * 32u, 0);
        put32(ctx, (1u + out_dci) * 32u + 4, EP_CERR_3 | (2u << 3) | (out_mps << 16));
        put32(ctx, (1u + out_dci) * 32u + 8,
              (axys_uint32_t)dev->bulk_out.phys | dev->bulk_out.cycle);
        put32(ctx, (1u + out_dci) * 32u + 12, (axys_uint32_t)(dev->bulk_out.phys >> 32));
        put32(ctx, (1u + in_dci) * 32u, 0);
        put32(ctx, (1u + in_dci) * 32u + 4, EP_CERR_3 | (6u << 3) | (in_mps << 16));
        put32(ctx, (1u + in_dci) * 32u + 8,
              (axys_uint32_t)dev->bulk_in.phys | dev->bulk_in.cycle);
        put32(ctx, (1u + in_dci) * 32u + 12, (axys_uint32_t)(dev->bulk_in.phys >> 32));
        axys_memset(&cmd, 0, sizeof(cmd));
        cmd.param = ctx_phys;
        cmd.control = (TRB_CONFIGURE_EP << 10) | ((axys_uint32_t)dev->slot << 24);
        rc = run_command(&cmd, deadline);
        axys_dma_free(ctx);
        if (rc != 0) {
            return -1;
        }
    }
    dev->has_bulk = 1;
    dev->bulk_out_dci = (axys_uint8_t)out_dci;
    dev->bulk_in_dci = (axys_uint8_t)in_dci;
    dev->bulk_mps = out_mps > in_mps ? out_mps : in_mps;
    for (unsigned i = 0; i < 2u; ++i) {
        if (!stor_slots[i].used) {
            stor_slots[i].used = 1;
            stor_slots[i].slot = dev->slot;
            stor_slots[i].dci_out = (axys_uint8_t)out_dci;
            stor_slots[i].dci_in = (axys_uint8_t)in_dci;
            break;
        }
    }
    return 0;
}

/* Queue one interrupt-IN transfer on a configured keyboard. */
static void kbd_arm(struct xhci_dev *dev)
{
    struct trb t;

    if (dev->pending != 0) {
        return;
    }
    axys_memset(&t, 0, sizeof(t));
    t.param = dev->intr_phys;
    t.status = 8 | (1u << 17); /* 8 bytes, TD size 1 */
    t.control = (TRB_NORMAL << 10) | TRB_IOC | TRB_ISP;
    t.control = (t.control & ~TRB_CYCLE) | dev->intr.cycle;
    dev->pending = ring_next(&dev->intr, &t);
    __sync_synchronize();
    ring_doorbell(dev->slot, dev->intr_dci);
}

/* Decode a completed boot report into console input. */
static void kbd_report(struct xhci_dev *dev)
{
    /* Worst case: 6 keys x 4 bytes of VT100 sequence. The decoder is bounded
     * by sizeof(out), so a hostile report can never write past it. */
    char out[24];
    int n = axys_hid_kbd_report(&dev->decoder, dev->intr_buf, out, (unsigned)sizeof(out));

    for (int i = 0; i < n; ++i) {
        axys_input_push_char(out[i]);
    }
}

/* ---- hubs ------------------------------------------------------------------------ */

static int enumerate_device(unsigned port, axys_uint32_t route, axys_uint8_t parent_slot,
                            axys_uint8_t parent_port, axys_uint8_t hub_slot, axys_uint8_t hub_port,
                            unsigned depth, axys_uint32_t speed);

/* Hub class requests (recipient device / port). */
#define HUB_GET_DESCRIPTOR 0x06u
#define HUB_SET_FEATURE 0x03u
#define HUB_CLEAR_FEATURE 0x01u
#define HUB_GET_STATUS 0x00u
#define HUB_PORT_CONNECTION 0u
#define HUB_PORT_ENABLE 1u
#define HUB_PORT_RESET 4u
#define HUB_PORT_POWER 8u
#define HUB_C_PORT_CONNECTION 16u
#define HUB_C_PORT_ENABLE 17u
#define HUB_C_PORT_RESET 20u
#define PORT_ST_CONNECTION (1u << 0)
#define PORT_ST_ENABLE (1u << 1)
#define PORT_ST_RESET (1u << 4)
#define PORT_ST_POWER (1u << 8)
#define PORT_ST_LOW_SPEED (1u << 9)
#define PORT_ST_HIGH_SPEED (1u << 10)

static int hub_request(struct xhci_dev *hub, axys_uint8_t reqtype, axys_uint8_t req,
                       axys_uint16_t value, axys_uint16_t index, axys_uint64_t data_phys,
                       axys_uint32_t length, int data_in, axys_uint64_t deadline)
{
    return control_transfer(hub, reqtype, req, value, index, data_phys, length, data_in,
                            deadline);
}

static int hub_port_feature(struct xhci_dev *hub, axys_uint8_t req, axys_uint16_t feature,
                            axys_uint8_t port, axys_uint64_t deadline)
{
    /* Recipient port (0x23): type class, recipient port. No data stage. */
    return hub_request(hub, 0x23u, req, feature, port, 0, 0, 0, deadline);
}

static int hub_port_status(struct xhci_dev *hub, axys_uint8_t port, axys_uint64_t buf_phys,
                           axys_uint64_t deadline)
{
    return hub_request(hub, 0xa3u, HUB_GET_STATUS, 0, port, buf_phys, 4, 1, deadline);
}

/* Tell the controller this slot is a hub (Hub flag + port count), keeping the
 * current EP0 state. Evaluate Context only touches what Add selects. */
static int evaluate_hub(struct xhci_dev *dev, axys_uint8_t nports, axys_uint64_t deadline)
{
    axys_uint8_t *ctx;
    axys_uint64_t ctx_phys;
    struct trb cmd;
    int rc;

    ctx = axys_dma_alloc(33u * 32u, 64, 0, AXYS_DMA_32BIT, &ctx_phys);
    if (ctx == AXYS_NULL) {
        return -1;
    }
    build_input_context(ctx, dev->speed, dev->root_port, dev->route, dev->parent_slot,
                        dev->parent_port, 1, nports, 1, dev->ep0_mps,
                        output_ep0_dequeue(dev->slot), 1, 0, 0, 0, 0);
    axys_memset(&cmd, 0, sizeof(cmd));
    cmd.param = ctx_phys;
    cmd.control = (13u << 10) | ((axys_uint32_t)dev->slot << 24); /* Evaluate Context */
    rc = run_command(&cmd, deadline);
    axys_dma_free(ctx);
    return rc;
}

/* Reset one hub downstream port. Returns the speed (1/2/3) or 0. */
static axys_uint32_t hub_reset_port(struct xhci_dev *hub, axys_uint8_t port,
                                    axys_uint8_t *status_buf, axys_uint64_t status_phys,
                                    axys_uint64_t deadline)
{
    axys_memset(status_buf, 0, 4);
    if (hub_port_status(hub, port, status_phys, deadline) != 0) {
        return 0;
    }
    if ((status_buf[0] & 0x01u) == 0u) {
        return 0; /* nothing connected */
    }
    (void)hub_port_feature(hub, HUB_CLEAR_FEATURE, HUB_C_PORT_CONNECTION, port, deadline);
    if (hub_port_feature(hub, HUB_SET_FEATURE, HUB_PORT_RESET, port, deadline) != 0) {
        return 0;
    }
    for (;;) {
        axys_memset(status_buf, 0, 4);
        if (hub_port_status(hub, port, status_phys, deadline) != 0) {
            return 0;
        }
        if ((status_buf[2] & 0x10u) != 0u || (status_buf[0] & 0x02u) != 0u) {
            break; /* C_RESET or ENABLE */
        }
        if (expired(deadline)) {
            return 0;
        }
        axys_cpu_relax();
    }
    (void)hub_port_feature(hub, HUB_CLEAR_FEATURE, HUB_C_PORT_RESET, port, deadline);
    (void)hub_port_feature(hub, HUB_CLEAR_FEATURE, HUB_C_PORT_ENABLE, port, deadline);
    axys_memset(status_buf, 0, 4);
    if (hub_port_status(hub, port, status_phys, deadline) != 0) {
        return 0;
    }
    if ((status_buf[0] & 0x02u) == 0u) {
        return 0;
    }
    /* wPortStatus is little-endian: buf[0..1] = status, buf[2..3] = change.
     * USB 2.0 table 11-21: bit 8 = PORT_POWER, bit 9 = PORT_LOW_SPEED,
     * bit 10 = PORT_HIGH_SPEED (both clear = full speed). In byte 1 those are
     * 0x01 (power: set on every powered port, so it says nothing about speed),
     * 0x02 (low speed) and 0x04 (high speed). Reading 0x01 as "low speed" made
     * every full-speed device behind a hub look low-speed. */
    if ((status_buf[1] & 0x04u) != 0u) {
        return SPEED_HS;
    }
    if ((status_buf[1] & 0x02u) != 0u) {
        return SPEED_LS;
    }
    return SPEED_FS;
}

static int hub_port_has_dev(axys_uint8_t hub_slot, axys_uint8_t hub_port)
{
    for (unsigned i = 0; i < MAX_SLOTS; ++i) {
        if (devs[i].used && devs[i].hub_slot == hub_slot && devs[i].hub_port == hub_port) {
            return 1;
        }
    }
    return 0;
}

static int setup_hub(struct xhci_dev *dev, const axys_uint8_t *config, axys_size_t config_len,
                     axys_uint8_t iface_no, unsigned port, axys_uint64_t deadline)
{
    axys_uint8_t *buf;
    axys_uint64_t buf_phys;
    axys_uint8_t nports = 0;
    int cfg_value = -1;

    (void)iface_no;
    for (int inst = 0;; ++inst) {
        int off = find_descriptor(config, config_len, 2, inst);

        if (off < 0) {
            break;
        }
        cfg_value = config[off + 5];
        break;
    }
    if (cfg_value < 0) {
        return -1;
    }
    buf = axys_dma_alloc(64, 16, 0, AXYS_DMA_32BIT, &buf_phys);
    if (buf == AXYS_NULL) {
        return -1;
    }
    if (control_transfer(dev, 0x00, 0x09, (axys_uint16_t)cfg_value, 0, 0, 0, 0, deadline) != 0) {
        axys_dma_free(buf);
        return -1;
    }
    /* Hub descriptor: bNbrPorts tells how many downstream ports exist. wValue
     * is (descriptor type << 8) | 0 = 0x2900; passing 0 asked for descriptor
     * type 0, which spec-strict hubs reject. */
    axys_memset(buf, 0, 64);
    if (axys_usb_control(dev->slot, 0xa0, HUB_GET_DESCRIPTOR, 0x2900u, 0, buf_phys, 8, 1,
                         deadline) != 0) {
        axys_dma_free(buf);
        return -1;
    }
    nports = buf[2];
    if (nports == 0 || nports > 16u) {
        axys_dma_free(buf);
        return -1;
    }
    if (evaluate_hub(dev, nports, deadline) != 0) {
        axys_dma_free(buf);
        return -1;
    }
    /* Power every port, then wait the hub's own power-settle time. */
    for (axys_uint8_t p = 1; p <= nports; ++p) {
        (void)hub_port_feature(dev, HUB_SET_FEATURE, HUB_PORT_POWER, p, deadline);
    }
    {
        unsigned settle = (unsigned)buf[5] * 2u + 20u; /* bPwrOn2PwrGood, 2ms units */

        if (settle > 1000u) {
            settle = 1000u;
        }
        axys_pit_sleep_ms(settle);
    }
    dev->is_hub = 1;
    dev->hub_ports = nports;
    for (axys_uint8_t p = 1; p <= nports; ++p) {
        axys_uint32_t speed = hub_reset_port(dev, p, buf, buf_phys, deadline_ms(2000u));

        if (speed == 0u) {
            continue;
        }
        if (dev->depth + 1u > 5u) {
            continue;
        }
        {
            axys_uint32_t route =
                dev->route | ((axys_uint32_t)(p & 0x0fu) << (dev->depth * 4u));
            /* TT parent routing only under a high-speed hub. */
            axys_uint8_t ps = dev->speed == SPEED_HS ? dev->slot : 0;
            axys_uint8_t pp = dev->speed == SPEED_HS ? p : 0;

            (void)enumerate_device(dev->root_port, route, ps, pp, dev->slot, p, dev->depth + 1u,
                                   speed);
        }
    }
    axys_dma_free(buf);
    axys_printf("usb: hub on port %u (%u ports)\n", port, nports);
    return 0;
}

/* ---- port bring-up --------------------------------------------------------------- */

static axys_uint64_t port_addr(unsigned port)
{
    return op_base + OP_PORTSC + (axys_uint64_t)port * OP_PORT_STRIDE;
}

/* Reset one connected root-hub port. Returns the negotiated speed, or 0. */
static axys_uint32_t reset_port(unsigned port)
{
    axys_uint64_t addr = port_addr(port);
    axys_uint32_t sc;
    axys_uint64_t deadline;

    sc = reg32(addr);
    if ((sc & PORTSC_CCS) == 0u) {
        return 0;
    }
    /* Clear stale change bits, then assert reset. Port Power is a plain RW
     * bit: writing a 0 across it de-powers the port (spec PORTSC), which left
     * the device dead on hardware that honours it. Always carry it over. */
    reg32_write(addr, (sc & PORTSC_CHANGES) | (sc & PORTSC_PP));
    reg32_write(addr, PORTSC_PR | (sc & PORTSC_PP));
    deadline = deadline_ms(2000u);
    for (;;) {
        sc = reg32(addr);
        if ((sc & PORTSC_PRC) != 0u) {
            break;
        }
        if (expired(deadline)) {
            return 0;
        }
        axys_cpu_relax();
    }
    reg32_write(addr, (sc & PORTSC_CHANGES) | (sc & PORTSC_PP)); /* clear PRC (and friends) */
    sc = reg32(addr);
    if ((sc & PORTSC_PED) == 0u) {
        return 0;
    }
    return (sc & PORTSC_SPEED_MASK) >> PORTSC_SPEED_SHIFT;
}

/* Enumerate one device: root-hub `port` (when depth == 0) or a hub downstream
 * port. `route` is the 20-bit route string, `depth` the hub depth (0 for
 * directly attached, max 5). Parent slot/port identify the hub above. */
static int enumerate_device(unsigned port, axys_uint32_t route, axys_uint8_t parent_slot,
                            axys_uint8_t parent_port, axys_uint8_t hub_slot, axys_uint8_t hub_port,
                            unsigned depth, axys_uint32_t speed)
{
    struct xhci_dev *dev = AXYS_NULL;
    axys_uint8_t slot = 0;
    axys_uint8_t *buf;
    axys_uint64_t buf_phys;
    axys_uint64_t deadline = deadline_ms(5000u);

    if (depth > 5u) {
        return -1;
    }
    for (unsigned i = 0; i < MAX_SLOTS; ++i) {
        if (!devs[i].used) {
            dev = &devs[i];
            break;
        }
    }
    if (dev == AXYS_NULL) {
        return -1;
    }
    if (ring_init(&dev->ep0) != 0) {
        return -1;
    }
    dev->used = 1;
    dev->is_kbd = 0;
    dev->slot = 0;
    dev->dcbaa_buf = AXYS_NULL;
    dev->speed = (axys_uint8_t)speed;
    dev->root_port = (axys_uint8_t)port;
    dev->route = route;
    dev->parent_slot = parent_slot;
    dev->parent_port = parent_port;
    dev->hub_slot = hub_slot;
    dev->hub_port = hub_port;
    dev->depth = depth;
    dev->is_hub = 0;
    dev->hub_ports = 0;
    dev->ep0_mps = ep0_mps_for((axys_uint8_t)speed);
    dev->intr_dci = 0;
    dev->pending = 0;
    dev->has_bulk = 0;
    dev->bulk_out_dci = 0;
    dev->bulk_in_dci = 0;
    axys_hid_kbd_init(&dev->decoder);
    if (enable_slot_cmd(&slot, deadline) != 0 || slot == 0 || slot > MAX_SLOTS) {
        dev->used = 0;
        return -1;
    }
    dev->slot = slot;
    /* The controller writes results into an Output Device Context that
     * software provides: one 1 KiB structure per slot, published before the
     * Address Device command that first uses it. */
    {
        axys_uint64_t phys;
        void *out = axys_dma_alloc(1024, 64, 0, AXYS_DMA_32BIT, &phys);

        if (out == AXYS_NULL) {
            abandon_device(dev, deadline);
            return -1;
        }
        dcbaa[slot] = phys;
        dev->dcbaa_buf = out;
    }
    if (address_device(dev, deadline) != 0) {
        abandon_device(dev, deadline);
        return -1;
    }
    buf = axys_dma_alloc(CTRL_BUFFER, 16, 0, AXYS_DMA_32BIT, &buf_phys);
    if (buf == AXYS_NULL) {
        abandon_device(dev, deadline);
        return -1;
    }
    /* Device descriptor: only the class triple matters here. */
    if (get_descriptor(dev, 0x80, DESC_DEVICE, 0, buf_phys, 18, 0, deadline) != 0) {
        axys_dma_free(buf);
        abandon_device(dev, deadline);
        return -1;
    }
    /* Configuration header first (to learn the total length), then all of it. */
    if (get_descriptor(dev, 0x80, DESC_CONFIG, 0, buf_phys, 9, 0, deadline) != 0) {
        axys_dma_free(buf);
        abandon_device(dev, deadline);
        return -1;
    }
    {
        axys_uint32_t total = desc_u16(buf + 2);

        if (total < 9u || total > CTRL_BUFFER) {
            axys_dma_free(buf);
            abandon_device(dev, deadline);
            return -1;
        }
        if (get_descriptor(dev, 0x80, DESC_CONFIG, 0, buf_phys, total, 0, deadline) != 0) {
            axys_dma_free(buf);
            abandon_device(dev, deadline);
            return -1;
        }
        {
            int ifoff = find_descriptor(buf, total, 4, 0);

            if (ifoff < 0) {
                axys_dma_free(buf);
                return 1; /* no interfaces: addressed, unconfigured */
            }
            {
                struct axys_usb_kbd_desc kd;
                struct axys_usb_msc_desc md;

                if (axys_usb_desc_keyboard(buf, total, &kd) == 0) {
                    if (setup_keyboard(dev, buf, total, deadline) != 0) {
                        axys_dma_free(buf);
                        return 1;
                    }
                    axys_dma_free(buf);
                    dev->is_kbd = 1;
                    if ((unsigned)kbd_count < MAX_KBDS) {
                        ++kbd_count;
                    }
                    kbd_arm(dev);
                    axys_printf("usb: keyboard on port %u (%d total)\n", port, kbd_count);
                    return 0;
                }
                if (axys_usb_desc_storage(buf, total, &md) == 0) {
                    int rc = setup_storage(dev, buf, total, md.iface, deadline);

                    axys_dma_free(buf);
                    if (rc != 0) {
                        return 1;
                    }
                    axys_printf("usb: storage on port %u (slot %u)\n", port, dev->slot);
                    return 0;
                }
            }
            if (buf[ifoff + 5] == 9 && buf[ifoff + 6] == 0 && buf[ifoff + 7] == 0) {
                int rc = setup_hub(dev, buf, total, buf[ifoff + 2], port, deadline);

                axys_dma_free(buf);
                return rc == 0 ? 0 : 1;
            }
            /* Addressed but unsupported: keep the slot, count nothing. */
            axys_dma_free(buf);
            return 1;
        }
    }
    return 1; /* unreachable, but keeps the shape obvious */
}

/* Enumerate one device on root-hub `port`: reset, then run the shared
 * enumerator with an empty route. */
static int enumerate_port(unsigned port, axys_uint32_t speed)
{
    (void)speed;
    {
        axys_uint32_t negotiated = reset_port(port);

        if (negotiated == 0u) {
            return -1;
        }
        return enumerate_device(port, 0, 0, 0, 0, 0, 0, negotiated);
    }
}

/* ---- polling task ---------------------------------------------------------------- */

static axys_uint64_t port_retry_at[32];

static int port_has_dev(unsigned port)
{
    for (unsigned i = 0; i < MAX_SLOTS; ++i) {
        if (devs[i].used && devs[i].root_port == port) {
            return 1;
        }
    }
    return 0;
}

static void poll_once(void)
{
    struct trb ev;

    while (event_next(&ev)) {
        event_ack_interrupt();
        service_kbd_event(&ev);
    }
}

/* Retry deadline per (hub, downstream port). Hubs can report up to 16 ports, so
 * the second dimension covers all of them. The first dimension is indexed by the
 * xHCI slot id, which is 1-based: sizing it MAX_SLOTS made slot MAX_SLOTS write
 * 128 bytes past the array. */
static axys_uint64_t hub_retry[MAX_SLOTS + 1u][16];

static struct xhci_dev *hub_child(axys_uint8_t hub_slot, axys_uint8_t hub_port)
{
    for (unsigned i = 0; i < MAX_SLOTS; ++i) {
        if (devs[i].used && devs[i].hub_slot == hub_slot && devs[i].hub_port == hub_port) {
            return &devs[i];
        }
    }
    return AXYS_NULL;
}

/* A device went away (or its hub did): remove it and everything below it. The
 * transaction mutex is held, so no transfer to it is in flight. The event ring
 * may still hold completions for the old slot; they match no live device and
 * are dropped. A storage device that was registered with the disk layer stops
 * answering (its transfers fail), but the disk layer does not re-attach a
 * replacement: that needs a block-device hot-plug policy of its own. */
static void remove_device(struct xhci_dev *dev)
{
    axys_uint8_t slot = dev->slot;

    if (slot != 0u) {
        for (unsigned i = 0; i < MAX_SLOTS; ++i) {
            if (devs[i].used && &devs[i] != dev && devs[i].hub_slot == slot) {
                remove_device(&devs[i]);
            }
        }
    }
    if (dev->is_kbd && kbd_count > 0) {
        --kbd_count;
    }
    for (unsigned i = 0; i < sizeof(stor_slots) / sizeof(stor_slots[0]); ++i) {
        if (stor_slots[i].used && stor_slots[i].slot == slot) {
            stor_slots[i].used = 0;
        }
    }
    axys_printf("usb: device removed (slot %u, port %u)\n", (unsigned)slot, (unsigned)dev->root_port);
    if (slot <= MAX_SLOTS) {
        for (unsigned k = 0; k < 16u; ++k) {
            hub_retry[slot][k] = 0;
        }
    }
    abandon_device(dev, deadline_ms(1000u));
}

static void poll_task(void *arg)
{
    unsigned slow = 0;

    (void)arg;
    for (;;) {
        axys_task_sleep_ms(8u);
        xhci_acquire();
        poll_once();
        xhci_release();
        /* Hotplug: a newly connected device (CCS set, port not enabled,
         * nothing assigned) gets enumerated. The waits inside need timer
         * interrupts, so this runs outside the lock above. */
        for (unsigned port = 0; port < max_ports && port < 32u; ++port) {
            axys_uint32_t sc = reg32(port_addr(port));

            if ((sc & PORTSC_CCS) == 0u && port_has_dev(port)) {
                /* Unplugged: free the device and anything behind it. */
                xhci_acquire();
                for (unsigned i = 0; i < MAX_SLOTS; ++i) {
                    if (devs[i].used && devs[i].root_port == port && devs[i].hub_slot == 0u) {
                        remove_device(&devs[i]);
                    }
                }
                sc = reg32(port_addr(port));
                reg32_write(port_addr(port), (sc & PORTSC_CHANGES) | (sc & PORTSC_PP));
                port_retry_at[port] = 0; /* a new device may be plugged in right away */
                xhci_release();
                continue;
            }
            if ((sc & PORTSC_CCS) == 0u || (sc & PORTSC_PED) != 0u || port_has_dev(port)) {
                continue;
            }
            if ((axys_int64_t)(axys_pit_millis() - port_retry_at[port]) < 0) {
                continue;
            }
            port_retry_at[port] = axys_pit_millis() + 10000u;
            {
                axys_uint32_t speed;

                xhci_acquire();
                speed = reset_port(port);
                if (speed != 0u) {
                    (void)enumerate_port(port, speed);
                }
                xhci_release();
            }
        }
        /* Hub downstream ports, at a slower cadence (a full scan is several
         * control transfers; every 8ms would drown the bus). Enumerates
         * newly connected devices behind already-configured hubs. */
        if (++slow >= 250u) {
            slow = 0;
            for (unsigned i = 0; i < MAX_SLOTS; ++i) {
                struct xhci_dev *hub = &devs[i];
                axys_uint8_t *buf;
                axys_uint64_t buf_phys;

                if (!hub->used || !hub->is_hub) {
                    continue;
                }
                buf = axys_dma_alloc(64, 16, 0, AXYS_DMA_32BIT, &buf_phys);
                if (buf == AXYS_NULL) {
                    continue;
                }
                xhci_acquire();
                for (axys_uint8_t p = 1; p <= hub->hub_ports; ++p) {
                    axys_uint32_t speed;

                    if (hub_port_has_dev(hub->slot, p)) {
                        /* Still there? A port whose connection bit dropped has
                         * lost its device (and anything below it). */
                        axys_memset(buf, 0, 4);
                        if (hub_port_status(hub, p, buf_phys, deadline_ms(1000u)) == 0 &&
                            (buf[0] & 0x01u) == 0u) {
                            struct xhci_dev *gone = hub_child(hub->slot, p);

                            if (gone != AXYS_NULL) {
                                remove_device(gone);
                            }
                            hub_retry[hub->slot][p - 1u] = 0;
                        }
                        continue;
                    }
                    if ((axys_int64_t)(axys_pit_millis() - hub_retry[hub->slot][p - 1u]) <
                        0) {
                        continue;
                    }
                    axys_memset(buf, 0, 4);
                    if (hub_port_status(hub, p, buf_phys, deadline_ms(1000u)) != 0) {
                        continue;
                    }
                    if ((buf[0] & 0x01u) == 0u) {
                        continue; /* nothing connected: look again on the next scan */
                    }
                    /* Something is plugged in: throttle retries only for a
                     * device that then fails to enumerate. */
                    hub_retry[hub->slot][p - 1u] = axys_pit_millis() + 10000u;
                    speed = hub_reset_port(hub, p, buf, buf_phys, deadline_ms(2000u));
                    if (speed == 0u || hub->depth + 1u > 5u) {
                        continue;
                    }
                    {
                        axys_uint32_t route =
                            hub->route | ((axys_uint32_t)(p & 0x0fu) << (hub->depth * 4u));
                        axys_uint8_t ps = hub->speed == SPEED_HS ? hub->slot : 0;
                        axys_uint8_t pp = hub->speed == SPEED_HS ? p : 0;

                        (void)enumerate_device(hub->root_port, route, ps, pp, hub->slot, p,
                                               hub->depth + 1u, speed);
                    }
                }
                xhci_release();
                axys_dma_free(buf);
            }
        }
    }
}

/* ---- init --------------------------------------------------------------------------- */

int axys_xhci_keyboard_count(void)
{
    return kbd_count;
}

const char *axys_xhci_summary(void)
{
    return summary;
}

int axys_xhci_init(void)
{
    const struct axys_pci_device *device;
    axys_uint64_t bar;
    axys_uint64_t bar_size;
    axys_uint8_t caplen;
    axys_uint32_t hcs1;
    axys_uint32_t hcc1;
    axys_uint32_t slots;
    axys_uint64_t dboff;
    axys_uint64_t rtsoff;
    axys_uint64_t deadline;

    kbd_count = 0;
    summary[0] = '\0';
    mmio = 0;
    for (unsigned i = 0; i < MAX_SLOTS; ++i) {
        devs[i].used = 0;
    }
    device = axys_pci_find(0x0cu, 0x03u);
    if (device == AXYS_NULL || device->bars[0] == AXYS_PCI_BAR_NONE) {
        axys_snprintf(summary, sizeof(summary), "no xHCI controller on the bus");
        return 0; /* absent is fine: PS/2 and serial still work */
    }
    if (device->prog_if != 0x30u) {
        axys_snprintf(summary, sizeof(summary), "USB controller is not xHCI, ignored");
        return 0;
    }
    bar = device->bars[0];
    bar_size = device->bar_size[0] != 0 ? device->bar_size[0] : 0x10000u;
    if (bar > axys_vmm_physical_limit() - bar_size ||
        axys_vmm_map_mmio_range(bar, (axys_uint32_t)bar_size) != 0) {
        axys_snprintf(summary, sizeof(summary), "xHCI BAR0 is not a reserved, mappable range");
        return -1;
    }
    axys_pci_enable_bus_master(device);
    mmio = bar;
    caplen = (axys_uint8_t)(reg32(mmio + CAP_CAPLENGTH) & 0xffu);
    if (caplen < 0x20u) {
        axys_snprintf(summary, sizeof(summary), "xHCI has a bogus capability length");
        return -1;
    }
    op_base = mmio + caplen;
    dboff = reg32(mmio + CAP_DBOFF) & ~3u;
    rtsoff = reg32(mmio + CAP_RTSOFF) & ~31u;
    db_base = mmio + dboff;
    rt_base = mmio + rtsoff;

    /* Stop a running controller, then reset it. */
    reg32_write(op_base + OP_USBCMD, reg32(op_base + OP_USBCMD) & ~CMD_RS);
    deadline = deadline_ms(2000u);
    while ((reg32(op_base + OP_USBSTS) & STS_HALTED) == 0u) {
        if (expired(deadline)) {
            axys_snprintf(summary, sizeof(summary), "xHCI would not halt");
            return -1;
        }
        axys_cpu_relax();
    }
    reg32_write(op_base + OP_USBCMD, CMD_HCRST);
    deadline = deadline_ms(5000u);
    while ((reg32(op_base + OP_USBSTS) & STS_CNR) != 0u) {
        if (expired(deadline)) {
            axys_snprintf(summary, sizeof(summary), "xHCI reset timed out");
            return -1;
        }
        axys_cpu_relax();
    }

    hcs1 = reg32(mmio + CAP_HCS1);
    hcc1 = reg32(mmio + CAP_HCC1);
    slots = HCS1_SLOTS(hcs1);
    if (slots > MAX_SLOTS) {
        slots = MAX_SLOTS;
    }
    max_ports = HCS1_PORTS(hcs1);
    if (max_ports > 32u) {
        max_ports = 32u;
    }
    (void)hcc1;

    /* Device context base address array (slot contexts live behind it). */
    {
        axys_uint64_t phys;
        axys_uint64_t n = (axys_uint64_t)slots + 1u;

        dcbaa = axys_dma_alloc(n * 8u, 64, 0, AXYS_DMA_32BIT, &phys);
        if (dcbaa == AXYS_NULL) {
            axys_snprintf(summary, sizeof(summary), "xHCI has no memory for the DCBAA");
            return -1;
        }
        dcbaa_count = slots;
        if (!HCC1_64BIT(reg32(mmio + CAP_HCC1))) {
            axys_snprintf(summary, sizeof(summary), "xHCI lacks 64-bit addressing");
            return -1;
        }
        for (axys_uint64_t i = 0; i < n; ++i) {
            dcbaa[i] = 0;
        }
        /* Scratchpad buffers when the controller wants them. */
        {
            axys_uint32_t hcs2 = reg32(mmio + CAP_HCS2);
            axys_uint32_t want = (HCS2_SCRATCH_HI(hcs2) << 5) | HCS2_SCRATCH_LO(hcs2);

            if (want > 64u) {
                axys_snprintf(summary, sizeof(summary),
                              "xHCI wants %u scratchpad buffers", want);
                return -1;
            }
            if (want != 0) {
                axys_uint64_t spa_phys;
                axys_uint64_t *spa =
                    axys_dma_alloc(1024, 1024, 0, AXYS_DMA_32BIT, &spa_phys);

                if (spa == AXYS_NULL) {
                    axys_snprintf(summary, sizeof(summary), "xHCI scratchpad failed");
                    return -1;
                }
                for (axys_uint32_t i = 0; i < want; ++i) {
                    axys_uint64_t page;
                    void *p = axys_dma_alloc(4096, 4096, 0, AXYS_DMA_32BIT, &page);

                    if (p == AXYS_NULL) {
                        axys_snprintf(summary, sizeof(summary), "xHCI scratchpad failed");
                        return -1;
                    }
                    spa[i] = page;
                }
                dcbaa[0] = spa_phys;
            }
        }
        reg64_write(op_base + OP_DCBAAP, (axys_uint64_t)(axys_uintptr_t)dcbaa);
    }

    /* Command ring, event ring + table, interrupter 0 (polled, no MSI). */
    if (ring_init(&cmd_ring) != 0) {
        axys_snprintf(summary, sizeof(summary), "xHCI has no memory for rings");
        return -1;
    }
    {
        event_seg = axys_dma_alloc(EVENT_TRBS * sizeof(struct trb), 4096, 0, AXYS_DMA_32BIT,
                                   &event_phys);
        erst = axys_dma_alloc(sizeof(*erst), 64, 0, AXYS_DMA_32BIT, &erst_phys);
        if (event_seg == AXYS_NULL || erst == AXYS_NULL) {
            axys_snprintf(summary, sizeof(summary), "xHCI has no memory for rings");
            return -1;
        }
        erst[0].base = event_phys;
        erst[0].size = EVENT_TRBS;
        erst[0].reserved = 0;
        event_dequeue = 0;
        event_cycle = 1;
        reg32_write(rt_base + RT_IMOD, 0);
        reg32_write(rt_base + RT_ERSTSZ, 1);
        reg64_write(rt_base + RT_ERSTBA, erst_phys);
        reg64_write(rt_base + RT_ERDP, event_phys);
        reg32_write(rt_base + RT_IMAN, reg32(rt_base + RT_IMAN) & ~IMAN_IP);
    }
    reg64_write(op_base + OP_CRCR, cmd_ring.phys | CRCR_RCS);
    reg32_write(op_base + OP_DNCTRL, 0);
    reg32_write(op_base + OP_CONFIG, (reg32(op_base + OP_CONFIG) & ~0xffu) | slots);

    /* Go. */
    reg32_write(op_base + OP_USBCMD, CMD_RS);
    deadline = deadline_ms(5000u);
    while ((reg32(op_base + OP_USBSTS) & STS_CNR) != 0u ||
           (reg32(op_base + OP_USBSTS) & STS_HALTED) != 0u) {
        if (expired(deadline)) {
            axys_snprintf(summary, sizeof(summary), "xHCI would not start");
            return -1;
        }
        axys_cpu_relax();
    }
    /* One pass over the root-hub ports. No lock: nothing else touches xHCI
     * state during init (the poll task starts below, after enumeration), and
     * the waits below are bounded by pit_millis(), which only advances with
     * timer interrupts enabled. Holding an irqsave lock across them would
     * freeze the very clock the timeouts read. */
    for (unsigned port = 0; port < max_ports; ++port) {
        axys_uint32_t speed = reset_port(port);

        if (speed != 0u) {
            (void)enumerate_port(port, speed);
        }
    }
    {
        struct axys_task *task;

        axys_snprintf(summary, sizeof(summary), "%02x:%02x.%u, %u ports, %d keyboards",
                      device->bus, device->device, device->function, max_ports, kbd_count);
        /* Keep polling interrupt endpoints in the background. */
        task = axys_task_create("usbhid", poll_task, AXYS_NULL);
        if (task != AXYS_NULL) {
            axys_task_detach(task);
        }
    }
    return kbd_count;
}
