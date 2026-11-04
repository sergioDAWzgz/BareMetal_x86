#include "pci.h"
#include "console.h"
#include "io.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC
#define PCI_MAX_DEVICES 64

static pci_device_t found[PCI_MAX_DEVICES];
static uint32_t found_count;

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset)
{
    uint32_t address = 0x80000000u |
                       ((uint32_t)bus << 16) |
                       ((uint32_t)slot << 11) |
                       ((uint32_t)function << 8) |
                       ((uint32_t)offset & 0xFCu);

    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset)
{
    uint32_t value = pci_config_read32(bus, slot, function, (uint8_t)(offset & 0xFCu));

    return (uint16_t)((value >> ((offset & 2u) * 8u)) & 0xFFFFu);
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset,
                        uint32_t value)
{
    uint32_t address = 0x80000000u |
                       ((uint32_t)bus << 16) |
                       ((uint32_t)slot << 11) |
                       ((uint32_t)function << 8) |
                       ((uint32_t)offset & 0xFCu);

    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

static void print_hex16(uint16_t value)
{
    console_hex_width(value, 4);
}

uint32_t pci_enumerate(void)
{
    uint32_t bus;
    uint32_t slot;

    found_count = 0;

    for (bus = 0; bus < 256; bus++) {
        for (slot = 0; slot < 32; slot++) {
            uint32_t identity = pci_config_read32((uint8_t)bus, (uint8_t)slot, 0, 0x00);
            uint32_t header;
            uint32_t functions;
            uint32_t function;

            if ((identity & 0xFFFFu) == 0xFFFFu) {
                continue;
            }

            header = pci_config_read32((uint8_t)bus, (uint8_t)slot, 0, 0x0C);
            functions = ((header >> 16) & 0x80u) != 0 ? 8u : 1u;

            for (function = 0; function < functions; function++) {
                uint32_t id = pci_config_read32((uint8_t)bus, (uint8_t)slot,
                                                (uint8_t)function, 0x00);
                uint32_t class_reg;
                pci_device_t *entry;

                if ((id & 0xFFFFu) == 0xFFFFu) {
                    continue;
                }
                if (found_count >= PCI_MAX_DEVICES) {
                    return found_count;
                }

                class_reg = pci_config_read32((uint8_t)bus, (uint8_t)slot,
                                              (uint8_t)function, 0x08);

                entry = &found[found_count++];
                entry->bus = (uint8_t)bus;
                entry->slot = (uint8_t)slot;
                entry->function = (uint8_t)function;
                entry->vendor = (uint16_t)(id & 0xFFFFu);
                entry->device = (uint16_t)(id >> 16);
                entry->class_code = (uint8_t)(class_reg >> 24);
                entry->subclass = (uint8_t)(class_reg >> 16);
            }
        }
    }

    return found_count;
}

uint32_t pci_count(void)
{
    return found_count;
}

const pci_device_t *pci_devices(void)
{
    return found;
}

void pci_print(void)
{
    uint32_t index;
    uint32_t intel = 0;

    console_write("pci: ");
    console_dec(found_count);
    console_write(" devices enumerated\n");

    for (index = 0; index < found_count; index++) {
        const pci_device_t *entry = &found[index];

        console_write("pci: ");
        console_hex_width(entry->bus, 2);
        console_write(":");
        console_hex_width(entry->slot, 2);
        console_write(".");
        console_dec(entry->function);
        console_write(" vendor=");
        print_hex16(entry->vendor);
        console_write(" device=");
        print_hex16(entry->device);
        console_write(" class=");
        console_hex_width(entry->class_code, 2);
        console_write(" subclass=");
        console_hex_width(entry->subclass, 2);
        console_write("\n");

        if (entry->vendor == 0x8086) {
            intel++;
        }
    }

    console_write("pci: intel devices ");
    console_dec(intel);
    console_write("\n");
}
