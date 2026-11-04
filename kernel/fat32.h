#ifndef BMX_FAT32_H
#define BMX_FAT32_H

#include <stdint.h>

#define FAT32_SECTOR_SIZE 512

typedef struct {
    uint32_t port;                      /* index into the AHCI port list */
    uint32_t cluster;                   /* first cluster of the file */
    uint32_t size;                      /* size in bytes */
} fat32_file_t;

/* Mounts the first AHCI port that holds a consistent FAT32 volume labelled
   BMXDATA. The volume is mounted read-only: no function here writes a sector. */
int fat32_mount(void);

void fat32_print(void);

/* Looks name up in the root directory. The name is the 8.3 form, case
   insensitive, for example "HELLO.TXT"; only the root directory is searched. */
int fat32_open(const char *name, fat32_file_t *file);

/* Reads the whole file into buffer, which must hold the file size rounded up to
   a whole sector. Returns the number of bytes the file claims, -1 on failure. */
int32_t fat32_read(const fat32_file_t *file, void *buffer, uint32_t capacity);

/* Reason for the most recent failure. */
const char *fat32_error(void);

#endif
