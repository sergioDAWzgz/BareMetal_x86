#ifndef BMX_PCI_H
#define BMX_PCI_H

#include <stdint.h>

typedef struct {
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint16_t vendor;
    uint16_t device;
    uint8_t class_code;
    uint8_t subclass;
} pci_device_t;

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);

/* Walks every bus/slot/function pair and prints the devices it finds.
   Returns the number of functions discovered. */
uint32_t pci_enumerate(void);
uint32_t pci_count(void);
const pci_device_t *pci_devices(void);
void pci_print(void);

#endif
