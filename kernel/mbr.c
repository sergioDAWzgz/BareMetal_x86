#include "mbr.h"

#include "lib.h"

/* Layout of one partition entry. The CHS fields are deliberately ignored: the
   images this project writes address everything by LBA, and a CHS triple that
   disagrees with the LBA fields would only be a second source of truth. */
#define MBR_TABLE_OFFSET 0x1BE
#define MBR_ENTRY_SIZE 16u
#define MBR_ENTRY_TYPE 4u
#define MBR_ENTRY_START 8u
#define MBR_ENTRY_SECTORS 12u
#define MBR_SIGNATURE 0x1FEu

static const char *failure = "none";

static uint32_t read32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static void write32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value & 0xFFu);
    data[1] = (uint8_t)((value >> 8) & 0xFFu);
    data[2] = (uint8_t)((value >> 16) & 0xFFu);
    data[3] = (uint8_t)((value >> 24) & 0xFFu);
}

int mbr_partition(const uint8_t *sector, uint32_t index, mbr_partition_t *entry)
{
    const uint8_t *record;

    if (sector[MBR_SIGNATURE] != 0x55 || sector[MBR_SIGNATURE + 1] != 0xAA) {
        failure = "the sector carries no MBR signature";
        return -1;
    }
    if (index >= MBR_MAX_PARTITIONS) {
        failure = "the partition index is out of range";
        return -1;
    }

    record = sector + MBR_TABLE_OFFSET + index * MBR_ENTRY_SIZE;
    if (record[MBR_ENTRY_TYPE] == 0) {
        failure = "the partition slot is empty";
        return -1;
    }
    entry->type = record[MBR_ENTRY_TYPE];
    entry->start_lba = read32(record + MBR_ENTRY_START);
    entry->sectors = read32(record + MBR_ENTRY_SECTORS);
    if (entry->start_lba == 0 || entry->sectors == 0) {
        failure = "the partition entry has no extent";
        return -1;
    }
    return 0;
}

/* Builds the sector a well-formed single-partition table produces, then checks
   the decoder against it and against the three ways an MBR can be unusable:
   no signature, an empty slot and an out-of-range index. */
int mbr_self_test(void)
{
    uint8_t sector[MBR_SECTOR_SIZE];
    mbr_partition_t entry;
    uint8_t *record;

    memset(sector, 0, sizeof(sector));

    record = sector + MBR_TABLE_OFFSET + MBR_ENTRY_SIZE;
    record[MBR_ENTRY_TYPE] = 0x0C;
    write32(record + MBR_ENTRY_START, 2048);
    write32(record + MBR_ENTRY_SECTORS, 131072);
    sector[MBR_SIGNATURE] = 0x55;
    sector[MBR_SIGNATURE + 1] = 0xAA;

    if (mbr_partition(sector, 0, &entry) != -1) {
        failure = "an empty partition slot was reported as a partition";
        return -1;
    }
    if (mbr_partition(sector, 1, &entry) != 0) {
        return -1;
    }
    if (entry.type != 0x0C || entry.start_lba != 2048 || entry.sectors != 131072) {
        failure = "the partition entry decoded to the wrong values";
        return -1;
    }
    if (mbr_partition(sector, 3, &entry) != -1) {
        failure = "an empty partition slot was reported as a partition";
        return -1;
    }
    if (mbr_partition(sector, MBR_MAX_PARTITIONS, &entry) != -1) {
        failure = "an out-of-range index was accepted";
        return -1;
    }

    sector[MBR_SIGNATURE] = 0;
    if (mbr_partition(sector, 1, &entry) != -1) {
        failure = "a sector without the MBR signature was accepted";
        return -1;
    }

    failure = "none";
    return 0;
}

const char *mbr_error(void)
{
    return failure;
}
