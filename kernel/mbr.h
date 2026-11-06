#ifndef MBR_H
#define MBR_H

#include <stdint.h>

/* The master boot record of a partitioned disk: 446 bytes of boot code, four
   16-byte partition entries and the 0x55AA signature. This project writes the
   table itself instead of assuming a fixed volume offset, because the same disk
   now carries the boot code and the FAT32 volume. */

#define MBR_SECTOR_SIZE 512u
#define MBR_MAX_PARTITIONS 4u

typedef struct {
    uint8_t type;
    uint32_t start_lba;
    uint32_t sectors;
} mbr_partition_t;

/* Decodes entry `index` out of a 512-byte MBR sector into `entry`.

   Returns 0 when the slot holds a partition and -1 when it is empty, the index
   is out of range, or the sector carries no MBR signature. The type is not
   interpreted: the caller owns the question of what lives in the partition,
   and a FAT32 boot sector says that better than a type byte does. */
int mbr_partition(const uint8_t *sector, uint32_t index, mbr_partition_t *entry);

/* Replays recorded sectors through the decoder. Pure, so it runs before any
   disk I/O and names a broken table instead of letting it surface later as a
   missing volume. Returns 0 when every case matched. */
int mbr_self_test(void);

const char *mbr_error(void);

#endif
