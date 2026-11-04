#include "elf.h"

#include "lib.h"
#include "vmm.h"

#define ELF_CLASS_64 2
#define ELF_DATA_LSB 1
#define ELF_VERSION_CURRENT 1

#define ELF_TYPE_EXEC 2
#define ELF_MACHINE_X86_64 62
#define ELF_PROGRAM_LOAD 1

typedef struct __attribute__((packed)) {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t program_offset;
    uint64_t section_offset;
    uint32_t flags;
    uint16_t header_size;
    uint16_t program_size;
    uint16_t program_count;
    uint16_t section_size;
    uint16_t section_count;
    uint16_t section_names;
} elf64_header_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t address;
    uint64_t physical;
    uint64_t file_size;
    uint64_t memory_size;
    uint64_t alignment;
} elf64_program_t;

static const uint8_t elf_magic[4] = { 0x7F, 'E', 'L', 'F' };

static const char *failure = "none";

int elf_load(const void *image, uint32_t size, elf_image_t *out)
{
    const uint8_t *bytes = (const uint8_t *)image;
    const elf64_header_t *header = (const elf64_header_t *)image;
    uint64_t pages_before;
    uint16_t index;

    failure = "none";
    out->entry = 0;
    out->segments = 0;
    out->pages = 0;

    if (image == 0 || size < sizeof(elf64_header_t)) {
        failure = "the image is too small to hold an elf header";
        return -1;
    }
    if (memcmp(header->ident, elf_magic, sizeof(elf_magic)) != 0) {
        failure = "the image is not an elf file";
        return -1;
    }
    if (header->ident[4] != ELF_CLASS_64 || header->ident[5] != ELF_DATA_LSB ||
        header->ident[6] != ELF_VERSION_CURRENT) {
        failure = "the image is not a 64-bit little-endian elf";
        return -1;
    }
    if (header->type != ELF_TYPE_EXEC ||
        header->machine != ELF_MACHINE_X86_64) {
        failure = "the image is not an x86-64 executable";
        return -1;
    }
    if (header->program_size != sizeof(elf64_program_t) ||
        header->program_count == 0) {
        failure = "the program header table is not usable";
        return -1;
    }
    if (header->program_offset > size ||
        (uint64_t)header->program_count * sizeof(elf64_program_t) >
            size - header->program_offset) {
        failure = "the program header table lies outside the image";
        return -1;
    }
    if (header->entry < USER_BASE || header->entry >= USER_LIMIT) {
        failure = "the entry point is outside the user window";
        return -1;
    }

    pages_before = vmm_user_pages();

    for (index = 0; index < header->program_count; index++) {
        const elf64_program_t *program = (const elf64_program_t *)
            (bytes + header->program_offset +
             (uint64_t)index * sizeof(elf64_program_t));
        uint64_t end;

        if (program->type != ELF_PROGRAM_LOAD) {
            continue;
        }
        if (program->memory_size == 0) {
            continue;
        }
        if (program->file_size > program->memory_size) {
            failure = "a segment claims more file bytes than memory bytes";
            return -1;
        }
        if (program->offset > size ||
            program->file_size > size - program->offset) {
            failure = "a segment lies outside the image";
            return -1;
        }

        end = program->address + program->memory_size;
        if (program->address < USER_BASE || end > USER_LIMIT ||
            end < program->address) {
            failure = "a segment is outside the user window";
            return -1;
        }

        if (vmm_map_user(program->address, program->memory_size,
                         program->file_size != 0 ? bytes + program->offset : 0,
                         program->file_size) < 0) {
            failure = vmm_error();
            return -1;
        }

        /* The copy went in through the direct map, so this is the only thing
           that proves the user mapping resolves where the program expects. */
        if (program->file_size != 0 &&
            *(const volatile uint8_t *)(uintptr_t)program->address !=
                bytes[program->offset]) {
            failure = "the user mapping does not expose the segment";
            return -1;
        }
        out->segments++;
    }

    if (out->segments == 0) {
        failure = "the image has no loadable segment";
        return -1;
    }

    out->entry = header->entry;
    out->pages = vmm_user_pages() - pages_before;
    return 0;
}

const char *elf_error(void)
{
    return failure;
}
