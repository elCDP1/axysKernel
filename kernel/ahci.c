#include "axys/ahci.h"
#include "axys/cpu.h"
#include "axys/io.h"
#include "axys/pit.h"
#include "axys/pci.h"
#include "axys/printf.h"
#include "axys/sched.h"
#include "axys/spinlock.h"
#include "axys/string.h"
#include "axys/vmm.h"

/* AHCI host bus adapter driver, polled.
 *
 * Layout facts this file encodes, all from the AHCI 1.3 specification:
 *
 *  - The HBA is reached through BAR5 (ABAR), a memory region of 0x1000 bytes
 *    or more, holding capability and global registers, then one register block
 *    per implemented port starting at offset 0x100, each strided by 0x80.
 *  - A command list is an array of 32-byte command *headers*. A header holds the
 *    address of a separate command table, not a FIS itself; the table is what
 *    carries the Command FIS, and the table's PRDT says where the payload goes.
 *  - The payload never travels inside a FIS. The HBA moves it between the drive
 *    and the memory the PRDT describes, and only 64-byte FIS headers land in the
 *    receive area. The 128-sector limit below is the size of the payload buffer,
 *    not a property of the FIS.
 *  - Everything the HBA reads or writes is described by a physical address, not
 *    a virtual one. The kernel identity-maps physical memory, so a pointer is
 *    already the address the controller needs.
 *
 * All multi-byte fields are little-endian, and the structures below are built
 * as byte arrays with explicit stores rather than as structs: that keeps the
 * wire layout exact without depending on padding or alignment rules, which is
 * the safer habit for anything the hardware reads directly. */

/* ---- HBA global registers (ABAR + offset) ----
 * Capability, general control, interrupt status, port-implemented and version
 * sit at 0x00, 0x04, 0x08, 0x0c and 0x10. The port-implemented mask and the
 * version register are two registers apart from the usual guess, and reading the
 * wrong one is silent: the value looks like a plausible mask. */
#define HBA_CAP 0x00u
#define HBA_GHC 0x04u
#define HBA_IS 0x08u
#define HBA_PI 0x0cu
#define HBA_VS 0x10u
#define HBA_CAP2 0x2cu

#define GHC_HBA_ENABLE 0x00000001u
#define GHC_AHB_ENABLE 0x00000002u

#define CAP_SUPPORTED_PORTS 0x0000001fu
#define CAP_SPT 0x00020000u /* supports port multipliers */
#define CAP_SLT 0x80000000u /* supports command slot limiting */

/* ---- Per-port registers, relative to abar + 0x100 + port * stride ----
 * The stride is 0x80, not the 0x40 the register block alone suggests: 0x28 bytes
 * of registers followed by reserved space up to the next port. Reading it as
 * 0x40 is silent rather than fatal -- every other port simply reads back as
 * zeros, so half the ports look empty.
 *
 * The offsets past PxCMD are the ones that bite. There is a reserved dword
 * between PxCMD and the task file data, so PxCMD is at 0x18, not 0x14, and
 * PxSSTS/PxSCTL/PxSERR/PxSACT/PxCI sit at 0x28..0x38 rather than 0x18..0x28.
 * Shifting them by one dword makes every write land in the neighbouring register
 * and still read back, which is why it is worth writing them out in full. */
#define PX_PORT_BASE 0x100u
#define PX_PORT_STRIDE 0x80u
#define PX_CLB 0x00u  /* command list base */
#define PX_CLBU 0x04u /* command list base, upper 32 bits */
#define PX_FB 0x08u   /* received FIS base */
#define PX_IS 0x10u    /* interrupt status, write-1-to-clear */
#define PX_IE 0x14u
#define PX_CMD 0x18u
#define PX_TFD 0x20u /* task file data: status in 7:0, error in 15:8 */
#define PX_SIG 0x24u
#define PX_SSTS 0x28u /* device present in bit 0 */
#define PX_SCTL 0x2cu
#define PX_SERR 0x30u
#define PX_SACT 0x34u /* slot active, one bit per command slot */
#define PX_CI 0x38u   /* command issue: non-zero while the HBA is busy */

/* Any of these three says the drive handed a status back: a task file
 * delivered, or a port status change, or a command that ended in an error. */
#define IS_REPORTED 0x40000003u /* PSS | TFES | DHRS */

/* PxCMD bit layout. ST is bit 0, the command list length is 15:8 and CRQ is bit
 * 28; since AHCI 1.1 the number of FIS receive entries is fixed at five, so there
 * is no FRE field to program.
 *
 * Two separate bits gate FIS reception and they are easy to conflate: FISRE (bit
 * 3) is the original AHCI 1.0 "accept FISes" flag, while FISRX (bit 4) is the
 * bit that actually starts the receive DMA engine. QEMU only looks at FISRX, so
 * a driver that sets FISRE alone leaves the receive area unmapped and then waits
 * forever for an answer that was never going to be written anywhere. */
#define CMD_ST 0x00000001u    /* start */
#define CMD_SP 0x00000002u    /* spin up */
#define CMD_CR 0x00000004u    /* command list override */
#define CMD_FISRE 0x00000008u /* FIS receive enable */
#define CMD_FISRX 0x00000010u /* FIS receive DMA engine enable */
#define CMD_PRDTL_SHIFT 8u
#define CMD_CRQ 0x10000000u   /* command list running */

/* The PxCMD value that leaves a port started with reception enabled. CRQ is
 * included for real hardware, where it is what tells the HBA to work the list. */
#define CMD_PORT_RUN (CMD_ST | CMD_FISRE | CMD_FISRX | CMD_CRQ)

/* PxSSTS.DEV: the HBA sets it once a device has answered on the port. */
#define SSTS_DEV_PF 0x0001u

/* PxSCTL.DET is bits 3:0: 1 issues a COMRESET, 3 an INIT. The HBA clears the
 * field back to 0 once the reset it triggered has finished. */
#define SCTL_DET_MASK 0x000fu
#define SCTL_DET_NORMAL 0x0000u
#define SCTL_DET_COMRESET 0x0001u
#define SCTL_DET_INIT 0x0003u

#define FIS_ERROR_MASK 0x80000000u
#define FIS_TYPE_MASK 0x000000ffu
#define FIS_C 0x00000080u

#define FIS_SETUP_H2D 0x27u
#define FIS_DATA_H2D 0x34u
#define FIS_SIGNATURE 0x10u

/* ATA commands. The "ext" variants take a 48-bit LBA in eight bytes and a count
 * of 0 meaning 256 sectors, which avoids the 28-bit gymnastics entirely.
 * IDENTIFY DEVICE is 0xec: 0xb0 is SMART and would return nothing useful. */
#define ATA_READ_DMA_EXT 0x25u
#define ATA_WRITE_DMA_EXT 0x35u
#define ATA_FLUSH_EXT 0xeau
#define ATA_IDENTIFY 0xecu
#define ATA_DEVICE_LBA48 0x40u

#define ATA_STATUS_ERR 0x01u
#define ATA_STATUS_DRDY 0x40u
#define ATA_STATUS_BSY 0x80u
#define ATA_STATUS_DFR 0x20u

/* The command header's flags word: the Command FIS length in dwords, the
 * ATAPI bit, the direction bit and the prefetchable bit. The HBA reads the
 * direction from here to decide which way the payload travels, so a write whose
 * flags word says "read" is a read no matter what the PRDT points at. */
#define CMD_FLAG_FIS_DWORDS 0x05u
#define CMD_FLAG_WRITE 0x40u
#define CMD_FLAG_PREFETCH 0x0100u

#define PORT_MAX 8u
#define MAX_SECTORS_PER_COMMAND 128u /* 64 KiB of Data FIS payload */

/* Time-bounded polling budgets (milliseconds). Counting register reads was the
 * previous scheme: its real duration depended on CPU speed and MMIO latency,
 * so the same constant meant seconds on one machine and minutes on another --
 * and a hung or slow device could hold ahci_lock (interrupts disabled) long
 * enough to look like a system freeze. Deadlines measured against the PIT are
 * hardware-independent. The AT spec itself allows a drive up to ~30 s to stay
 * BUSY after a reset/soft-reset before we may call it dead, so command waits
 * use that; detection is a quick probe run across every port, so it stays
 * short. The presence budget must also cover the worst case of the multiplier
 * handshake on top of the plain DEV wait (both are DETECT_MS), because one
 * empty port costs comreset settle + DEV wait + handshake + reset + DEV wait. */
#define AHCI_TIMEOUT_CMD_MS   30000u /* command completion and spin-up */
#define AHCI_TIMEOUT_RESET_MS 30000u /* reserved: the AT spec bound for a future SRST path; COMRESET uses fixed settle delays */
#define AHCI_TIMEOUT_DETECT_MS 4000u /* per-port device-presence probe */

static axys_uint64_t ahci_deadline_ms(axys_uint64_t ms)
{
    return axys_pit_millis() + ms;
}

static int ahci_expired(axys_uint64_t deadline)
{
    return (axys_int64_t)(deadline - axys_pit_millis()) <= 0;
}

/* The ABAR is at least a page; ask for two so a controller that implements
 * more than the minimum is still mapped. */
#define ABAR_MAP_BYTES 0x2000u

static axys_uint64_t abar;
static int present;
static axys_uint8_t drive_port; /* the port the drive was found on */
static axys_uint64_t sectors;  /* LBA48 capacity */
static char summary[160];
static struct axys_spinlock ahci_lock;
static int ahci_busy;

/* ---- HBA working memory -------------------------------------------------
 * Fixed rather than allocated: the sizes are known, the lifetime is the
 * kernel's, and keeping them in BSS removes any question about whether the
 * heap can hand back 64 KiB of page-aligned memory at this point in boot. Each
 * buffer is aligned to the strictest alignment the spec places on it.
 *
 * The payload buffer is separate from the FIS receive area on purpose. A data
 * FIS proper is a 64-byte header, and the sector data does not live inside it:
 * the HBA moves the payload straight between the drive and the memory the PRDT
 * describes, and only the headers land in fis_receive. Conflating the two
 * would cap every transfer at the 480 bytes that follow a header in a 512-byte
 * structure -- less than a single sector. */

/* The command list holds one 32-byte header per slot, and only slot 0 is ever
 * used: this driver runs one command at a time, so there is no second FIS to
 * keep apart. */
#define CMD_HDR_BYTES 32u
#define CMD_SLOT 0u
static axys_uint8_t command_list[32u * CMD_HDR_BYTES] __attribute__((aligned(1024)));

/* A command table is its own structure, pointed at by the header's CTBA: a
 * 32-byte Command FIS, 16 bytes of ATAPI CDB, then the PRDT. AHCI 1.3 requires
 * the table to be 0x80-aligned and the HBA maps a full 0x80 bytes of it before
 * reading the FIS, so the region is padded to 0x80 rather than packed -- the
 * descriptor table is only reachable from there once the header's count is
 * taken into account, and packing it against the FIS puts it in the hole the HBA
 * never mapped. */
#define CMD_TABLE_FIS_BYTES 32u
#define CMD_TABLE_ATAPI_BYTES 16u
#define CMD_TABLE_HDR_BYTES 0x80u
#define CMD_TABLE_BYTES (CMD_TABLE_HDR_BYTES + 32u)
static axys_uint8_t command_table[CMD_TABLE_BYTES] __attribute__((aligned(128)));
#define PRDT_AT CMD_TABLE_HDR_BYTES

/* A physical region descriptor: a 64-bit address, a reserved dword, and a dword
 * holding the byte count in 22 bits and a completion-interrupt request in the
 * top bit. The count is zero-based. */
#define PRDT_BYTES 16u
#define PRDT_COUNT_MASK 0x003fffffu
#define PRDT_INTERRUPT 0x80000000u

/* The spec requires room for the 4-byte signature plus five 64-byte receive
 * entries, so 256 bytes per port is too small and would spill into the next port. */
#define FIS_AREA_BYTES 512u
static axys_uint8_t fis_receive[PORT_MAX * FIS_AREA_BYTES] __attribute__((aligned(256)));
static axys_uint8_t data_buffer[64u * 1024u] __attribute__((aligned(128)));

static axys_uint32_t read32(axys_uint64_t address)
{
    return *(volatile axys_uint32_t *)(axys_uintptr_t)address;
}

static void write32(axys_uint64_t address, axys_uint32_t value)
{
    *(volatile axys_uint32_t *)(axys_uintptr_t)address = value;
}

/* The port stride is a fixed 0x80 in every revision of AHCI there is a CAP.PSC
 * field for, so it is not something to read out of the capability register: CAP
 * bits 12:8 hold the number of command slots, and treating that as a stride
 * turns a working controller into one whose ports all sit 0x80000 bytes apart. */
static axys_uint64_t port_reg(axys_uint8_t index, axys_uint32_t offset)
{
    return abar + PX_PORT_BASE + (axys_uint64_t)index * PX_PORT_STRIDE + offset;
}

static void port_write(axys_uint8_t index, axys_uint32_t offset, axys_uint32_t value)
{
    write32(port_reg(index, offset), value);
}

static axys_uint32_t port_read(axys_uint8_t index, axys_uint32_t offset)
{
    return read32(port_reg(index, offset));
}

/* The receive area belonging to a port. The index is clamped rather than trusted:
 * every caller passes a port that came out of the HBA port-implemented mask, but
 * an unchecked index would silently walk off the end of the array. */
static axys_uint8_t *fis_for(axys_uint8_t index)
{
    return &fis_receive[(axys_size_t)(index < PORT_MAX ? index : 0) * FIS_AREA_BYTES];
}

static void put8(axys_uint8_t *at, axys_size_t offset, axys_uint8_t value)
{
    at[offset] = value;
}

static void put16(axys_uint8_t *at, axys_size_t offset, axys_uint16_t value)
{
    put8(at, offset, (axys_uint8_t)value);
    put8(at, offset + 1u, (axys_uint8_t)(value >> 8));
}

static void put32(axys_uint8_t *at, axys_size_t offset, axys_uint32_t value)
{
    at[offset] = (axys_uint8_t)value;
    at[offset + 1u] = (axys_uint8_t)(value >> 8);
    at[offset + 2u] = (axys_uint8_t)(value >> 16);
    at[offset + 3u] = (axys_uint8_t)(value >> 24);
}

static void put64(axys_uint8_t *at, axys_size_t offset, axys_uint64_t value)
{
    put32(at, offset, (axys_uint32_t)value);
    put32(at, offset + 4u, (axys_uint32_t)(value >> 32));
}

static void clear_bytes(axys_uint8_t *at, axys_size_t count)
{
    for (axys_size_t i = 0; i < count; ++i) {
        at[i] = 0u;
    }
}

/* Point slot 0 at the command table and say how many descriptors it holds. The
 * count is zero-based, so one descriptor is 0 and no data at all is also 0 --
 * which is why a command with no payload is told so by a zero count rather than
 * by an empty table. */
static void build_header(axys_uint16_t flags, axys_uint16_t prdtl)
{
    axys_uint8_t *header = &command_list[CMD_SLOT * CMD_HDR_BYTES];

    clear_bytes(header, CMD_HDR_BYTES);
    put16(header, 0, flags);
    put16(header, 2, prdtl);
    put32(header, 4, 0u); /* PRDBC, written back by the HBA as bytes move */
    put64(header, 8, (axys_uint64_t)(axys_uintptr_t)command_table);
}


/* Poll until the masked bits equal `wanted`, or the budget runs out. Returns
 * non-zero on success: a caller that ignores this cannot tell a port that
 * finished from one that never will. */
static int spin_wait(axys_uint32_t address, axys_uint32_t mask, axys_uint32_t wanted,
                     axys_uint64_t deadline)
{
    for (;;) {
        if ((read32(address) & mask) == wanted) {
            return 1;
        }
        if (ahci_expired(deadline)) {
            return 0;
        }
        axys_cpu_relax();
    }
}

/* Wait for a port to become quiescent, and report whether it did.
 *
 * Only PxCI is waited on, not CRQ. CRQ is the bit this driver sets to tell the
 * HBA to work the command list, so asking for it to be clear before issuing
 * anything waits for a state the driver is itself responsible for producing,
 * and the first command after a port is started never leaves. The task file
 * gets the last word: a slot can read free while the drive is still chewing on
 * the previous command. */
static int port_idle(axys_uint8_t index)
{
    if (!spin_wait(port_reg(index, PX_CI), 0xffffffffu, 0u,
                   ahci_deadline_ms(AHCI_TIMEOUT_CMD_MS))) {
        return 0;
    }
    {
        axys_uint64_t deadline = ahci_deadline_ms(AHCI_TIMEOUT_CMD_MS);

        for (;;) {
            if (((port_read(index, PX_TFD) & 0xffu) & ATA_STATUS_BSY) == 0u) {
                return 1;
            }
            if (ahci_expired(deadline)) {
                return 0;
            }
            axys_cpu_relax();
        }
    }
}

/* Wait for the HBA to report a device on the port. The port reset is
 * asynchronous: the HBA runs a signature check of its own and only asserts DEV
 * afterwards, so reading the bit once after the reset says nothing. */
static int wait_for_device(axys_uint8_t index)
{
    axys_uint64_t deadline = ahci_deadline_ms(AHCI_TIMEOUT_DETECT_MS);

    for (;;) {
        if ((port_read(index, PX_SSTS) & SSTS_DEV_PF) != 0u) {
            return 1;
        }
        if (ahci_expired(deadline)) {
            return 0;
        }
        axys_cpu_relax();
    }
}

/* Full COMRESET/COMINIT/COMWAKE settle with the delays the spec requires.
 *
 * Writing SCTL.DET=1 and merely waiting for DET to clear again is NOT a
 * complete reset: AHCI 1.3 section 7.1 states software must hold COMRESET
 * asserted for at least 10 ms before deasserting it, and a drive needs up to
 * 10 ms after COMRESET is released before it drives DAS/DET with its
 * signature. A write that lands and is cleared microseconds later is a pulse
 * too short for real hardware to even see, and polling SSTS in the instant
 * after it can latch a stale "no device" state. So: assert DET, sleep past the
 * 10 ms floor, release DET, then sleep past the power-on/settle window before
 * anybody reads SSTS. */
static void port_comreset(axys_uint8_t index)
{
    port_write(index, PX_SCTL, SCTL_DET_COMRESET);
    axys_pit_sleep_ms(15u);
    port_write(index, PX_SCTL, SCTL_DET_NORMAL);
    /* IDENTIFY DEVICE itself may take up to 30 s on an old drive; this covers
     * only the electrical settle plus the HBA's own signature check. */
    axys_pit_sleep_ms(50u);
    /* Give the HBA one extra poll round-trip budget (a few ms) rather than
     * trusting the settle sleep alone; wait_for_device still owns the final
     * deadline. */
}

/* Bring a port out of reset, give the HBA the locations it needs, and leave the
 * port started with FIS receive enabled. PxCLB and PxFB are latched by the HBA
 * during a port reset and are not re-read afterwards, so both have to be written
 * before the reset that is expected to start the drive. */
static int port_reset(axys_uint8_t index)
{
    /* Stop the port first. A running port would be fetching from the command
     * list underneath us, and the reset below is only defined from idle. */
    port_write(index, PX_CMD, 0u);
    (void)port_idle(index);

    port_write(index, PX_FB, (axys_uint32_t)(axys_uintptr_t)fis_for(index));
    port_write(index, PX_CLB, (axys_uint32_t)(axys_uintptr_t)command_list);
    port_write(index, PX_CLBU, 0u);
    port_write(index, PX_SERR, 0u);

    /* COMRESET with the spec's minimum assertion time and a settle window.
     * Waiting for software-detected DET to clear on its own is not enough:
     * it can clear before the drive has even powered up its interface, and
     * an empty port leaves DET stuck at COMRESET until we release it. */
    port_comreset(index);

    /* PRDTL of 0 means all 32 command list entries are usable. */
    port_write(index, PX_CMD, CMD_PORT_RUN);
    return 0;
}

/* Ask every attached port multiplier for its signature. A device on the port
 * itself answers with its own signature too, so a reply here means there is
 * something behind the port even when DEV_PF stays clear. */
static int port_count_multipliers(axys_uint8_t index)
{
    axys_uint8_t *received = fis_for(index);
    int seen = 0;

    clear_bytes(command_table, CMD_TABLE_BYTES);
    clear_bytes(received, 64u);
    /* Host-to-Device Signature FIS: type 0x10 with C set and no command. The
     * 0xff "signature" value only appears in what the device sends back, so
     * nothing but the type and C belongs in the outgoing FIS. */
    put8(command_table, 0, (axys_uint8_t)(FIS_SIGNATURE | FIS_C));
    build_header(0u, 0u);

    port_write(index, PX_SCTL, SCTL_DET_INIT);
    port_write(index, PX_SERR, 1u);
    port_write(index, PX_CMD, CMD_PORT_RUN);
    port_write(index, PX_CI, 1u << CMD_SLOT);

    {
        axys_uint64_t deadline = ahci_deadline_ms(AHCI_TIMEOUT_DETECT_MS);

        for (;;) {
            if ((received[0] & FIS_TYPE_MASK) == FIS_SIGNATURE) {
                seen = 1;
                break;
            }
            if (ahci_expired(deadline)) {
                break;
            }
            axys_cpu_relax();
        }
    }
    port_write(index, PX_SCTL, SCTL_DET_NORMAL);
    return seen;
}
/* Write the Host-to-Device Setup FIS: a 32-byte task file. `device` is the ATA
 * device register, and the LBA48 bit on it belongs only on the 48-bit commands --
 * on IDENTIFY it turns the command into a different one. */
static void build_fis(axys_uint8_t command, axys_uint64_t lba, axys_uint32_t count,
                      axys_uint8_t device)
{
    clear_bytes(command_table, CMD_TABLE_BYTES);

    /* Byte 1 is the port multiplier field with the C bit above it, and the HBA
     * inspects it before it looks at anything else: its low four bits select a
     * device behind a multiplier, and the bits between C and those are reserved.
     * The command therefore does not go there -- it goes in byte 2. Writing it
     * in byte 1 makes every command look like it was aimed at some other device
     * and the HBA drops it without reporting an error, which leaves the drive
     * sitting there perfectly healthy and the driver certain it was spoken to.
     *
     * Bytes 5 to 11 carry the LBA split across the 28-bit and 48-bit register
     * pairs, and bytes 12 and 13 the sector count; the gaps at 4/14 are the
     * register's high feature and ICC fields. */
    put8(command_table, 0, FIS_SETUP_H2D);
    put8(command_table, 1, FIS_C);
    put8(command_table, 2, command);
    put8(command_table, 3, 0u); /* features */
    put8(command_table, 4, (axys_uint8_t)lba);
    put8(command_table, 5, (axys_uint8_t)(lba >> 8));
    put8(command_table, 6, (axys_uint8_t)(lba >> 16));
    put8(command_table, 7, device);
    put8(command_table, 8, (axys_uint8_t)(lba >> 24));
    put8(command_table, 9, (axys_uint8_t)(lba >> 32));
    put8(command_table, 10, (axys_uint8_t)(lba >> 40));
    put8(command_table, 12, (axys_uint8_t)count);
    put8(command_table, 13, (axys_uint8_t)(count >> 8));
}

/* Describe where the payload lives. One descriptor covers a whole command: the
 * HBA walks the list only as far as the header's count says, and the payload
 * buffer is capped well below what a single descriptor can express. */
static void build_prdt(axys_uint32_t payload_bytes)
{
    axys_uint8_t *descriptor = &command_table[PRDT_AT];

    put64(descriptor, 0, (axys_uint64_t)(axys_uintptr_t)data_buffer);
    put32(descriptor, 8, 0u);
    /* The length is zero-based, and the top bit asks for a completion interrupt
     * which stays set harmlessly because the port is polled. */
    put32(descriptor, 12, ((payload_bytes - 1u) & PRDT_COUNT_MASK) | PRDT_INTERRUPT);
}

/* Program slot 0 for one command and leave the port ready to be handed it. */
static void build_command(axys_uint8_t command, axys_uint64_t lba, axys_uint32_t count,
                          axys_uint32_t payload_bytes, int needs_payload,
                          axys_uint8_t device, int is_write)
{
    axys_uint16_t flags = (axys_uint16_t)(CMD_FLAG_FIS_DWORDS | CMD_FLAG_PREFETCH |
                                          (is_write ? CMD_FLAG_WRITE : 0u));

    build_fis(command, lba, count, device);
    if (needs_payload && payload_bytes != 0u) {
        build_prdt(payload_bytes);
    }
    build_header(flags, (axys_uint16_t)(needs_payload && payload_bytes != 0u ? 1u : 0u));
}

/* Whether a drive answered the last command. The status byte PxTFD is updated
 * together with the receive-area FIS, and it is the drive's own verdict: the
 * payload of a read never passes through here, it travels through the PRDT
 * straight into the buffer. */
static int ata_status_ok(axys_uint8_t index)
{
    axys_uint32_t status = port_read(index, PX_TFD) & 0xffu;

    return (status & (ATA_STATUS_ERR | ATA_STATUS_DFR)) == 0u;
}

static void lock(void)
{
    for (;;) {
        axys_uint64_t flags = axys_spin_lock_irqsave(&ahci_lock);

        if (!ahci_busy) {
            ahci_busy = 1;
            axys_spin_unlock_irqrestore(&ahci_lock, flags);
            return;
        }
        axys_spin_unlock_irqrestore(&ahci_lock, flags);
        axys_yield();
    }
}

static void unlock(void)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&ahci_lock);

    ahci_busy = 0;
    axys_spin_unlock_irqrestore(&ahci_lock, flags);
}

/* Hand slot 0 to the HBA and wait for it to retire. */
static int run_command(void)
{
    axys_uint8_t *received = fis_for(drive_port);
    int retired = 0;

    if (!port_idle(drive_port)) {
        return -1;
    }
    /* Clear the slot the answer will land in. Only the first entry is used: one
     * command at a time, so there is never a second FIS to confuse it. */
    clear_bytes(received, 64u);
    /* PxIS is write-1-to-clear, and these three bits are what say the drive
     * reported back. Clearing first is what makes them mean this command rather
     * than an earlier one. */
    port_write(drive_port, PX_IS, 0xffffffffu);

    /* CRQ is what makes real hardware work through the list. QEMU instead only
     * looks at PxCI, so that is written too: a compliant controller treats the
     * register as read-only and discards the write, while QEMU takes it as the
     * request to run the slot. Setting only CRQ leaves the command sitting in
     * memory forever on one and the other. */
    port_write(drive_port, PX_CMD, (read32(port_reg(drive_port, PX_CMD)) & ~CMD_CRQ) | CMD_CRQ);
    port_write(drive_port, PX_CI, 1u << CMD_SLOT);

    /* Wait for the command to finish. Waiting for the slot to appear in PxCI
     * first is what a spec-first reading suggests, and it hangs twice over: an
     * HBA that completes the command inside the issue itself never shows the
     * bit, and PxCI is not cleared at all when the drive answers with an error,
     * so a single failure wedges the port for good. Watching the task file alone
     * is not enough either: it is already idle before the command starts, and a
     * controller that finishes the transfer from a bottom half has not touched
     * it yet when the issue write returns, so the loop would fall through on the
     * stale status and read the buffer before the data lands. PxIS is the
     * register that actually changes hands, so wait for the drive to report and
     * then for it to stop being busy. */
    {
        axys_uint64_t deadline = ahci_deadline_ms(AHCI_TIMEOUT_CMD_MS);

        for (;;) {
            axys_uint32_t status;

            if ((port_read(drive_port, PX_IS) & IS_REPORTED) == 0u) {
                if (ahci_expired(deadline)) {
                    break;
                }
                axys_cpu_relax();
                continue;
            }
            status = port_read(drive_port, PX_TFD) & 0xffu;
            if ((status & ATA_STATUS_BSY) == 0u &&
                (status & (ATA_STATUS_DRDY | ATA_STATUS_ERR)) != 0u) {
                retired = 1;
                break;
            }
            if (ahci_expired(deadline)) {
                break;
            }
            axys_cpu_relax();
        }
    }
    if (!retired) {
        return -1;
    }
    port_write(drive_port, PX_CMD, read32(port_reg(drive_port, PX_CMD)) & ~CMD_CRQ);
    return ata_status_ok(drive_port) ? 0 : -1;
}


/* IDENTIFY DEVICE. The table is 512 bytes of little-endian 16-bit words, so it
 * lands in memory in order and only needs to be indexed by word. */
static int identify_capacity(axys_uint64_t *out)
{
    axys_uint16_t words[256];
    axys_uint64_t lba28;
    axys_uint64_t lba48;

    /* IDENTIFY is not a DMA command, but the HBA still moves the 512 bytes
     * through the PRDT, so the header needs a descriptor or there is nowhere to
     * put them. */
    build_command(ATA_IDENTIFY, 0u, 0u, AXYS_AHCI_SECTOR_SIZE, 1, 0u, 0);
    if (run_command() != 0) {
        return -1;
    }
    for (axys_size_t i = 0; i < 256u; ++i) {
        words[i] = (axys_uint16_t)((axys_uint16_t)data_buffer[i * 2u] |
                                   (axys_uint16_t)((axys_uint16_t)data_buffer[i * 2u + 1u] << 8));
    }
    /* The 28-bit capacity is words 60 and 61 as one 32-bit little-endian value,
     * so word 60 holds the low half. Reading them the other way round does not
     * look like an error: it moves the low half up sixteen bits, and a drive of
     * 65536 sectors comes back as one sector. */
    lba28 = ((axys_uint64_t)words[61] << 16) | words[60];
    /* Words 100-103 hold the LBA48 count as four little-endian 16-bit pieces,
     * and word 100 bit 10 announces that the drive really implements it.
     * Without that bit the 28-bit value is all there is. */
    if ((words[100] & 0x0400u) == 0u) {
        *out = lba28;
        return *out != 0u ? 0 : -1;
    }
    lba48 = (axys_uint64_t)(words[100] & 0x0fffu) | ((axys_uint64_t)words[101] << 16) |
            ((axys_uint64_t)words[102] << 32) | ((axys_uint64_t)words[103] << 48);
    *out = lba48;
    return *out != 0u ? 0 : -1;
}

/* Report why a port has no usable drive. A controller that enumerates but never
 * answers is the normal symptom of a port that was not reset properly or of a
 * device that never asserted DEV_PF, and the per-port registers say which. */
static void log_port_state(axys_uint8_t index)
{
    axys_uint32_t ssts = port_read(index, PX_SSTS);

    axys_printf("ahci: port %u clb=0x%08x fb=0x%08x ssts=0x%04x dev=%u sig=0x%08x "
                "tfd=0x%04x ci=0x%08x sact=0x%08x serr=0x%08x cmd=0x%08x sctl=0x%04x\n",
                index, port_read(index, PX_CLB), port_read(index, PX_FB), ssts,
                (ssts & SSTS_DEV_PF) != 0u ? 1u : 0u, port_read(index, PX_SIG),
                port_read(index, PX_TFD) & 0xffffu, port_read(index, PX_CI),
                port_read(index, PX_SACT), port_read(index, PX_SERR),
                port_read(index, PX_CMD), port_read(index, PX_SCTL));
}

int axys_ahci_init(void)
{
    const struct axys_pci_device *device;
    axys_uint32_t cap;
    axys_uint32_t implemented;
    axys_uint32_t ports;

    present = 0;
    sectors = 0;
    summary[0] = '\0';
    abar = 0u;

    /* SATA is class 0x01 subclass 0x06; the class field is one byte, so the
     * two halves have to be passed separately. */
    device = axys_pci_find(AXYS_PCI_CLASS_STORAGE, AXYS_PCI_SUBCLASS_SATA);
    if (device == AXYS_NULL) {
        axys_snprintf(summary, sizeof(summary), "no SATA controller on the bus");
        return -1;
    }
    if (device->bars[5] == AXYS_PCI_BAR_NONE) {
        axys_snprintf(summary, sizeof(summary), "controller has no ABAR");
        return -1;
    }
    abar = device->bars[5];
    if (abar > axys_vmm_physical_limit() - ABAR_MAP_BYTES ||
        axys_vmm_map_mmio_range(abar, ABAR_MAP_BYTES) != 0) {
        axys_snprintf(summary, sizeof(summary), "ABAR is not a reserved, mappable MMIO range");
        abar = 0u;
        return -1;
    }
    /* The HBA DMAs to and from system memory, so it needs both memory decoding
     * and bus mastering enabled before any register is touched. */
    axys_pci_enable_bus_master(device);

    cap = read32(abar + HBA_CAP);
    /* Only trust ports that both CAP advertises and HBA_PI reports as
     * implemented; the two do not have to agree. */
    /* CAP.NP is encoded as the maximum port index, not a count: zero means
     * one port and five means ports 0 through 5. */
    ports = (cap & CAP_SUPPORTED_PORTS) + 1u;
    if (ports > PORT_MAX) {
        ports = PORT_MAX;
    }
    implemented = read32(abar + HBA_PI);

    /* Program every port before the HBA is switched on. Setting HBA.GHC.HBAEN
     * resets all ports, and that reset is the one moment the HBA latches PxCLB
     * and PxFB. Enabling first and writing them afterwards leaves the HBA
     * running a signature check against whatever addresses the firmware left
     * behind, which shows up as a port that never reports a device. */
    for (axys_uint32_t i = 0; i < ports; ++i) {
        if ((implemented & (1u << i)) == 0u) {
            continue;
        }
        port_write((axys_uint8_t)i, PX_CMD, 0u);
        port_write((axys_uint8_t)i, PX_CLB, (axys_uint32_t)(axys_uintptr_t)command_list);
        port_write((axys_uint8_t)i, PX_CLBU, 0u);
        port_write((axys_uint8_t)i, PX_FB,
                   (axys_uint32_t)(axys_uintptr_t)fis_for((axys_uint8_t)i));
        port_write((axys_uint8_t)i, PX_SERR, 0xffffffffu);
        port_write((axys_uint8_t)i, PX_SCTL, SCTL_DET_NORMAL);
    }
    write32(abar + HBA_GHC, GHC_AHB_ENABLE | GHC_HBA_ENABLE);

    /* The reset that enabling the HBA just performed also cleared PxCMD, so the
     * ports have to be started afterwards. Starting a port is what makes the HBA
     * look for the device. PRDTL of 0 means all 32 command list entries are
     * usable. */
    for (axys_uint32_t i = 0; i < ports; ++i) {
        if ((implemented & (1u << i)) != 0u) {
            port_write((axys_uint8_t)i, PX_CMD, CMD_PORT_RUN);
        }
    }

    for (axys_uint32_t i = 0; i < ports; ++i) {
        axys_uint8_t index = (axys_uint8_t)i;

        if ((implemented & (1u << i)) == 0u) {
            continue;
        }
        /* Full COMRESET with the spec's assertion/settle timing. Enabling the
         * HBA already pulsed reset, but that pulse is not guaranteed to meet
         * the 10 ms floor and a drive left in a strange state answers only to
         * a proper reset. DET must be released before we start the port; on
         * this hardware path there is no device yet, so nothing can be busy. */
        port_comreset(index);
        port_write(index, PX_CMD, CMD_PORT_RUN);

        if (wait_for_device(index) == 0) {
            /* The port came up empty. Try the multiplier handshake before giving
             * up on it, since a drive behind a multiplier does not assert DEV. */
            if ((cap & CAP_SPT) != 0u && port_count_multipliers(index) > 0) {
                (void)port_reset(index);
                (void)wait_for_device(index);
            }
            if ((port_read(index, PX_SSTS) & SSTS_DEV_PF) == 0u) {
                continue;
            }
        }
        drive_port = index;
        if (identify_capacity(&sectors) == 0 && sectors != 0u) {
            present = 1;
            axys_snprintf(summary, sizeof(summary),
                          "dev %x:%x.%x port %u, %llu sectors (%u MiB), rev %u.%u%s",
                          device->vendor_id, device->device_id, device->function, drive_port,
                          (unsigned long long)sectors, (axys_uint32_t)(sectors / 2048u),
                          (read32(abar + HBA_VS) >> 16) & 0x7fu, read32(abar + HBA_VS) & 0xffffu,
                          (cap & CAP_SPT) != 0u ? ", multipliers" : "");
            return 0;
        }
        sectors = 0u;
    }
    axys_printf("ahci: abar=0x%016llx cap=0x%08x ports=%u\n",
                (unsigned long long)abar, cap, ports);
    for (axys_uint32_t i = 0; i < ports; ++i) {
        log_port_state((axys_uint8_t)i);
    }
    axys_snprintf(summary, sizeof(summary), "controller present but no drive answered");
    return -1;
}

int axys_ahci_present(void)
{
    return present;
}

const char *axys_ahci_summary(void)
{
    return summary;
}

axys_uint64_t axys_ahci_sectors(void)
{
    return sectors;
}

int axys_ahci_read(axys_uint64_t lba, axys_uint32_t count, void *buffer)
{
    axys_uint8_t *out = (axys_uint8_t *)buffer;

    if (!present || count == 0u || lba >= sectors || (axys_uint64_t)count > sectors - lba) {
        return -1;
    }
    lock();
    while (count > 0u) {
        axys_uint32_t chunk = count > MAX_SECTORS_PER_COMMAND ? MAX_SECTORS_PER_COMMAND : count;
        axys_size_t bytes = (axys_size_t)chunk * AXYS_AHCI_SECTOR_SIZE;

        build_command(ATA_READ_DMA_EXT, lba, chunk, (axys_uint32_t)bytes, 1,
                      ATA_DEVICE_LBA48, 0);
        if (run_command() != 0) {
            unlock();
            return -1;
        }
        axys_memcpy(out, data_buffer, bytes);
        out += bytes;
        lba += chunk;
        count -= chunk;
    }
    unlock();
    return 0;
}

int axys_ahci_write(axys_uint64_t lba, axys_uint32_t count, const void *buffer)
{
    const axys_uint8_t *in = (const axys_uint8_t *)buffer;

    if (!present || count == 0u || lba >= sectors || (axys_uint64_t)count > sectors - lba) {
        return -1;
    }
    lock();
    while (count > 0u) {
        axys_uint32_t chunk = count > MAX_SECTORS_PER_COMMAND ? MAX_SECTORS_PER_COMMAND : count;
        axys_size_t bytes = (axys_size_t)chunk * AXYS_AHCI_SECTOR_SIZE;

        /* The payload has to be in the buffer before the command starts: the
         * HBA reads it from the memory the PRDT names once it issues the
         * write, and it reads it without consulting the kernel again. The
         * header's direction bit has to agree, or the drive reads where the
         * write was meant to go. */
        axys_memcpy(data_buffer, in, bytes);
        build_command(ATA_WRITE_DMA_EXT, lba, chunk, (axys_uint32_t)bytes, 1,
                      ATA_DEVICE_LBA48, 1);
        if (run_command() != 0) {
            axys_printf("ahci: write failed lba=%llu count=%u is=%08x tfd=%04x ci=%08x serr=%08x\n",
                        (unsigned long long)lba, chunk, port_read(drive_port, PX_IS),
                        port_read(drive_port, PX_TFD) & 0xffffu,
                        port_read(drive_port, PX_CI), port_read(drive_port, PX_SERR));
            unlock();
            return -1;
        }
        in += bytes;
        lba += chunk;
        count -= chunk;
    }
    unlock();
    return 0;
}

int axys_ahci_flush(void)
{
    int rc;

    if (!present) {
        return -1;
    }
    lock();
    build_command(ATA_FLUSH_EXT, 0u, 0u, 0u, 0, 0u, 0);
    rc = run_command();
    if (rc != 0) {
        axys_printf("ahci: flush failed is=%08x tfd=%04x ci=%08x serr=%08x\n",
                    port_read(drive_port, PX_IS), port_read(drive_port, PX_TFD) & 0xffffu,
                    port_read(drive_port, PX_CI), port_read(drive_port, PX_SERR));
    }
    unlock();
    return rc;
}
