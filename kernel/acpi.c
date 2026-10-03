#include "axys/acpi.h"
#include "axys/io.h"
#include "axys/printf.h"
#include "axys/string.h"
#include "axys/vmm.h"

/* ACPI 6.x table layout. Offsets include the 36-byte common SDT header. */
#define ACPI_SDT_HEADER_SIZE 36u
#define ACPI_MAX_TABLE_SIZE (1024u * 1024u)
#define RSDP_SCAN_START 0x0e0000u
#define RSDP_SCAN_END 0x100000u
#define RSDP_STRIDE 16u

#define RSDP_SIG "RSD PTR "
#define RSDT_SIG "RSDT"
#define XSDT_SIG "XSDT"
#define FADT_SIG "FACP"
#define DSDT_SIG "DSDT"

#define FADT_DSDT 40u
#define FADT_PM1A_CNT 64u
#define FADT_PM1B_CNT 68u
#define FADT_FLAGS 112u
#define FADT_RESET_REGISTER 116u
#define FADT_RESET_VALUE 128u
#define FADT_X_DSDT 140u
#define FADT_X_PM1A_CNT 172u
#define FADT_X_PM1B_CNT 184u
#define FADT_PM1_CNT_LEN 89u
#define FADT_RESET_REG_SUP (1u << 10)
#define FADT_HW_REDUCED_ACPI (1u << 20)

#define GAS_SYSTEM_MEMORY 0u
#define GAS_SYSTEM_IO 1u

#define PM1_SLP_TYP_SHIFT 10u
#define PM1_SLP_TYP_MASK (7u << PM1_SLP_TYP_SHIFT)
#define PM1_SLP_EN (1u << 13)

#define PS2_STATUS 0x64u
#define PS2_RESET_CMD 0xfeu
#define RESET_PORT 0xcf9u
#define RESET_VALUE_CPU 0x06u

struct acpi_register {
    axys_uint64_t address;
    axys_uint8_t space;
    int valid;
};

static struct acpi_register pm1a;
static struct acpi_register pm1b;
static struct acpi_register reset_register;
static axys_uint8_t s5_type_a;
static axys_uint8_t s5_type_b;
static axys_uint8_t reset_value;
static int have_fadt;
static int have_s5;
static int have_reset_register;
static int rsdp_from_boot_tag;
static char summary[192];

static axys_uint32_t load32(const volatile axys_uint8_t *p)
{
    return (axys_uint32_t)p[0] | ((axys_uint32_t)p[1] << 8) |
           ((axys_uint32_t)p[2] << 16) | ((axys_uint32_t)p[3] << 24);
}

static axys_uint64_t load64(const volatile axys_uint8_t *p)
{
    return (axys_uint64_t)load32(p) | ((axys_uint64_t)load32(p + 4) << 32);
}

static axys_uint8_t sum8(const volatile axys_uint8_t *bytes, axys_size_t length)
{
    axys_uint8_t sum = 0;

    for (axys_size_t i = 0; i < length; ++i) {
        sum = (axys_uint8_t)(sum + bytes[i]);
    }
    return sum;
}

static int sig_is(const volatile axys_uint8_t *bytes, const char *signature,
                  axys_size_t length)
{
    for (axys_size_t i = 0; i < length; ++i) {
        if (bytes[i] != (axys_uint8_t)signature[i]) {
            return 0;
        }
    }
    return 1;
}

/* Physical ACPI pointers are usable only while the current identity map covers
 * the complete object. Reject a wraparound or a table that straddles the map. */
static const volatile axys_uint8_t *physical_bytes(axys_uint64_t address,
                                                   axys_size_t length)
{
    axys_uint64_t limit = axys_vmm_identity_limit();

    if (address == 0u || length == 0 || address >= limit ||
        (axys_uint64_t)length > limit - address ||
        address > (axys_uint64_t)(~(axys_uintptr_t)0)) {
        return AXYS_NULL;
    }
    return (const volatile axys_uint8_t *)(axys_uintptr_t)address;
}

/* Validate a complete System Description Table before any caller reads fields
 * beyond the common header. This rejects stale, truncated and corrupt tables. */
static const volatile axys_uint8_t *validated_sdt(axys_uint64_t address,
                                                  const char *signature,
                                                  axys_uint32_t *length_out)
{
    const volatile axys_uint8_t *table = physical_bytes(address, ACPI_SDT_HEADER_SIZE);
    axys_uint32_t length;

    if (table == AXYS_NULL || !sig_is(table, signature, 4u)) {
        return AXYS_NULL;
    }
    length = load32(table + 4);
    if (length < ACPI_SDT_HEADER_SIZE || length > ACPI_MAX_TABLE_SIZE ||
        physical_bytes(address, length) == AXYS_NULL || sum8(table, length) != 0u) {
        return AXYS_NULL;
    }
    if (length_out != AXYS_NULL) {
        *length_out = length;
    }
    return table;
}

static int valid_rsdp(const volatile axys_uint8_t *candidate, axys_size_t available)
{
    if (candidate == AXYS_NULL || available < 20u ||
        !sig_is(candidate, RSDP_SIG, 8u) || sum8(candidate, 20u) != 0u) {
        return 0;
    }
    if (candidate[15] >= 2u) {
        axys_uint32_t length;

        if (available < 36u) {
            return 0;
        }
        length = load32(candidate + 20);
        if (length < 36u || length > 4096u || length > available ||
            sum8(candidate, length) != 0u) {
            return 0;
        }
    }
    return 1;
}

/* Prefer the RSDP copied into a Multiboot2 ACPI tag. This also covers the EBDA
 * location that cannot be read through the intentionally unmapped null page.
 * The BIOS scan remains the fallback for loaders that omit the optional tag. */
static const volatile axys_uint8_t *find_rsdp(const volatile axys_uint8_t *boot_rsdp,
                                              axys_size_t boot_rsdp_length)
{
    if (boot_rsdp != AXYS_NULL && boot_rsdp_length <= ACPI_MAX_TABLE_SIZE &&
        physical_bytes((axys_uint64_t)(axys_uintptr_t)boot_rsdp,
                       boot_rsdp_length) != AXYS_NULL &&
        valid_rsdp(boot_rsdp, boot_rsdp_length)) {
        rsdp_from_boot_tag = 1;
        return boot_rsdp;
    }

    for (axys_uint64_t address = RSDP_SCAN_START; address < RSDP_SCAN_END;
         address += RSDP_STRIDE) {
        const volatile axys_uint8_t *candidate = physical_bytes(address, 20u);

        if (candidate == AXYS_NULL || candidate[0] != (axys_uint8_t)'R') {
            continue;
        }
        if (candidate[15] >= 2u) {
            axys_uint32_t length;

            candidate = physical_bytes(address, 36u);
            if (candidate == AXYS_NULL) {
                continue;
            }
            length = load32(candidate + 20);
            if (physical_bytes(address, length) == AXYS_NULL ||
                !valid_rsdp(candidate, length)) {
                continue;
            }
        } else if (!valid_rsdp(candidate, 20u)) {
            continue;
        }
        return candidate;
    }
    return AXYS_NULL;
}

static const volatile axys_uint8_t *find_fadt_in_root(axys_uint64_t address,
                                                      int extended)
{
    const volatile axys_uint8_t *root;
    axys_uint32_t length;
    axys_uint32_t entry_size = extended ? 8u : 4u;
    const char *signature = extended ? XSDT_SIG : RSDT_SIG;

    root = validated_sdt(address, signature, &length);
    if (root == AXYS_NULL || (length - ACPI_SDT_HEADER_SIZE) % entry_size != 0u) {
        return AXYS_NULL;
    }
    for (axys_uint32_t offset = ACPI_SDT_HEADER_SIZE; offset < length;
         offset += entry_size) {
        axys_uint64_t child = extended ? load64(root + offset) : load32(root + offset);
        const volatile axys_uint8_t *table;

        if (child == 0u) {
            continue;
        }
        table = validated_sdt(child, FADT_SIG, AXYS_NULL);
        if (table != AXYS_NULL) {
            return table;
        }
    }
    return AXYS_NULL;
}

static const volatile axys_uint8_t *find_fadt(const volatile axys_uint8_t *rsdp)
{
    const volatile axys_uint8_t *fadt = AXYS_NULL;

    if (rsdp[15] >= 2u) {
        fadt = find_fadt_in_root(load64(rsdp + 24), 1);
    }
    if (fadt == AXYS_NULL) {
        fadt = find_fadt_in_root(load32(rsdp + 16), 0);
    }
    return fadt;
}

/* Decode an ACPI Generic Address Structure. The implementation supports only
 * ordinary System Memory and System I/O registers; PCI config, embedded
 * controllers and unsupported address spaces fail closed. */
static int decode_gas(const volatile axys_uint8_t *gas, axys_uint8_t min_width,
                      struct acpi_register *reg)
{
    axys_uint8_t space = gas[0];
    axys_uint8_t width = gas[1];
    axys_uint8_t bit_offset = gas[2];
    axys_uint8_t access_size = gas[3];
    axys_uint64_t address = load64(gas + 4);

    if (address == 0u || bit_offset != 0u || width < min_width ||
        (access_size != 0u && access_size != 1u && access_size != 2u) ||
        (access_size == 2u && min_width <= 8u)) {
        return 0;
    }
    if (space == GAS_SYSTEM_IO) {
        if (address > 0xffffu || address + ((min_width + 7u) / 8u) - 1u > 0xffffu ||
            (access_size == 1u && min_width > 8u)) {
            return 0;
        }
    } else if (space == GAS_SYSTEM_MEMORY) {
        if (physical_bytes(address, (min_width + 7u) / 8u) == AXYS_NULL ||
            (access_size == 1u && min_width > 8u)) {
            return 0;
        }
    } else {
        return 0;
    }
    reg->address = address;
    reg->space = space;
    reg->valid = 1;
    return 1;
}

static int decode_legacy_pm1(const volatile axys_uint8_t *fadt,
                             axys_uint32_t fadt_length, axys_uint32_t offset,
                             struct acpi_register *reg)
{
    axys_uint32_t address;
    axys_uint8_t length;

    if (fadt_length <= FADT_PM1_CNT_LEN || fadt_length < offset + 4u) {
        return 0;
    }
    address = load32(fadt + offset);
    length = fadt[FADT_PM1_CNT_LEN];
    if (address == 0u || length < 2u || address > 0xffffu || address + 1u > 0xffffu) {
        return 0;
    }
    reg->address = address;
    reg->space = GAS_SYSTEM_IO;
    reg->valid = 1;
    return 1;
}

static int decode_pm1(const volatile axys_uint8_t *fadt,
                      axys_uint32_t fadt_length, axys_uint32_t extended_offset,
                      axys_uint32_t legacy_offset, struct acpi_register *reg)
{
    if (fadt_length >= extended_offset + 12u &&
        load64(fadt + extended_offset + 4u) != 0u &&
        decode_gas(fadt + extended_offset, 16u, reg)) {
        return 1;
    }
    return decode_legacy_pm1(fadt, fadt_length, legacy_offset, reg);
}

static int reg_read16(const struct acpi_register *reg, axys_uint16_t *value)
{
    if (!reg->valid || value == AXYS_NULL) {
        return 0;
    }
    if (reg->space == GAS_SYSTEM_IO) {
        *value = axys_inw((axys_uint16_t)reg->address);
    } else {
        const volatile axys_uint16_t *memory =
            (const volatile axys_uint16_t *)(axys_uintptr_t)reg->address;

        *value = *memory;
    }
    return 1;
}

static int reg_write16(const struct acpi_register *reg, axys_uint16_t value)
{
    if (!reg->valid) {
        return 0;
    }
    if (reg->space == GAS_SYSTEM_IO) {
        axys_outw((axys_uint16_t)reg->address, value);
    } else {
        volatile axys_uint16_t *memory = (volatile axys_uint16_t *)(axys_uintptr_t)reg->address;

        *memory = value;
    }
    return 1;
}

static void reg_write8(const struct acpi_register *reg, axys_uint8_t value)
{
    if (reg->space == GAS_SYSTEM_IO) {
        axys_outb((axys_uint16_t)reg->address, value);
    } else {
        volatile axys_uint8_t *memory = (volatile axys_uint8_t *)(axys_uintptr_t)reg->address;

        *memory = value;
    }
}

static int reg_location(const struct acpi_register *reg, char *space, axys_uint32_t *address)
{
    if (!reg->valid) {
        return 0;
    }
    *space = reg->space == GAS_SYSTEM_IO ? 'I' : 'M';
    *address = (axys_uint32_t)reg->address;
    return 1;
}

int axys_acpi_init(const volatile axys_uint8_t *boot_rsdp,
                   axys_size_t boot_rsdp_length)
{
    const volatile axys_uint8_t *rsdp;
    const volatile axys_uint8_t *fadt;
    const volatile axys_uint8_t *dsdt;
    axys_uint32_t fadt_length;
    axys_uint32_t dsdt_length;
    axys_uint32_t flags;
    axys_uint64_t dsdt_address;
    char pm1a_space = '?';
    char pm1b_space = '?';
    axys_uint32_t pm1a_address = 0;
    axys_uint32_t pm1b_address = 0;

    pm1a.valid = 0;
    pm1b.valid = 0;
    reset_register.valid = 0;
    have_fadt = 0;
    have_s5 = 0;
    have_reset_register = 0;
    rsdp_from_boot_tag = 0;
    s5_type_a = s5_type_b = reset_value = 0;
    summary[0] = '\0';

    rsdp = find_rsdp(boot_rsdp, boot_rsdp_length);
    if (rsdp == AXYS_NULL) {
        axys_snprintf(summary, sizeof(summary), "no valid RSDP in BIOS range");
        return -1;
    }
    fadt = find_fadt(rsdp);
    if (fadt == AXYS_NULL) {
        axys_snprintf(summary, sizeof(summary), "no valid FADT in RSDT/XSDT");
        return -1;
    }
    fadt_length = load32(fadt + 4u);
    if (fadt_length < FADT_PM1_CNT_LEN + 1u) {
        axys_snprintf(summary, sizeof(summary), "FADT too short for PM1 registers");
        return -1;
    }

    flags = fadt_length >= FADT_FLAGS + 4u ? load32(fadt + FADT_FLAGS) : 0u;
    if ((flags & FADT_HW_REDUCED_ACPI) != 0u) {
        axys_snprintf(summary, sizeof(summary), "hardware-reduced ACPI is unsupported");
        return -1;
    }
    (void)decode_pm1(fadt, fadt_length, FADT_X_PM1A_CNT, FADT_PM1A_CNT, &pm1a);
    (void)decode_pm1(fadt, fadt_length, FADT_X_PM1B_CNT, FADT_PM1B_CNT, &pm1b);
    if (!pm1a.valid) {
        axys_snprintf(summary, sizeof(summary), "FADT has no usable PM1a control register");
        return -1;
    }

    if (fadt_length >= FADT_X_DSDT + 8u && load64(fadt + FADT_X_DSDT) != 0u) {
        dsdt_address = load64(fadt + FADT_X_DSDT);
    } else {
        dsdt_address = load32(fadt + FADT_DSDT);
    }
    dsdt = validated_sdt(dsdt_address, DSDT_SIG, &dsdt_length);
    if (dsdt != AXYS_NULL && dsdt_length > ACPI_SDT_HEADER_SIZE &&
        axys_acpi_extract_s5(dsdt + ACPI_SDT_HEADER_SIZE,
                             dsdt_length - ACPI_SDT_HEADER_SIZE,
                             &s5_type_a, &s5_type_b) == 0) {
        have_s5 = 1;
    }

    if ((flags & FADT_RESET_REG_SUP) != 0u &&
        fadt_length >= FADT_RESET_VALUE + 1u &&
        fadt_length >= FADT_RESET_REGISTER + 12u &&
        decode_gas(fadt + FADT_RESET_REGISTER, 8u, &reset_register)) {
        reset_value = fadt[FADT_RESET_VALUE];
        have_reset_register = 1;
    }

    reg_location(&pm1a, &pm1a_space, &pm1a_address);
    if (pm1b.valid) {
        reg_location(&pm1b, &pm1b_space, &pm1b_address);
    }
    axys_snprintf(summary, sizeof(summary),
                  "RSDP %s; PM1a %s:0x%x, PM1b %s, S5 %s%u/%u, reset %s",
                  rsdp_from_boot_tag ? "Multiboot2" : "BIOS scan",
                  pm1a_space == 'I' ? "I/O" : "MMIO",
                  pm1a_address,
                  pm1b.valid ? (pm1b_space == 'I' ? "I/O" : "MMIO") : "absent",
                  have_s5 ? "" : "unavailable ", have_s5 ? s5_type_a : 0u,
                  have_s5 ? s5_type_b : 0u,
                  have_reset_register ? "FADT GAS" : "fallback");
    if (pm1b.valid) {
        axys_size_t used = axys_strlen(summary);
        if (used < sizeof(summary)) {
            axys_snprintf(summary + used, sizeof(summary) - used, " at 0x%x", pm1b_address);
        }
    }
    have_fadt = 1;
    return 0;
}

int axys_acpi_available(void)
{
    return have_fadt && have_s5;
}

const char *axys_acpi_summary(void)
{
    return summary;
}

static AXYS_NORETURN void halt_forever(void)
{
    __asm__ volatile("cli" ::: "memory");
    for (;;) {
        __asm__ volatile("hlt");
    }
}

AXYS_NORETURN void axys_acpi_poweroff(void)
{
    axys_uint16_t current;
    axys_uint16_t control;

    if (!have_fadt || !have_s5 || !reg_read16(&pm1a, &current)) {
        axys_printf("acpi: safe poweroff unavailable (%s)\n",
                    summary[0] != '\0' ? summary : "ACPI not initialized");
        halt_forever();
    }

    control = (axys_uint16_t)(current & (axys_uint16_t)~(PM1_SLP_TYP_MASK | PM1_SLP_EN));
    control |= (axys_uint16_t)(((axys_uint16_t)s5_type_a << PM1_SLP_TYP_SHIFT) | PM1_SLP_EN);
    (void)reg_write16(&pm1a, control);
    if (pm1b.valid && reg_read16(&pm1b, &current)) {
        control = (axys_uint16_t)(current & (axys_uint16_t)~(PM1_SLP_TYP_MASK | PM1_SLP_EN));
        control |= (axys_uint16_t)(((axys_uint16_t)s5_type_b << PM1_SLP_TYP_SHIFT) | PM1_SLP_EN);
        (void)reg_write16(&pm1b, control);
    }
    halt_forever();
}

static void settle(void)
{
    for (axys_uint32_t i = 0; i < 200000u; ++i) {
        __asm__ volatile("" ::: "memory");
    }
}

AXYS_NORETURN void axys_acpi_reboot(void)
{
    if (have_reset_register) {
        reg_write8(&reset_register, reset_value);
        settle();
    }

    /* Legacy 8042 reset path. Wait for the controller input buffer to empty. */
    for (axys_uint32_t i = 0; i < 100000u; ++i) {
        if ((axys_inb(PS2_STATUS) & 0x02u) == 0u) {
            break;
        }
        __asm__ volatile("" ::: "memory");
    }
    axys_outb(PS2_STATUS, PS2_RESET_CMD);
    settle();
    axys_outb(RESET_PORT, RESET_VALUE_CPU);
    halt_forever();
}
