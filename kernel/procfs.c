#include "axys/procfs.h"
#include "axys/acpi.h"
#include "axys/console.h"
#include "axys/cpu.h"
#include "axys/disk.h"
#include "axys/e1000.h"
#include "axys/kernel.h"
#include "axys/pci.h"
#include "axys/printf.h"
#include "axys/spinlock.h"
#include "axys/string.h"
#include "axys/vfs.h"
#include "axys/xhci.h"

#define PROC_TEXT 8192u

typedef axys_size_t (*generator_t)(char *out, axys_size_t cap);

static char text[PROC_TEXT];
static struct axys_spinlock procfs_lock = AXYS_SPINLOCK_INIT(procfs_lock);

static axys_size_t gen_pci(char *out, axys_size_t cap)
{
    return axys_pci_format_all(out, cap);
}

static axys_size_t clip(int n, axys_size_t cap)
{
    return n < 0 ? 0u : ((axys_size_t)n < cap ? (axys_size_t)n : cap - 1u);
}

static axys_size_t gen_version(char *out, axys_size_t cap)
{
    return clip(axys_snprintf(out, cap, "axysOS %s x86_64\n", AXYS_VERSION), cap);
}

static axys_size_t gen_acpi(char *out, axys_size_t cap)
{
    return clip(axys_snprintf(out, cap, "acpi: %s\n", axys_acpi_summary()), cap);
}

static axys_size_t gen_usb(char *out, axys_size_t cap)
{
    axys_size_t length;
    int slot = axys_usb_storage_slot();

    length = clip(axys_snprintf(out, cap, "usb: %s\nkeyboards: %d\n", axys_xhci_summary(), axys_xhci_keyboard_count()), cap);
    if (slot != 0) {
        axys_uint8_t s = 0, dci_out = 0, dci_in = 0;

        (void)axys_usb_storage_info(&s, &dci_out, &dci_in);
        length += clip(axys_snprintf(out + length, cap - length, "storage: slot %u dci-out %u dci-in %u\n",
                                     (unsigned)s, (unsigned)dci_out, (unsigned)dci_in), cap - length);
    } else {
        length += clip(axys_snprintf(out + length, cap - length, "storage: none\n"), cap - length);
    }
    if (length + 1u < cap) {
        length += axys_xhci_format(out + length, cap - length);
    }
    return length;
}

static axys_size_t gen_disk(char *out, axys_size_t cap)
{
    return clip(axys_snprintf(out, cap, "disk: %s\nsectors: %u\ncapacity_bytes: %llu\n", axys_disk_backend_note(),
                              axys_disk_sectors(), (unsigned long long)axys_disk_capacity() * 512u), cap);
}

static axys_size_t gen_net(char *out, axys_size_t cap)
{
    struct axys_net_stat st;
    axys_size_t length = clip(axys_snprintf(out, cap, "net: %s\n", axys_e1000_summary()), cap);

    if (axys_net_present()) {
        axys_net_stat(&st);
        length += clip(axys_snprintf(out + length, cap - length,
                                     "link: %s %u Mb/s\nmac: %02x:%02x:%02x:%02x:%02x:%02x\nip: %u.%u.%u.%u\n"
                                     "tx: %llu rx: %llu dropped: %llu\n",
                                     st.link ? "up" : "down", (unsigned)st.speed_mbps, st.mac[0], st.mac[1],
                                     st.mac[2], st.mac[3], st.mac[4], st.mac[5], st.ip[0], st.ip[1], st.ip[2],
                                     st.ip[3], (unsigned long long)st.tx_packets, (unsigned long long)st.rx_packets,
                                     (unsigned long long)st.rx_dropped), cap - length);
    }
    return length;
}

static axys_size_t gen_cpu(char *out, axys_size_t cap)
{
    /* Vendor, brand and flags from CPUID. The brand leaves are only read when
     * the CPU reports them: leaves above the maximum return stale data. */
    axys_uint32_t eax, ebx, ecx, edx;
    char vendor[13];
    char brand[49];
    axys_size_t blen;

    axys_cpu_cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    axys_memcpy(vendor, &ebx, 4);
    axys_memcpy(vendor + 4, &edx, 4);
    axys_memcpy(vendor + 8, &ecx, 4);
    vendor[12] = '\0';
    brand[0] = '\0';
    if (axys_cpu_extended_leaf_max() >= 0x80000004u) {
        for (axys_uint32_t leaf = 0; leaf < 3; ++leaf) {
            axys_cpu_cpuid(0x80000002u + leaf, 0, &eax, &ebx, &ecx, &edx);
            axys_memcpy(brand + leaf * 16, &eax, 4);
            axys_memcpy(brand + leaf * 16 + 4, &ebx, 4);
            axys_memcpy(brand + leaf * 16 + 8, &ecx, 4);
            axys_memcpy(brand + leaf * 16 + 12, &edx, 4);
        }
        brand[48] = '\0';
        blen = axys_strlen(brand);
        while (blen > 0 && brand[blen - 1] == ' ') {
            brand[--blen] = '\0';
        }
    }
    return clip(axys_snprintf(out, cap, "vendor: %s\nbrand: %s\nlogical: %u\nfeatures:%s%s%s%s%s%s%s%s\n", vendor, brand,
                              axys_cpu_count(), axys_cpu_has_rdrand() ? " rdrand" : "",
                              axys_cpu_has_rdseed() ? " rdseed" : "", axys_cpu_has_smep() ? " smep" : "",
                              axys_cpu_has_smap() ? " smap" : "", axys_cpu_has_nx() ? " nx" : "",
                              axys_cpu_has_invariant_tsc() ? " invariant-tsc" : "",
                              axys_cpu_has_apic() ? " apic" : "", axys_cpu_has_x2apic() ? " x2apic" : ""), cap);
}

static const struct {
    const char *name;
    generator_t generate;
} files[] = {
    {"pci", gen_pci}, {"version", gen_version}, {"acpi", gen_acpi}, {"usb", gen_usb},
    {"disk", gen_disk}, {"net", gen_net},       {"cpu", gen_cpu},
};

#define FILE_COUNT (sizeof(files) / sizeof(files[0]))

static void publish(unsigned index)
{
    char path[32];
    axys_vfs_node_t node;
    axys_size_t length;
    axys_uint64_t flags = axys_spin_lock_irqsave(&procfs_lock);

    axys_snprintf(path, sizeof(path), "/proc/%s", files[index].name);
    length = files[index].generate(text, sizeof(text));
    node = axys_vfs_lookup(path);
    if (node < 0) {
        node = axys_vfs_create_as(path, AXYS_VFS_FILE, 0444u, 0, 0);
    }
    if (node >= 0) {
        (void)axys_vfs_write(node, text, length);
    }
    axys_spin_unlock_irqrestore(&procfs_lock, flags);
}

void axys_procfs_init(void)
{
    for (unsigned i = 0; i < FILE_COUNT; ++i) {
        publish(i);
    }
}

void axys_procfs_refresh(const char *path)
{
    if (path == AXYS_NULL || axys_strncmp(path, "/proc/", 6) != 0) {
        return;
    }
    for (unsigned i = 0; i < FILE_COUNT; ++i) {
        if (axys_strcmp(path + 6, files[i].name) == 0) {
            publish(i);
            return;
        }
    }
}
