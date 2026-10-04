#include "axys/nvme.h"
#include "axys/cpu.h"
#include "axys/pit.h"
#include "axys/pci.h"
#include "axys/pmm.h"
#include "axys/printf.h"
#include "axys/spinlock.h"
#include "axys/string.h"
#include "axys/vmm.h"

/* Minimal NVMe 1.x PCIe driver. This intentionally starts with the common
 * 4 KiB-page, 512-byte-LBA, single-namespace path and serializes commands.
 * Queue memory and the single-page transfer buffer live in the kernel image,
 * which guarantees physically contiguous DMA addresses below 4 GiB. */

#define NVME_CLASS_STORAGE 0x01u
#define NVME_SUBCLASS_NVM 0x08u
#define NVME_PROGIF_NVMHCI 0x02u

#define REG_CAP 0x0000u
#define REG_VS 0x0008u
#define REG_INTMS 0x000cu
#define REG_CC 0x0014u
#define REG_CSTS 0x001cu
#define REG_AQA 0x0024u
#define REG_ASQ 0x0028u
#define REG_ACQ 0x0030u
#define REG_DOORBELL 0x1000u

#define CAP_MQES(cap) ((axys_uint32_t)((cap) & 0xffffu) + 1u)
#define CAP_CSS(cap) ((axys_uint32_t)(((cap) >> 37) & 0xffu))
#define CAP_MPSMIN(cap) ((axys_uint32_t)(((cap) >> 48) & 0x0fu))
#define CAP_MPSMAX(cap) ((axys_uint32_t)(((cap) >> 52) & 0x0fu))
#define CAP_TO_MS(cap) ((axys_uint64_t)(((cap) >> 24) & 0xffu) * 500u)
#define CAP_DSTRD(cap) ((axys_uint32_t)(((cap) >> 32) & 0x0fu))

#define CSTS_RDY 0x01u
#define CSTS_CFS 0x02u
#define CC_EN 0x00000001u
#define CC_IOSQES_64 0x00060000u
#define CC_IOCQES_16 0x00400000u

#define ADMIN_DELETE_SQ 0x00u
#define ADMIN_CREATE_SQ 0x01u
#define ADMIN_IDENTIFY 0x06u
#define ADMIN_CREATE_CQ 0x05u
#define ADMIN_IDENTIFY_CONTROLLER 0x01u
#define ADMIN_IDENTIFY_NAMESPACE 0x00u

#define IO_FLUSH 0x00u
#define IO_WRITE 0x01u
#define IO_READ 0x02u

#define QUEUE_MAX_DEPTH 16u
#define QUEUE_MIN_DEPTH 2u
#define SECTOR_SIZE 512u
#define PAGE_SIZE 4096u
#define SECTORS_PER_PAGE (PAGE_SIZE / SECTOR_SIZE)
/* Time-bounded polling budgets (milliseconds), like the AHCI driver: counting
 * MMIO reads made the real wait depend on CPU speed and host latency. CAP.TO
 * (in 500 ms units) is the controller's own advertised startup timeout and is
 * honored at enable time; these are the ceilings for waits that have no CAP
 * field. The NVMe spec allows a formatted drive up to 2 minutes to complete a
 * command before it must report failure, so command/completion waits get that;
 * CSTS.RDY transitions after an enable/disable follow CC.TIMEOUT instead,
 * which we cap here. */
#define NVME_TIMEOUT_CMD_MS      120000u /* completion of any submitted command */
#define NVME_TIMEOUT_READY_MS     30000u /* CSTS.RDY transitions (fallback if CAP.TO is 0) */
#define NVME_TIMEOUT_SHUTDOWN_MS   5000u /* CC.SHN handshake */

static axys_uint64_t nvme_deadline_ms(axys_uint64_t ms)
{
    return axys_pit_millis() + ms;
}

static int nvme_expired(axys_uint64_t deadline)
{
    return (axys_int64_t)(deadline - axys_pit_millis()) <= 0;
}

struct nvme_command {
    axys_uint32_t dword[16];
};

struct nvme_completion {
    axys_uint32_t dword[4];
};

AXYS_STATIC_ASSERT(sizeof(struct nvme_command) == 64u, nvme_command_is_64_bytes);
AXYS_STATIC_ASSERT(sizeof(struct nvme_completion) == 16u, nvme_completion_is_16_bytes);

static struct nvme_command admin_sq[QUEUE_MAX_DEPTH] AXYS_ALIGN(PAGE_SIZE);
static struct nvme_completion admin_cq[QUEUE_MAX_DEPTH] AXYS_ALIGN(PAGE_SIZE);
static struct nvme_command io_sq[QUEUE_MAX_DEPTH] AXYS_ALIGN(PAGE_SIZE);
static struct nvme_completion io_cq[QUEUE_MAX_DEPTH] AXYS_ALIGN(PAGE_SIZE);
static axys_uint8_t transfer_page[PAGE_SIZE] AXYS_ALIGN(PAGE_SIZE);
static axys_uint8_t identify_page[PAGE_SIZE] AXYS_ALIGN(PAGE_SIZE);

static axys_uint64_t register_base;
static axys_uint64_t sectors;
static axys_uint64_t capability;
static axys_uint64_t startup_timeout_ms; /* CAP.TO, clamped */
static axys_uint32_t queue_depth;
static axys_uint32_t doorbell_stride;
static axys_uint32_t admin_sq_tail;
static axys_uint32_t admin_cq_head;
static axys_uint32_t admin_cq_phase;
static axys_uint32_t io_sq_tail;
static axys_uint32_t io_cq_head;
static axys_uint32_t io_cq_phase;
static axys_uint16_t next_command_id;
static axys_uint16_t last_command_status;
static int present;
static char summary[192];
static struct axys_spinlock nvme_lock;

static volatile axys_uint32_t *reg32(axys_uint32_t offset)
{
    return (volatile axys_uint32_t *)(axys_uintptr_t)(register_base + offset);
}

static volatile axys_uint64_t *reg64(axys_uint32_t offset)
{
    return (volatile axys_uint64_t *)(axys_uintptr_t)(register_base + offset);
}

static axys_uint32_t read_reg32(axys_uint32_t offset)
{
    return *reg32(offset);
}

static void write_reg32(axys_uint32_t offset, axys_uint32_t value)
{
    *reg32(offset) = value;
    __sync_synchronize();
}

static void ring_doorbell(axys_uint32_t queue_id, int completion, axys_uint32_t value)
{
    axys_uint64_t index = (axys_uint64_t)queue_id * 2u + (completion ? 1u : 0u);
    axys_uint64_t offset = REG_DOORBELL + (index << (2u + doorbell_stride));

    *(volatile axys_uint32_t *)(axys_uintptr_t)(register_base + offset) = value;
    __sync_synchronize();
}

static int wait_ready(int ready, axys_uint64_t ms)
{
    axys_uint64_t deadline = nvme_deadline_ms(ms);

    for (;;) {
        axys_uint32_t status = read_reg32(REG_CSTS);

        if ((status & CSTS_CFS) != 0u) {
            return -1;
        }
        if (((status & CSTS_RDY) != 0u) == (ready != 0)) {
            return 0;
        }
        if (nvme_expired(deadline)) {
            return -1;
        }
        axys_cpu_relax();
    }
}

static axys_uint16_t next_cid(void)
{
    ++next_command_id;
    if (next_command_id == 0u) {
        ++next_command_id;
    }
    return next_command_id;
}

static void set_prp1(struct nvme_command *command, const void *pointer)
{
    axys_uint64_t physical = (axys_uint64_t)(axys_uintptr_t)pointer;

    command->dword[6] = (axys_uint32_t)physical;
    command->dword[7] = (axys_uint32_t)(physical >> 32);
}

static int take_completion(volatile struct nvme_completion *queue, axys_uint32_t depth,
                           axys_uint32_t *head, axys_uint32_t *phase,
                           axys_uint32_t queue_id, axys_uint16_t expected_cid)
{
    {
    axys_uint64_t deadline = nvme_deadline_ms(NVME_TIMEOUT_CMD_MS);

    for (;;) {
        axys_uint32_t status = queue[*head].dword[3];

        /* CQE DW3 packs CID in bits 15:0 and status in bits 31:16;
         * the phase tag is status bit 0 (DW3 bit 16), not CID bit 0. */
        if (((status >> 16) & 1u) == *phase) {
            axys_uint16_t cid;
            axys_uint16_t command_status;

            __sync_synchronize();
            cid = (axys_uint16_t)queue[*head].dword[3];
            command_status = (axys_uint16_t)((queue[*head].dword[3] >> 17) & 0x7fffu);
            last_command_status = command_status;
            ++*head;
            if (*head == depth) {
                *head = 0;
                *phase ^= 1u;
            }
            ring_doorbell(queue_id, 1, *head);
            if (cid != expected_cid || command_status != 0u) {
                return -1;
            }
            return 0;
        }
        if ((read_reg32(REG_CSTS) & CSTS_CFS) != 0u) {
            return -1;
        }
        if (nvme_expired(deadline)) {
            return -1;
        }
        axys_cpu_relax();
    }
    }
}

static int admin_command(struct nvme_command *command)
{
    axys_uint16_t cid = next_cid();

    last_command_status = 0xffffu; /* distinguish timeout/CID mismatch from success */
    command->dword[0] = (command->dword[0] & 0xffffu) | ((axys_uint32_t)cid << 16);
    admin_sq[admin_sq_tail] = *command;
    __sync_synchronize();
    admin_sq_tail = (admin_sq_tail + 1u) % queue_depth;
    ring_doorbell(0, 0, admin_sq_tail);
    return take_completion(admin_cq, queue_depth, &admin_cq_head,
                           &admin_cq_phase, 0, cid);
}

static int identify(axys_uint32_t namespace_id, axys_uint32_t cns)
{
    struct nvme_command command;

    axys_memset(identify_page, 0, sizeof(identify_page));
    axys_memset(&command, 0, sizeof(command));
    command.dword[0] = ADMIN_IDENTIFY;
    command.dword[1] = namespace_id;
    set_prp1(&command, identify_page);
    command.dword[10] = cns;
    return admin_command(&command);
}

static axys_uint32_t read_le32(const axys_uint8_t *data)
{
    return (axys_uint32_t)data[0] | ((axys_uint32_t)data[1] << 8) |
           ((axys_uint32_t)data[2] << 16) | ((axys_uint32_t)data[3] << 24);
}

static axys_uint64_t read_le64(const axys_uint8_t *data)
{
    return (axys_uint64_t)read_le32(data) | ((axys_uint64_t)read_le32(data + 4) << 32);
}

static int create_io_queues(void)
{
    struct nvme_command command;

    axys_memset(&command, 0, sizeof(command));
    command.dword[0] = ADMIN_CREATE_CQ;
    set_prp1(&command, io_cq);
    command.dword[10] = 1u | ((queue_depth - 1u) << 16);
    command.dword[11] = 1u; /* physically contiguous, interrupts disabled */
    if (admin_command(&command) != 0) {
        return -1;
    }

    axys_memset(&command, 0, sizeof(command));
    command.dword[0] = ADMIN_CREATE_SQ;
    set_prp1(&command, io_sq);
    command.dword[10] = 1u | ((queue_depth - 1u) << 16);
    command.dword[11] = 1u | (1u << 16); /* physically contiguous, CQ 1 */
    return admin_command(&command);
}

static int submit_io(axys_uint8_t opcode, axys_uint64_t lba,
                     axys_uint32_t sectors_in_command)
{
    struct nvme_command command;
    axys_uint16_t cid = next_cid();

    axys_memset(&command, 0, sizeof(command));
    command.dword[0] = (axys_uint32_t)opcode | ((axys_uint32_t)cid << 16);
    command.dword[1] = 1u; /* namespace 1 */
    if (opcode != IO_FLUSH) {
        set_prp1(&command, transfer_page);
        command.dword[10] = (axys_uint32_t)lba;
        command.dword[11] = (axys_uint32_t)(lba >> 32);
        command.dword[12] = sectors_in_command - 1u;
    }
    io_sq[io_sq_tail] = command;
    __sync_synchronize();
    io_sq_tail = (io_sq_tail + 1u) % queue_depth;
    ring_doorbell(1, 0, io_sq_tail);
    return take_completion(io_cq, queue_depth, &io_cq_head, &io_cq_phase, 1, cid);
}

static const struct axys_pci_device *find_nvme_controller(void)
{
    for (int index = 0; index < axys_pci_count(); ++index) {
        const struct axys_pci_device *device = axys_pci_device_at(index);

        if (device != AXYS_NULL && device->class_code == NVME_CLASS_STORAGE &&
            device->subclass == NVME_SUBCLASS_NVM && device->prog_if == NVME_PROGIF_NVMHCI) {
            return device;
        }
    }
    return AXYS_NULL;
}

int axys_nvme_init(void)
{
    const struct axys_pci_device *device;
    axys_uint64_t version;
    axys_uint64_t controller_phys;
    axys_uint32_t max_entries;
    axys_uint32_t controller_major;
    axys_uint32_t active_format;
    axys_uint32_t lba_data_size;
    axys_uint32_t namespace_count;
    axys_uint32_t minimum_bar_offset;
    axys_uint64_t namespace_size;
    axys_uint8_t format_byte;

    present = 0;
    sectors = 0;
    summary[0] = '\0';
    register_base = 0;
    device = find_nvme_controller();
    if (device == AXYS_NULL) {
        axys_snprintf(summary, sizeof(summary), "no NVMe controller on the bus");
        return -1;
    }
    if (device->bars[0] == AXYS_PCI_BAR_NONE || (device->bars[0] & 0xfffu) != 0u) {
        axys_snprintf(summary, sizeof(summary), "NVMe controller has no page-aligned BAR0");
        return -1;
    }
    controller_phys = device->bars[0];
    if (controller_phys >= axys_vmm_physical_limit() ||
        controller_phys > axys_vmm_physical_limit() - 0x1000u) {
        axys_snprintf(summary, sizeof(summary), "BAR0 is outside the mapped MMIO range");
        return -1;
    }

    if (axys_vmm_map_mmio_range(controller_phys, 0x1000u) != 0) {
        axys_snprintf(summary, sizeof(summary), "BAR0 is not a reserved, mappable MMIO range");
        return -1;
    }
    axys_pci_enable_bus_master(device);
    register_base = controller_phys;
    capability = *(volatile axys_uint64_t *)reg64(REG_CAP);
    max_entries = CAP_MQES(capability);
    doorbell_stride = CAP_DSTRD(capability);
    if (max_entries < QUEUE_MIN_DEPTH || (CAP_CSS(capability) & 1u) == 0u ||
        CAP_MPSMIN(capability) != 0u || CAP_MPSMAX(capability) < CAP_MPSMIN(capability)) {
        axys_snprintf(summary, sizeof(summary), "controller lacks required 4 KiB NVM queue support");
        return -1;
    }
    queue_depth = max_entries < QUEUE_MAX_DEPTH ? max_entries : QUEUE_MAX_DEPTH;
    minimum_bar_offset = REG_DOORBELL + (12u << doorbell_stride) + sizeof(axys_uint32_t);
    if (controller_phys > axys_vmm_physical_limit() - minimum_bar_offset) {
        axys_snprintf(summary, sizeof(summary), "NVMe doorbells extend outside the mapped MMIO range");
        return -1;
    }
    if (axys_vmm_map_mmio_range(controller_phys, minimum_bar_offset) != 0) {
        axys_snprintf(summary, sizeof(summary), "NVMe doorbells are not in reserved MMIO space");
        return -1;
    }
    /* CAP.TO: worst-case time for CSTS.RDY to reach 1 after CC.EN is set, in
     * 500 ms units. Controllers are allowed to advertise up to 127.5 s; use
     * the real value instead of a fixed guess so slow-but-healthy hardware is
     * not declared dead, while a broken one still fails within a bound. */
    startup_timeout_ms = CAP_TO_MS(capability);
    if (startup_timeout_ms == 0u || startup_timeout_ms > NVME_TIMEOUT_READY_MS) {
        startup_timeout_ms = NVME_TIMEOUT_READY_MS;
    }
    version = read_reg32(REG_VS);
    controller_major = (axys_uint32_t)(version >> 16);
    if (controller_major != 1u) {
        axys_snprintf(summary, sizeof(summary), "unsupported NVMe version %u.%u",
                      controller_major, (axys_uint32_t)((version >> 8) & 0xffu));
        return -1;
    }

    /* Interrupts remain masked: all completions are polled. */
    write_reg32(REG_INTMS, 0xffffffffu);
    write_reg32(REG_CC, read_reg32(REG_CC) & ~CC_EN);
    /* A disable transition has no advertised bound in CAP (TO covers startup
     * only); NVMe implementations take RDY down within their startup timeout
     * in practice, so reuse it with a sane fallback. */
    if (wait_ready(0, startup_timeout_ms) != 0) {
        axys_snprintf(summary, sizeof(summary), "controller did not stop");
        return -1;
    }

    axys_memset(admin_sq, 0, sizeof(admin_sq));
    axys_memset(admin_cq, 0, sizeof(admin_cq));
    axys_memset(io_sq, 0, sizeof(io_sq));
    axys_memset(io_cq, 0, sizeof(io_cq));
    admin_sq_tail = 0;
    admin_cq_head = 0;
    admin_cq_phase = 1;
    io_sq_tail = 0;
    io_cq_head = 0;
    io_cq_phase = 1;
    next_command_id = 0;
    last_command_status = 0;

    write_reg32(REG_AQA, ((queue_depth - 1u) << 16) | (queue_depth - 1u));
    *reg64(REG_ASQ) = (axys_uint64_t)(axys_uintptr_t)admin_sq;
    *reg64(REG_ACQ) = (axys_uint64_t)(axys_uintptr_t)admin_cq;
    __sync_synchronize();
    write_reg32(REG_CC, CC_EN | CC_IOSQES_64 | CC_IOCQES_16);
    if (wait_ready(1, startup_timeout_ms) != 0) {
        axys_snprintf(summary, sizeof(summary), "controller failed to become ready");
        return -1;
    }

    if (identify(0, ADMIN_IDENTIFY_CONTROLLER) != 0) {
        axys_snprintf(summary, sizeof(summary), "Identify Controller command failed");
        return -1;
    }
    namespace_count = read_le32(identify_page + 516u);
    if (namespace_count == 0u) {
        axys_snprintf(summary, sizeof(summary), "controller reports zero namespaces (NN=%u)",
                      namespace_count);
        return -1;
    }
    if (identify(1, ADMIN_IDENTIFY_NAMESPACE) != 0) {
        axys_snprintf(summary, sizeof(summary), "Identify Namespace 1 failed (status 0x%04x)",
                      last_command_status);
        return -1;
    }
    namespace_size = read_le64(identify_page);
    format_byte = identify_page[26];
    active_format = format_byte & 0x0fu;
    if (active_format > identify_page[25]) {
        axys_snprintf(summary, sizeof(summary), "namespace reports an invalid LBA format");
        return -1;
    }
    lba_data_size = identify_page[128u + active_format * 4u + 2u];
    if (lba_data_size != 9u || namespace_size == 0u) {
        axys_snprintf(summary, sizeof(summary), "namespace does not use 512-byte logical blocks");
        return -1;
    }
    if (create_io_queues() != 0) {
        axys_snprintf(summary, sizeof(summary), "could not create an I/O queue pair");
        return -1;
    }

    sectors = namespace_size;
    present = 1;
    axys_snprintf(summary, sizeof(summary), "%02x:%02x.%u, namespace 1, %llu sectors",
                  device->bus, device->device, device->function,
                  (unsigned long long)sectors);
    return 0;
}

axys_uint64_t axys_nvme_sectors(void)
{
    return sectors;
}

const char *axys_nvme_summary(void)
{
    return summary;
}

static int request_valid(axys_uint64_t lba, axys_uint32_t count, const void *buffer)
{
    return present && count != 0u && buffer != AXYS_NULL && lba < sectors &&
           (axys_uint64_t)count <= sectors - lba;
}

int axys_nvme_read(axys_uint64_t lba, axys_uint32_t count, void *buffer)
{
    axys_uint8_t *output = (axys_uint8_t *)buffer;
    axys_uint64_t flags;
    int result = 0;

    if (!request_valid(lba, count, buffer)) {
        return -1;
    }
    flags = axys_spin_lock_irqsave(&nvme_lock);
    while (count != 0u) {
        axys_uint32_t chunk = count > SECTORS_PER_PAGE ? SECTORS_PER_PAGE : count;
        axys_size_t bytes = chunk * SECTOR_SIZE;

        if (submit_io(IO_READ, lba, chunk) != 0) {
            result = -1;
            break;
        }
        axys_memcpy(output, transfer_page, bytes);
        output += bytes;
        lba += chunk;
        count -= chunk;
    }
    axys_spin_unlock_irqrestore(&nvme_lock, flags);
    return result;
}

int axys_nvme_write(axys_uint64_t lba, axys_uint32_t count, const void *buffer)
{
    const axys_uint8_t *input = (const axys_uint8_t *)buffer;
    axys_uint64_t flags;
    int result = 0;

    if (!request_valid(lba, count, buffer)) {
        return -1;
    }
    flags = axys_spin_lock_irqsave(&nvme_lock);
    while (count != 0u) {
        axys_uint32_t chunk = count > SECTORS_PER_PAGE ? SECTORS_PER_PAGE : count;
        axys_size_t bytes = chunk * SECTOR_SIZE;

        axys_memcpy(transfer_page, input, bytes);
        if (submit_io(IO_WRITE, lba, chunk) != 0) {
            result = -1;
            break;
        }
        input += bytes;
        lba += chunk;
        count -= chunk;
    }
    axys_spin_unlock_irqrestore(&nvme_lock, flags);
    return result;
}

int axys_nvme_flush(void)
{
    axys_uint64_t flags;
    int result;

    if (!present) {
        return -1;
    }
    flags = axys_spin_lock_irqsave(&nvme_lock);
    result = submit_io(IO_FLUSH, 0u, 0u);
    axys_spin_unlock_irqrestore(&nvme_lock, flags);
    return result;
}
