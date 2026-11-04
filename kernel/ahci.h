#ifndef BMX_AHCI_H
#define BMX_AHCI_H

#include <stdint.h>

/* Every transfer the driver issues moves whole 512-byte sectors. */
#define AHCI_SECTOR_SIZE 512

/* Finds every SATA controller in AHCI mode, enables bus mastering, initialises
   the command engine on every implemented port that carries an ATA drive and
   records those ports. Returns 0 when at least one usable port was found. */
int ahci_init(void);

/* Number of usable ports across every controller, and the port number of one of
   them. Both are indices into the discovered list, which is why the driver
   exposes a port number for the log and an index for I/O: two controllers can
   both name a port 0. */
uint32_t ahci_port_count(void);
uint32_t ahci_port_number(uint32_t index);

/* Reads count sectors starting at lba from the indexed port into buffer. The
   buffer must hold count * AHCI_SECTOR_SIZE bytes. Returns 0 on success, -1 on
   any failure; the buffer is left untouched when a transfer fails. */
int ahci_read(uint32_t index, uint64_t lba, uint32_t count, void *buffer);

void ahci_print(void);

/* Human-readable reason for the most recent failure. */
const char *ahci_error(void);

#endif
