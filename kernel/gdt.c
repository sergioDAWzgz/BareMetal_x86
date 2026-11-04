#include "gdt.h"

/* Null, kernel code, kernel data, user code, user data, then the two slots a
   64-bit TSS descriptor occupies. */
#define GDT_ENTRIES 7

typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} gdt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
    uint32_t base_upper;
    uint32_t reserved;
} tss_descriptor_t;

typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} tss_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} gdt_pointer_t;

static gdt_entry_t gdt[GDT_ENTRIES];
static tss_t tss;

static void gdt_set(gdt_entry_t *entry, uint8_t access, uint8_t granularity)
{
    entry->limit_low = 0xFFFF;
    entry->base_low = 0x0000;
    entry->base_mid = 0x00;
    entry->base_high = 0x00;
    entry->access = access;
    entry->granularity = granularity;
}

void gdt_init(void)
{
    gdt_pointer_t pointer;
    uint16_t selector;
    uint64_t tss_base = (uint64_t)(uintptr_t)&tss;
    uint32_t tss_limit = (uint32_t)(sizeof(tss) - 1);
    tss_descriptor_t *tss_entry = (tss_descriptor_t *)&gdt[5];
    unsigned int i;

    for (i = 0; i < sizeof(gdt); i++) {
        ((uint8_t *)gdt)[i] = 0;
    }
    for (i = 0; i < sizeof(tss); i++) {
        ((uint8_t *)&tss)[i] = 0;
    }

    gdt_set(&gdt[1], 0x9A, 0xAF);   /* 64-bit code, ring 0 */
    gdt_set(&gdt[2], 0x92, 0xCF);   /* flat data, ring 0 */
    /* DPL 3 is the whole point of these two: without it the iretq into ring 3
       would fault before it executed a single user instruction. */
    gdt_set(&gdt[3], 0xFA, 0xAF);   /* 64-bit code, ring 3 */
    gdt_set(&gdt[4], 0xF2, 0xCF);   /* flat data, ring 3 */

    tss_entry->limit_low = (uint16_t)(tss_limit & 0xFFFF);
    tss_entry->base_low = (uint16_t)(tss_base & 0xFFFF);
    tss_entry->base_mid = (uint8_t)((tss_base >> 16) & 0xFF);
    tss_entry->access = 0x89;       /* present, 64-bit available TSS */
    tss_entry->granularity = (uint8_t)((tss_limit >> 16) & 0x0F);
    tss_entry->base_high = (uint8_t)((tss_base >> 24) & 0xFF);
    tss_entry->base_upper = (uint32_t)(tss_base >> 32);
    tss_entry->reserved = 0;

    tss.iomap_base = (uint16_t)sizeof(tss);

    pointer.limit = (uint16_t)(sizeof(gdt) - 1);
    pointer.base = (uint64_t)(uintptr_t)&gdt[0];

    __asm__ volatile("lgdt %0" : : "m"(pointer));

    selector = GDT_SELECTOR_DATA;
    __asm__ volatile("mov %0, %%ds\n\t"
                     "mov %0, %%es\n\t"
                     "mov %0, %%fs\n\t"
                     "mov %0, %%gs\n\t"
                     "mov %0, %%ss" : : "r"(selector));

    selector = GDT_SELECTOR_TSS;
    __asm__ volatile("ltr %0" : : "r"(selector));
}

void gdt_set_kernel_stack(uint64_t top)
{
    tss.rsp0 = top;
}
