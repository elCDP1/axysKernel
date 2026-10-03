#include "axys/kernel.h"
#include "axys/console.h"
#include "axys/cpu.h"
#include "axys/exceptions.h"
#include "axys/gdt.h"
#include "axys/heap.h"
#include "axys/idt.h"
#include "axys/interrupts.h"
#include "axys/multiboot2.h"
#include "axys/panic.h"
#include "axys/pit.h"
#include "axys/pagetest.h"
#include "axys/pmm.h"
#include "axys/printf.h"
#include "axys/acpi.h"
#include "axys/ahci.h"
#include "axys/disk.h"
#include "axys/initrd.h"
#include "axys/pci.h"
#include "axys/persist.h"
#include "axys/input.h"
#include "axys/process.h"
#include "axys/syscall.h"
#include "axys/random.h"
#include "axys/sched.h"
#include "axys/vfs.h"
#include "axys/vmm.h"

/* NO_SSP: this frame is live while the canary is replaced. */
__attribute__((no_stack_protector))
void axys_kmain(axys_uint32_t multiboot_magic, axys_uint32_t multiboot_information)
{
    struct axys_boot_info boot_info;

    /* The IDT must be a valid IA-32e table before anything else can fault:
     * boot.S only installs a 32-bit gate table, which the long-mode CPU would
     * misread as 16-byte gates. But the GDT/TSS/IST stacks come first: the
     * IDT's #DF gate references IST 1, and an IST reference into a TSS that
     * has not been loaded yet is a latent triple-fault window. Establishing
     * GDT -> TSS -> IDT in that order means every gate that exists points at
     * machinery that already works. */
    axys_gdt_init();
    axys_idt_init();
    axys_console_init();
    axys_pic_remap();

    axys_printf("axys kernel online\n");

    axys_printf("multiboot magic=0x%08x information=0x%08x\n", multiboot_magic,
                multiboot_information);

    switch (axys_multiboot2_parse((axys_uintptr_t)multiboot_information,
                                  multiboot_magic, &boot_info)) {
    case AXYS_MB2_OK:
        break;
    case AXYS_MB2_ERR_NULL:
        axys_panic("multiboot2 parse: null boot_info");
        break;
    case AXYS_MB2_ERR_ADDRESS:
        axys_panic("multiboot2 parse: implausible info address");
        break;
    case AXYS_MB2_ERR_MAGIC:
        axys_panic("multiboot2 parse: bad info header magic");
        break;
    case AXYS_MB2_ERR_SIZE:
        axys_panic("multiboot2 parse: bad header_length");
        break;
    case AXYS_MB2_ERR_NO_MMAP:
        /*
         * Bootable, but no allocator is possible: mem_lower/mem_upper are only
         * totals and say nothing about which regions are firmware-reserved.
         * Say so loudly rather than letting a later stage size a heap off a
         * figure that cannot support it.
         */
        axys_printf("FATAL: bootloader supplied no memory map; no physical "
                    "allocator can be built\n");
        axys_panic("multiboot2 parse: no mmap tag");
        break;
    default:
        axys_panic("multiboot2 parse: unknown status");
        break;
    }
    axys_vmm_set_memory_map(&boot_info);

    axys_printf("memory lower=%u KiB upper=%u KiB mmap_entries=%u\n",
                boot_info.mem_lower_kib, boot_info.mem_upper_kib,
                (axys_uint32_t)boot_info.mmap_entries);
    axys_printf("mmap: stride=%u version=%u highest=0x%016llx available=%u MiB\n",
                (axys_uint32_t)boot_info.mmap_entry_size,
                boot_info.mmap_entry_version,
                (axys_uint64_t)axys_mmap_highest_address(&boot_info),
                (axys_uint32_t)(boot_info.mem_available_bytes / (1024u * 1024u)));
    {
        axys_uint64_t smx = axys_cpu_enable_smep_smap();

        axys_printf("cpu: smep %s, smap %s\n",
                    (smx & AXYS_CR4_SMEP) ? "enabled" : "unavailable",
                    (smx & AXYS_CR4_SMAP) ? "enabled" : "unavailable");
    }

    axys_printf("cpu: %u logical, rdrand=%d rdseed=%d smep=%d smap=%d "
                "invariant_tsc=%d apic=%d x2apic=%d\n",
                axys_cpu_count(), axys_cpu_has_rdrand(), axys_cpu_has_rdseed(),
                axys_cpu_has_smep(), axys_cpu_has_smap(),
                axys_cpu_has_invariant_tsc(), axys_cpu_has_apic(),
                axys_cpu_has_x2apic());
    axys_printf("console: serial 115200 8n1, VGA text 80x25\n");

    if (axys_pagetest_selftest() != 0) {
        axys_panic("boot page tables failed verification");
    }

    axys_pmm_init(&boot_info);
    if (axys_pmm_extend(&boot_info) != 0) {
        axys_printf("pmm: high memory extension unavailable; using low memory map\n");
    }
    axys_pmm_selftest();
    axys_printf("pmm: %u KiB free / %u KiB total (physical address limit %u MiB)\n",
                (axys_uint32_t)(axys_pmm_free_frames() * (AXYS_PMM_FRAME_SIZE / 1024u)),
                (axys_uint32_t)(axys_pmm_total_frames() * (AXYS_PMM_FRAME_SIZE / 1024u)),
                (axys_uint32_t)(axys_pmm_identity_limit() / (1024u * 1024u)));

    axys_heap_init();
    axys_heap_selftest();
    axys_printf("heap: %u bytes in use, %u bytes free\n",
                (axys_uint32_t)axys_heap_bytes_in_use(), (axys_uint32_t)axys_heap_bytes_free());

    if (axys_random_init() == 0) {
        axys_printf("rng: chacha20 ready (%s)\n",
                    axys_random_source() == AXYS_RANDOM_SOURCE_HARDWARE ? "hardware DRNG"
                                                                        : "timing-jitter fallback");
        if (axys_ssp_reseed() == 0) {
            axys_printf("ssp: kernel stack canary randomised\n");
        }
    } else {
        axys_printf("rng: FAILED to gather entropy\n");
    }

    axys_vfs_init();
    axys_vfs_selftest();
    axys_printf("vfs: root hierarchy:\n");
    axys_vfs_dump();

    /*
     * ACPI and PCI before anything that needs firmware tables or a bus. Both
     * are pure lookups over memory the firmware already published, so they only
     * need the console and the identity map, both of which exist by now.
     */
    if (axys_acpi_init(boot_info.acpi_rsdp, boot_info.acpi_rsdp_length) == 0) {
        axys_printf("acpi: %s\n", axys_acpi_summary());
    } else {
        axys_printf("acpi: %s; ACPI poweroff/reboot support is limited\n",
                    axys_acpi_summary());
    }
    {
        int buses = axys_pci_scan();

        axys_printf("pci: %d functions", buses);
        for (int i = 0; i < buses; ++i) {
            const struct axys_pci_device *device = axys_pci_device_at(i);

            axys_printf(" %02x:%02x.%u[%04x:%04x]", device->bus, device->device,
                        device->function, device->vendor_id, device->device_id);
        }
        axys_printf("\n");
    }
    /* The probe is idempotent, so persist_init asking again later is free. */
    (void)axys_disk_init();
    axys_printf("disk: %s\n", axys_disk_backend_note());

    axys_cpu_enable_interrupts();

    /*
     * Prove the exception path works before entering the idle loop. Nothing in
     * normal startup faults, so a broken IDT or frame layout would otherwise go
     * completely unnoticed until the first real crash -- which, with a broken
     * handler, means a triple fault and no diagnostics.
     */
    axys_exception_selftest();

    /*
     * Then prove interrupts actually arrive and get dispatched. Registering the
     * handler before programming the timer matters: the PIT starts ticking the
     * moment it is initialised, and a tick that lands before the handler is
     * registered would be counted by nobody and the total would be permanently
     * off by one.
     */
    axys_sched_init();
    axys_pit_start();
    axys_pit_init(AXYS_PIT_DEFAULT_HZ);
    axys_pit_sleep_ms(250);

    axys_printf("pit: %u Hz, %u ticks in 250 ms (%u IRQs dispatched)\n",
                axys_pit_hz(), (axys_uint32_t)axys_pit_ticks(),
                (axys_uint32_t)axys_irq_total());
    if (axys_pit_ticks() == 0) {
        axys_printf("pit: WARNING no ticks -- IRQ0 is not being delivered\n");
    }

    axys_sched_selftest();

    axys_process_init();
    axys_syscall_init();
    {
        int files = axys_initrd_install();
        int restored;

        if (files < 0) {
            axys_panic("initrd: cannot install embedded programs");
        }
        axys_printf("initrd: %d programs installed (/sbin, /bin)\n", files);

        restored = axys_persist_init();
        if (restored >= 0) {
            axys_printf("persist: disk %u MiB, restored %d files (snapshot generation %u)\n",
                        (axys_uint32_t)(axys_disk_sectors() / 2048u), restored,
                        (axys_uint32_t)axys_persist_generation());
        } else if (restored == -2) {
            axys_printf("persist: disk holds foreign data, left untouched; running volatile\n");
        } else {
            axys_printf("persist: no usable disk, running volatile\n");
        }
        if (axys_initrd_install_defaults() != 0) {
            axys_panic("initrd: cannot create default files");
        }
        axys_persist_start_flusher();
    }
    axys_process_selftest();

    if (axys_input_init() == 0) {
        axys_printf("input: PS/2 keyboard + serial RX ready\n");
    } else {
        axys_printf("input: no i8042 controller, serial RX only\n");
    }

    axys_printf("boot complete\n");
    {
        int init_pid = axys_process_spawn("/sbin/init", AXYS_NULL, AXYS_KERNEL_PARENT);

        if (init_pid < 0) {
            axys_panic("cannot start /sbin/init");
        }
        for (;;) {
            int status = 0;

            (void)axys_process_wait(init_pid, AXYS_KERNEL_PARENT, &status);
            axys_printf("init exited (%d); restarting\n", status);
            init_pid = axys_process_spawn("/sbin/init", AXYS_NULL, AXYS_KERNEL_PARENT);
            if (init_pid < 0) {
                axys_panic("cannot restart /sbin/init");
            }
        }
    }

    for (;;) {
        /*
         * sti; hlt, not a bare hlt: IF must be set for the halt to wake up on
         * the next interrupt, and doing both in one instruction closes the
         * window where an interrupt arriving between sti and hlt would be held
         * off for a whole extra idle period.
         */
        axys_cpu_idle();
    }
}
