#include "acpi.h"
#include "bootinfo.h"
#include "console.h"
#include "lib.h"
#include "memory.h"

/* The BIOS keeps the EBDA segment selector at this real-mode vector. */
#define BDA_EBDA_VECTOR 0x40E
#define EBDA_SCAN_BYTES 1024
#define EBDA_MIN_ADDRESS 0x80000
#define EBDA_MAX_ADDRESS 0xA0000

/* ACPI 1.0 puts the RSDP in the first KiB of the EBDA; later firmware may put a
   copy in the BIOS read-only area instead. Scan both, aligned to 16 bytes. */
#define BIOS_ROM_START 0xE0000
#define BIOS_ROM_END 0x100000
#define RSDP_SEARCH_STEP 16

#define RSDP_V1_LENGTH 20
#define SDT_HEADER_LENGTH 36
#define MAX_TABLE_LENGTH 0x100000
#define DIRECT_MAP_LIMIT 0x100000000ULL

typedef struct __attribute__((packed)) {
    uint8_t signature[8];
    uint8_t checksum;
    uint8_t oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} rsdp_t;

typedef struct __attribute__((packed)) {
    uint8_t signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    uint8_t oem_id[6];
    uint8_t oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} sdt_header_t;

static acpi_table_t tables[ACPI_MAX_TABLES];
static uint32_t table_count;
static int ready;

static uint64_t rsdp_physical;
static uint8_t rsdp_revision;
static const char *rsdp_source = "none";
static const char *root_table_name = "none";
static uint8_t rsdp_oem[6];

static uint64_t local_apic;
static uint32_t ioapic_count;
static uint32_t override_count;

static uint64_t ecam_base;
static uint8_t ecam_first_bus;
static uint8_t ecam_last_bus;
static int ecam_valid;

static int bytes_equal(const void *left, const char *right, uint32_t count)
{
    const uint8_t *a = left;
    uint32_t index;

    for (index = 0; index < count; index++) {
        if (a[index] != (uint8_t)right[index]) {
            return 0;
        }
    }
    return 1;
}

static uint8_t sum_bytes(const void *data, uint32_t length)
{
    const uint8_t *bytes = data;
    uint8_t sum = 0;
    uint32_t index;

    for (index = 0; index < length; index++) {
        sum = (uint8_t)(sum + bytes[index]);
    }
    return sum;
}

static uint64_t read_u64(const void *base, uint32_t offset)
{
    uint64_t value;

    memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    return value;
}

static uint32_t read_u32(const void *base, uint32_t offset)
{
    uint32_t value;

    memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    return value;
}

/* A physical address outside the direct map would fault, so treat it as absent
   instead of trusting a firmware pointer. */
static const void *map_table(uint64_t physical, uint32_t minimum_length)
{
    const sdt_header_t *header;

    if (physical == 0 || physical >= DIRECT_MAP_LIMIT) {
        return 0;
    }
    /* Compare against the remaining window instead of adding to the address: an
       addition near UINT64_MAX would wrap and pass the check. */
    if (physical > DIRECT_MAP_LIMIT - minimum_length) {
        return 0;
    }

    header = (const sdt_header_t *)PHYS_TO_VIRT(physical);
    if (header->length < minimum_length || header->length > MAX_TABLE_LENGTH) {
        return 0;
    }
    /* The caller checksums and walks the whole declared length, so it has to fit
       inside the direct map as well, not just the header. */
    if (header->length > DIRECT_MAP_LIMIT - physical) {
        return 0;
    }
    return header;
}

static const rsdp_t *validate_rsdp(uint64_t physical)
{
    const rsdp_t *candidate;

    if (physical == 0 || physical >= DIRECT_MAP_LIMIT
        || sizeof(rsdp_t) > DIRECT_MAP_LIMIT - physical) {
        return 0;
    }

    candidate = (const rsdp_t *)PHYS_TO_VIRT(physical);
    if (!bytes_equal(candidate->signature, "RSD PTR ", 8)) {
        return 0;
    }
    if (sum_bytes(candidate, RSDP_V1_LENGTH) != 0) {
        return 0;
    }
    if (candidate->revision >= 2) {
        /* The extended checksum covers the whole structure, and a broken one
           means the v1 fields are not trustworthy either. It also has to stay
           inside the direct map: a firmware pointer near the 4 GiB edge would
           otherwise walk off the mapping. */
        if (candidate->length < sizeof(rsdp_t) || candidate->length > MAX_TABLE_LENGTH) {
            return 0;
        }
        if (candidate->length > DIRECT_MAP_LIMIT - physical) {
            return 0;
        }
        if (sum_bytes(candidate, candidate->length) != 0) {
            return 0;
        }
    }
    return candidate;
}

static const rsdp_t *scan_for_rsdp(uint64_t start, uint64_t end)
{
    uint64_t address;

    for (address = start; address + sizeof(rsdp_t) <= end; address += RSDP_SEARCH_STEP) {
        const rsdp_t *candidate = validate_rsdp(address);

        if (candidate != 0) {
            return candidate;
        }
    }
    return 0;
}

static void record_table(uint64_t physical)
{
    const sdt_header_t *header;
    uint32_t index;

    if (table_count >= ACPI_MAX_TABLES) {
        return;
    }

    header = map_table(physical, SDT_HEADER_LENGTH);
    if (header == 0) {
        return;
    }

    if (sum_bytes(header, header->length) != 0) {
        console_write("acpi: table at ");
        console_hex64(physical);
        console_write(" fails its checksum, skipped\n");
        return;
    }

    for (index = 0; index < 4; index++) {
        tables[table_count].signature[index] = (char)header->signature[index];
    }
    tables[table_count].signature[4] = '\0';
    tables[table_count].physical_address = physical;
    tables[table_count].virtual_address = header;
    tables[table_count].length = header->length;
    tables[table_count].revision = header->revision;
    table_count++;
}

static void walk_root_table(uint64_t physical, uint32_t entry_size)
{
    const sdt_header_t *root;
    uint32_t entries;
    uint32_t index;

    root = map_table(physical, SDT_HEADER_LENGTH);
    if (root == 0) {
        return;
    }

    entries = (root->length - SDT_HEADER_LENGTH) / entry_size;
    for (index = 0; index < entries; index++) {
        uint32_t offset = SDT_HEADER_LENGTH + index * entry_size;

        if (entry_size == 8) {
            record_table(read_u64(root, offset));
        } else {
            record_table((uint64_t)read_u32(root, offset));
        }
    }
}

/* MADT: a local APIC address and a flags word, then a chain of variable-length
   entries: type, length, payload. Walk it by length rather than by a fixed
   size, and start after the eight header bytes, not at them. */
static void parse_madt(const acpi_table_t *table)
{
    const uint8_t *data = table->virtual_address;
    uint32_t offset = SDT_HEADER_LENGTH + 8;

    /* The local APIC address and its flags word are part of the table body, so a
       table shorter than that body is not a MADT this kernel can use. */
    if (table->length < offset) {
        return;
    }

    local_apic = read_u32(data, SDT_HEADER_LENGTH);

    while (offset + 2 <= table->length) {
        const uint8_t *entry = data + offset;
        uint8_t length = entry[1];

        if (length < 2 || offset + length > table->length) {
            break;
        }

        if (entry[0] == 1) {
            ioapic_count++;
        } else if (entry[0] == 2) {
            override_count++;
        }
        offset += length;
    }
}

/* MCFG: eight reserved bytes, then 16-byte ECAM entries. The kernel only keeps
   the first window; a multi-segment platform is out of scope for this phase. */
static void parse_mcfg(const acpi_table_t *table)
{
    const uint8_t *data = table->virtual_address;
    uint32_t offset = SDT_HEADER_LENGTH + 8;

    if (offset + 16 > table->length) {
        return;
    }

    ecam_base = read_u64(data, offset);
    ecam_first_bus = data[offset + 10];
    ecam_last_bus = data[offset + 11];
    ecam_valid = ecam_base != 0;
}

const acpi_table_t *acpi_find(const char *signature)
{
    uint32_t index;

    for (index = 0; index < table_count; index++) {
        if (bytes_equal(tables[index].signature, signature, 4)) {
            return &tables[index];
        }
    }
    return 0;
}

int acpi_init(void)
{
    const rsdp_t *rsdp = 0;
    uint64_t ebda = 0;
    uint32_t index;

    ready = 0;
    table_count = 0;
    ecam_valid = 0;
    local_apic = 0;
    ioapic_count = 0;
    override_count = 0;

    /* A UEFI firmware does not keep an RSDP in the legacy segment, so the loader
       hands the address over. A BIOS firmware leaves it zero and the scan below
       finds it. Either way the candidate is revalidated here, because the boot
       info block is data, not a trusted pointer. */
    if (bootinfo_valid()) {
        const boot_info_t *info = bootinfo_get();

        if (info->rsdp != 0) {
            rsdp = validate_rsdp(info->rsdp);
            if (rsdp != 0) {
                rsdp_source = "bootinfo";
            }
        }
    }

    if (rsdp == 0) {
        ebda = (uint64_t)(*(const volatile uint16_t *)PHYS_TO_VIRT(BDA_EBDA_VECTOR)) << 4;
        if (ebda >= EBDA_MIN_ADDRESS && ebda < EBDA_MAX_ADDRESS) {
            rsdp = scan_for_rsdp(ebda, ebda + EBDA_SCAN_BYTES);
        }
        if (rsdp != 0) {
            rsdp_source = "ebda";
        }
    }
    if (rsdp == 0) {
        rsdp = scan_for_rsdp(BIOS_ROM_START, BIOS_ROM_END);
        if (rsdp != 0) {
            rsdp_source = "bios rom";
        }
    }
    if (rsdp == 0) {
        return -1;
    }

    rsdp_physical = VIRT_TO_PHYS(rsdp);
    rsdp_revision = rsdp->revision;
    memcpy(rsdp_oem, rsdp->oem_id, sizeof(rsdp_oem));

    /* Prefer the 64-bit XSDT when the firmware advertises one; RSDT is the
       ACPI 1.0 fallback and only holds 32-bit addresses. */
    if (rsdp->revision >= 2 && rsdp->xsdt_address != 0) {
        root_table_name = "xsdt";
        walk_root_table(rsdp->xsdt_address, 8);
    } else if (rsdp->rsdt_address != 0) {
        root_table_name = "rsdt";
        walk_root_table(rsdp->rsdt_address, 4);
    }

    /* A root pointer that leads nowhere is not a usable ACPI description, and
       the caller has to see that instead of an empty but successful result. */
    if (table_count == 0) {
        return -1;
    }

    for (index = 0; index < table_count; index++) {
        if (bytes_equal(tables[index].signature, "APIC", 4)) {
            parse_madt(&tables[index]);
        } else if (bytes_equal(tables[index].signature, "MCFG", 4)) {
            parse_mcfg(&tables[index]);
        }
    }

    ready = 1;
    return 0;
}

int acpi_ready(void)
{
    return ready;
}

uint32_t acpi_table_count(void)
{
    return table_count;
}

const acpi_table_t *acpi_table_at(uint32_t index)
{
    if (index >= table_count) {
        return 0;
    }
    return &tables[index];
}

uint64_t acpi_local_apic(void)
{
    return local_apic;
}

uint32_t acpi_ioapic_count(void)
{
    return ioapic_count;
}

uint32_t acpi_interrupt_override_count(void)
{
    return override_count;
}

int acpi_ecam(uint64_t *base, uint8_t *first_bus, uint8_t *last_bus)
{
    if (!ecam_valid) {
        return -1;
    }
    if (base != 0) {
        *base = ecam_base;
    }
    if (first_bus != 0) {
        *first_bus = ecam_first_bus;
    }
    if (last_bus != 0) {
        *last_bus = ecam_last_bus;
    }
    return 0;
}

void acpi_print(void)
{
    uint32_t index;

    console_write("acpi: rsdp at ");
    console_hex64(rsdp_physical);
    console_write(" revision ");
    console_dec(rsdp_revision);
    console_write(" oem \"");
    for (index = 0; index < sizeof(rsdp_oem); index++) {
        console_putc((char)rsdp_oem[index]);
    }
    console_write("\" via ");
    console_write(rsdp_source);
    console_write("\n");

    console_write("acpi: ");
    console_dec(table_count);
    console_write(" tables via ");
    console_write(root_table_name);
    for (index = 0; index < table_count; index++) {
        console_write(" ");
        console_write(tables[index].signature);
    }
    console_write("\n");

    if (local_apic != 0) {
        console_write("acpi: madt local apic ");
        console_hex64(local_apic);
        console_write(" ioapics ");
        console_dec(ioapic_count);
        console_write(" overrides ");
        console_dec(override_count);
        console_write("\n");
    } else {
        console_write("acpi: no madt, local apic unknown\n");
    }

    if (ecam_valid) {
        console_write("acpi: mcfg ecam ");
        console_hex64(ecam_base);
        console_write(" buses ");
        console_hex_width(ecam_first_bus, 2);
        console_write("-");
        console_hex_width(ecam_last_bus, 2);
        console_write("\n");
    } else {
        console_write("acpi: no mcfg, no pcie ecam window\n");
    }
}
