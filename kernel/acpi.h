#ifndef BMX_ACPI_H
#define BMX_ACPI_H

#include <stdint.h>

/* Firmware description of the machine: RSDP discovery, the XSDT/RSDT walk, and
   the MADT and MCFG contents the rest of the kernel needs.

   Every address inside those structures is physical. This module translates
   through the direct map before it reads, and it validates each checksum before
   trusting a table, because the tables are firmware data and not kernel data. */
#define ACPI_MAX_TABLES 32

typedef struct {
    char signature[5];          /* four characters plus a terminator */
    uint64_t physical_address;
    const void *virtual_address; /* through the direct map */
    uint32_t length;
    uint8_t revision;
} acpi_table_t;

/* Returns 0 when an RSDP was found and at least the root table was readable.
   A partial result (no MADT, no MCFG) is still a success: not every firmware
   publishes every table. */
int acpi_init(void);
int acpi_ready(void);

uint32_t acpi_table_count(void);
const acpi_table_t *acpi_table_at(uint32_t index);
const acpi_table_t *acpi_find(const char *signature);

/* MADT: the physical address the CPU uses to reach its own local APIC, plus how
   many IO APICs and interrupt source overrides the platform declares. */
uint64_t acpi_local_apic(void);
uint32_t acpi_ioapic_count(void);
uint32_t acpi_interrupt_override_count(void);

/* MCFG: the PCIe ECAM window, when the firmware published one. Returns 0 and
   fills the outputs on success, -1 when there is no usable entry. */
int acpi_ecam(uint64_t *base, uint8_t *first_bus, uint8_t *last_bus);

/* Physical address of the RSDP this kernel validated, or 0 before acpi_init has
   found one. Linux needs it on the UEFI path: there the pointer only exists in
   the firmware configuration table, which nothing can scan for after the
   loader has left boot services. */
uint64_t acpi_rsdp_physical(void);

void acpi_print(void);

#endif
