#include "memory.h"
#include "console.h"

#define E820_COUNT_ADDR 0x0504
#define E820_BUF_ADDR 0x0508
#define E820_MAX_ENTRIES 64
#define E820_TYPE_USABLE 1

#define FRAME_SIZE 4096ULL
#define PMM_LIMIT 0x40000000ULL                  /* first GiB, the mapped window */
#define PMM_FIRST_USABLE 0x100000ULL             /* keep the low MiB reserved */
#define PMM_FRAME_COUNT (PMM_LIMIT / FRAME_SIZE)
#define PMM_BITMAP_BYTES (PMM_FRAME_COUNT / 8)

static uint8_t frame_bitmap[PMM_BITMAP_BYTES];

uint32_t memmap_count(void)
{
    return *(volatile uint32_t *)PHYS_TO_VIRT(E820_COUNT_ADDR);
}

const e820_entry_t *memmap_entries(void)
{
    return (const e820_entry_t *)PHYS_TO_VIRT(E820_BUF_ADDR);
}

static const char *memmap_type_name(uint32_t type)
{
    switch (type) {
    case 1:
        return "usable";
    case 2:
        return "reserved";
    case 3:
        return "acpi-reclaimable";
    case 4:
        return "acpi-nvs";
    case 5:
        return "bad";
    default:
        return "unknown";
    }
}

void memmap_print(void)
{
    uint32_t count = memmap_count();
    const e820_entry_t *entries = memmap_entries();
    uint64_t usable_bytes = 0;
    uint32_t usable_regions = 0;
    uint32_t i;

    if (count > E820_MAX_ENTRIES) {
        count = E820_MAX_ENTRIES;
    }

    for (i = 0; i < count; i++) {
        console_write("mem: base=");
        console_hex64(entries[i].base);
        console_write(" len=");
        console_hex64(entries[i].length);
        console_write(" type=");
        console_dec(entries[i].type);
        console_write(" ");
        console_write(memmap_type_name(entries[i].type));
        console_write("\n");

        if (entries[i].type == E820_TYPE_USABLE) {
            usable_bytes += entries[i].length;
            usable_regions++;
        }
    }

    console_write("mem: usable ");
    console_dec(usable_bytes / (1024ULL * 1024ULL));
    console_write(" MiB across ");
    console_dec(usable_regions);
    console_write(" regions\n");
}

static void frame_mark_used(uint64_t frame)
{
    frame_bitmap[frame / 8] |= (uint8_t)(1u << (frame % 8));
}

static void frame_mark_free(uint64_t frame)
{
    frame_bitmap[frame / 8] &= (uint8_t)~(1u << (frame % 8));
}

static int frame_is_free(uint64_t frame)
{
    return (frame_bitmap[frame / 8] & (uint8_t)(1u << (frame % 8))) == 0;
}

void pmm_init(void)
{
    uint32_t count = memmap_count();
    const e820_entry_t *entries = memmap_entries();
    uint64_t bitmap_bytes;
    uint32_t i;

    if (count > E820_MAX_ENTRIES) {
        count = E820_MAX_ENTRIES;
    }

    for (bitmap_bytes = 0; bitmap_bytes < PMM_BITMAP_BYTES; bitmap_bytes++) {
        frame_bitmap[bitmap_bytes] = 0xFF;
    }

    for (i = 0; i < count; i++) {
        uint64_t start;
        uint64_t end;
        uint64_t frame;

        if (entries[i].type != E820_TYPE_USABLE) {
            continue;
        }

        start = entries[i].base;
        end = entries[i].base + entries[i].length;

        if (start < PMM_FIRST_USABLE) {
            start = PMM_FIRST_USABLE;
        }
        if (end > PMM_LIMIT) {
            end = PMM_LIMIT;
        }
        start = (start + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1);
        end = end & ~(FRAME_SIZE - 1);

        for (frame = start / FRAME_SIZE; frame < end / FRAME_SIZE; frame++) {
            frame_mark_free(frame);
        }
    }
}

uintptr_t pmm_alloc_frame(void)
{
    uint64_t frame;

    for (frame = 0; frame < PMM_FRAME_COUNT; frame++) {
        if (frame_is_free(frame)) {
            frame_mark_used(frame);
            return (uintptr_t)(frame * FRAME_SIZE);
        }
    }
    return 0;
}

uintptr_t pmm_alloc_frames(uint64_t count)
{
    uint64_t start = 0;
    uint64_t run = 0;
    uint64_t frame;
    uint64_t index;

    if (count == 0 || count > PMM_FRAME_COUNT) {
        return 0;
    }

    for (frame = 0; frame < PMM_FRAME_COUNT; frame++) {
        if (!frame_is_free(frame)) {
            run = 0;
            continue;
        }

        if (run == 0) {
            start = frame;
        }
        run++;

        if (run == count) {
            for (index = 0; index < count; index++) {
                frame_mark_used(start + index);
            }
            return (uintptr_t)(start * FRAME_SIZE);
        }
    }
    return 0;
}

void pmm_free_frame(uintptr_t frame)
{
    uint64_t index = (uint64_t)frame / FRAME_SIZE;

    if (index < PMM_FRAME_COUNT) {
        frame_mark_free(index);
    }
}

uint64_t pmm_free_frames(void)
{
    uint64_t free_frames = 0;
    uint64_t frame;

    for (frame = 0; frame < PMM_FRAME_COUNT; frame++) {
        if (frame_is_free(frame)) {
            free_frames++;
        }
    }
    return free_frames;
}
