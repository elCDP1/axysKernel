#ifndef AXYS_ACPI_H
#define AXYS_ACPI_H

#include "axys/types.h"

/* ACPI: locate the tables the firmware published, and drive power management
 * through the real PM1 control block instead of poking a fixed port that only
 * happens to be right on one emulator. */

/* Validate the bootloader-provided RSDP when available, then search the BIOS
 * range as a fallback. Safe to call once, after the identity map exists. */
int axys_acpi_init(const volatile axys_uint8_t *boot_rsdp,
                   axys_size_t boot_rsdp_length);

int axys_acpi_available(void);

/* Human-readable one-line summary for the boot log. Returns "" when ACPI is
 * not available. Never AXYS_NULL. */
const char *axys_acpi_summary(void);

/* Extract the two S5 sleep-type values from the firmware's AML byte stream.
 * This deliberately handles only the static Name (_S5, Package (...)) form;
 * it does not execute AML or methods. Returns 0 when a safe pair was found. */
int axys_acpi_extract_s5(const volatile axys_uint8_t *aml, axys_size_t length,
                         axys_uint8_t *type_a, axys_uint8_t *type_b);

/* Power off / reboot the machine through validated firmware-described ACPI
 * registers. Neither returns. If firmware does not provide a supported path,
 * the kernel reports the limitation and halts instead of writing guessed
 * chipset ports. */
AXYS_NORETURN void axys_acpi_poweroff(void);
AXYS_NORETURN void axys_acpi_reboot(void);

#endif
