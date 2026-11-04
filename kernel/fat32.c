#include "fat32.h"

#include "ahci.h"
#include "console.h"
#include "lib.h"

/* Boot sector field offsets. Only the fields this driver needs are named; the
   ones that describe the geometry it refuses are read straight from the image. */
#define BPB_BYTES_PER_SECTOR 0x0B
#define BPB_SECTORS_PER_CLUSTER 0x0D
#define BPB_RESERVED_SECTORS 0x0E
#define BPB_FAT_COUNT 0x10
#define BPB_ROOT_ENTRIES 0x11
#define BPB_SECTORS_PER_FAT_16 0x16
#define BPB_TOTAL_SECTORS 0x20
#define BPB_SECTORS_PER_FAT_32 0x24
#define BPB_ROOT_CLUSTER 0x2C
#define BPB_VOLUME_LABEL 0x47
#define BPB_FILESYSTEM 0x52
#define BPB_SIGNATURE 0x1FE

#define DIR_ENTRY_SIZE 32
#define DIR_ENTRY_NAME 0
#define DIR_ENTRY_ATTRIBUTES 11
#define DIR_ENTRY_CLUSTER_HIGH 20
#define DIR_ENTRY_CLUSTER_LOW 26
#define DIR_ENTRY_SIZE_BYTES 28
#define DIR_ENTRY_FREE 0xE5
#define DIR_ENTRY_END 0x00
#define ATTRIBUTES_LONG_NAME 0x0F
#define ATTRIBUTES_VOLUME_LABEL 0x08

#define FAT_ENTRY_MASK 0x0FFFFFFFu
#define FAT_ENTRY_END 0x0FFFFFF8u
#define FIRST_DATA_CLUSTER 2

#define VOLUME_LABEL "BMXDATA    "
#define FILESYSTEM_NAME "FAT32   "

typedef struct {
    uint32_t port;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t fat_sectors;
    uint32_t total_sectors;
    uint32_t root_cluster;
    uint32_t first_data_sector;
    uint32_t cluster_count;
    int mounted;
} fat32_volume_t;

static fat32_volume_t volume;
static uint8_t sector[FAT32_SECTOR_SIZE];
static const char *failure = "none";

static uint16_t read16(const uint8_t *data)
{
    return (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
}

static uint32_t read32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static int is_power_of_two(uint32_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

/* A candidate has to describe a usable FAT32 geometry. Anything that fails here
   is not a volume this driver understands, which matters because one of the
   disks on the bus is the raw boot image and one is an ESP. */
static int volume_ok(const uint8_t *boot)
{
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t fat_sectors;
    uint32_t total_sectors;
    uint32_t first_data_sector;
    uint32_t clusters;

    if (boot[BPB_SIGNATURE] != 0x55 || boot[BPB_SIGNATURE + 1] != 0xAA) {
        failure = "the volume has no boot signature";
        return 0;
    }

    bytes_per_sector = read16(boot + BPB_BYTES_PER_SECTOR);
    if (bytes_per_sector != FAT32_SECTOR_SIZE) {
        failure = "the volume does not use 512-byte sectors";
        return 0;
    }

    /* FAT32 is the only variant that leaves these two 16-bit fields empty and
       carries the real sector count in the 32-bit fields instead. */
    if (read16(boot + BPB_SECTORS_PER_FAT_16) != 0 ||
        read16(boot + BPB_ROOT_ENTRIES) != 0) {
        failure = "the volume is not FAT32";
        return 0;
    }
    if (memcmp(boot + BPB_FILESYSTEM, FILESYSTEM_NAME, 8) != 0) {
        failure = "the volume does not name itself FAT32";
        return 0;
    }

    sectors_per_cluster = boot[BPB_SECTORS_PER_CLUSTER];
    if (!is_power_of_two(sectors_per_cluster) || sectors_per_cluster > 128) {
        failure = "the cluster size is not a power of two";
        return 0;
    }

    fat_sectors = read32(boot + BPB_SECTORS_PER_FAT_32);
    total_sectors = read32(boot + BPB_TOTAL_SECTORS);
    if (fat_sectors == 0 || total_sectors == 0) {
        failure = "the volume geometry is empty";
        return 0;
    }
    if (boot[BPB_FAT_COUNT] == 0) {
        failure = "the volume has no fat";
        return 0;
    }

    first_data_sector = read16(boot + BPB_RESERVED_SECTORS) +
                        boot[BPB_FAT_COUNT] * fat_sectors;
    if (first_data_sector >= total_sectors) {
        failure = "the volume has no data region";
        return 0;
    }
    clusters = (total_sectors - first_data_sector) / sectors_per_cluster;
    if (clusters == 0) {
        failure = "the volume has no clusters";
        return 0;
    }
    if (read32(boot + BPB_ROOT_CLUSTER) < FIRST_DATA_CLUSTER ||
        read32(boot + BPB_ROOT_CLUSTER) > clusters + 1) {
        failure = "the root cluster is out of range";
        return 0;
    }

    if (memcmp(boot + BPB_VOLUME_LABEL, VOLUME_LABEL, 11) != 0) {
        failure = "the volume does not carry the BMXDATA label";
        return 0;
    }
    return 1;
}

static int mount_at(uint32_t port, const uint8_t *boot)
{
    volume.port = port;
    volume.sectors_per_cluster = boot[BPB_SECTORS_PER_CLUSTER];
    volume.reserved_sectors = read16(boot + BPB_RESERVED_SECTORS);
    volume.fat_sectors = read32(boot + BPB_SECTORS_PER_FAT_32);
    volume.total_sectors = read32(boot + BPB_TOTAL_SECTORS);
    volume.root_cluster = read32(boot + BPB_ROOT_CLUSTER);
    volume.first_data_sector = volume.reserved_sectors +
                               boot[BPB_FAT_COUNT] * volume.fat_sectors;
    volume.cluster_count = (volume.total_sectors - volume.first_data_sector) /
                           volume.sectors_per_cluster;
    volume.mounted = 1;
    return 0;
}

int fat32_mount(void)
{
    uint32_t index;

    volume.mounted = 0;
    failure = "no fat32 volume labelled BMXDATA";

    for (index = 0; index < ahci_port_count(); index++) {
        if (ahci_read(index, 0, 1, sector) != 0) {
            continue;
        }
        if (volume_ok(sector)) {
            return mount_at(index, sector);
        }
    }
    return -1;
}

static uint32_t cluster_lba(uint32_t cluster)
{
    return volume.first_data_sector +
           (cluster - FIRST_DATA_CLUSTER) * volume.sectors_per_cluster;
}

/* Returns the next cluster in the chain, or 0 when the chain ends or the entry
   is out of range. A corrupt chain must stop the walk instead of looping. */
static uint32_t next_cluster(uint32_t cluster)
{
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = volume.reserved_sectors + fat_offset / FAT32_SECTOR_SIZE;
    uint32_t entry;

    if (ahci_read(volume.port, fat_sector, 1, sector) != 0) {
        return 0;
    }

    entry = read32(sector + (fat_offset % FAT32_SECTOR_SIZE)) & FAT_ENTRY_MASK;
    if (entry >= FAT_ENTRY_END || entry < FIRST_DATA_CLUSTER ||
        entry > volume.cluster_count + 1) {
        return 0;
    }
    return entry;
}

/* Encodes the 8.3 form the directory stores: eight characters, three
   characters, both padded with spaces and folded to upper case. */
static int name_to_83(const char *name, uint8_t *out)
{
    uint32_t written = 0;
    uint32_t slot;
    uint32_t index = 0;
    char character;

    /* Eight name bytes plus three extension bytes, space padded. */
    for (slot = 0; slot < 11; slot++) {
        out[slot] = ' ';
    }

    while ((character = name[index]) != '\0' && character != '.') {
        if (written >= 8) {
            return -1;
        }
        if (character >= 'a' && character <= 'z') {
            character = (char)(character - 'a' + 'A');
        }
        out[written++] = (uint8_t)character;
        index++;
    }
    if (written == 0 || name[index] != '.') {
        return -1;
    }

    written = 8;
    index++;
    while (name[index] != '\0') {
        char extension = name[index];

        if (written >= 11) {
            return -1;
        }
        if (extension >= 'a' && extension <= 'z') {
            extension = (char)(extension - 'a' + 'A');
        }
        out[written++] = (uint8_t)extension;
        index++;
    }
    return 0;
}

int fat32_open(const char *name, fat32_file_t *file)
{
    uint8_t wanted[11];
    uint32_t cluster;
    uint32_t visited = 0;

    if (!volume.mounted) {
        failure = "no volume is mounted";
        return -1;
    }
    if (name_to_83(name, wanted) != 0) {
        failure = "the name is not in 8.3 form";
        return -1;
    }

    cluster = volume.root_cluster;
    while (cluster != 0 && visited <= volume.cluster_count) {
        uint32_t sector_index;

        for (sector_index = 0; sector_index < volume.sectors_per_cluster;
             sector_index++) {
            uint32_t entry;

            if (ahci_read(volume.port, cluster_lba(cluster) + sector_index,
                          1, sector) != 0) {
                failure = "the directory could not be read";
                return -1;
            }

            for (entry = 0; entry < FAT32_SECTOR_SIZE / DIR_ENTRY_SIZE; entry++) {
                const uint8_t *record = sector + entry * DIR_ENTRY_SIZE;
                uint8_t attributes = record[DIR_ENTRY_ATTRIBUTES];

                if (record[DIR_ENTRY_NAME] == DIR_ENTRY_END) {
                    failure = "the file is not in the root directory";
                    return -1;
                }
                if (record[DIR_ENTRY_NAME] == DIR_ENTRY_FREE ||
                    attributes == ATTRIBUTES_LONG_NAME ||
                    (attributes & ATTRIBUTES_VOLUME_LABEL) != 0) {
                    continue;
                }
                if (memcmp(record + DIR_ENTRY_NAME, wanted, 11) != 0) {
                    continue;
                }

                file->port = volume.port;
                file->cluster = read16(record + DIR_ENTRY_CLUSTER_HIGH);
                file->cluster = (file->cluster << 16) |
                                read16(record + DIR_ENTRY_CLUSTER_LOW);
                file->size = read32(record + DIR_ENTRY_SIZE_BYTES);
                if (file->cluster < FIRST_DATA_CLUSTER ||
                    file->cluster > volume.cluster_count + 1) {
                    failure = "the file starts at an invalid cluster";
                    return -1;
                }
                if (file->size > (uint64_t)volume.cluster_count *
                                 volume.sectors_per_cluster * FAT32_SECTOR_SIZE) {
                    failure = "the file is larger than the volume";
                    return -1;
                }
                return 0;
            }
        }

        cluster = next_cluster(cluster);
        visited++;
    }

    failure = "the root directory chain is broken";
    return -1;
}

int32_t fat32_read(const fat32_file_t *file, void *buffer, uint32_t capacity)
{
    uint8_t *destination = (uint8_t *)buffer;
    uint32_t remaining = file->size;
    uint32_t cluster = file->cluster;
    uint32_t needed = (file->size + FAT32_SECTOR_SIZE - 1) & ~(FAT32_SECTOR_SIZE - 1);
    uint32_t visited = 0;

    if (capacity < needed) {
        failure = "the destination is smaller than the file";
        return -1;
    }

    while (remaining > 0) {
        uint32_t sector_index;

        if (cluster < FIRST_DATA_CLUSTER || cluster > volume.cluster_count + 1 ||
            visited > volume.cluster_count) {
            failure = "the file chain is broken";
            return -1;
        }

        for (sector_index = 0;
             sector_index < volume.sectors_per_cluster && remaining > 0;
             sector_index++) {
            uint32_t bytes = remaining < FAT32_SECTOR_SIZE ? remaining
                                                           : FAT32_SECTOR_SIZE;

            if (ahci_read(volume.port, cluster_lba(cluster) + sector_index,
                          1, destination) != 0) {
                failure = "a file sector could not be read";
                return -1;
            }
            destination += FAT32_SECTOR_SIZE;
            remaining -= bytes;
        }

        cluster = next_cluster(cluster);
        visited++;
    }
    return (int32_t)file->size;
}

void fat32_print(void)
{
    console_write("fat32: volume BMXDATA sectors ");
    console_dec(volume.total_sectors);
    console_write(" clusters ");
    console_dec(volume.cluster_count);
    console_write("\n");
}

const char *fat32_error(void)
{
    return failure;
}
