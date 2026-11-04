/*
 * BareMetal_x86 - UEFI loader
 *
 * Runs as BOOTX64.EFI from the EFI system partition. It:
 *   1. reports progress on COM1 at 115200 8N1,
 *   2. picks a 1024x768 GOP mode and publishes the boot info block at 0x0C00,
 *   3. loads KERNEL.BIN to physical 0x10000,
 *   4. synthesises an E820 style map from the UEFI memory map at 0x0504,
 *   5. leaves boot services, then builds the page tables and the GDT in fixed
 *      low memory. They must be built *after* ExitBootServices: the firmware
 *      hands the pages behind EfiLoaderData allocations out again while it
 *      tears boot services down, which silently overwrites anything built
 *      before that point,
 *   6. installs the GDT and jumps to the kernel virtual entry with the same
 *      memory layout the BIOS path produces.
 */

#include <efi.h>
#include <efilib.h>

/* The boot info block is a published contract between the two loaders and the
   kernel, so there is exactly one definition of it: the kernel header. */
#include "bootinfo.h"

#define COM1_PORT 0x3F8

#define E820_COUNT_ADDR 0x0504
#define E820_BUF_ADDR 0x0508
#define E820_MAX 64

#define PAGE_TABLE_ADDR 0x70000ULL
#define GDT_ADDR 0x78000ULL
#define BOOT_STACK_BASE 0x79000ULL
#define BOOT_STACK_TOP (BOOT_STACK_BASE + 4ULL * 4096ULL)

#define KERNEL_VIRT 0xFFFFFFFF80010000ULL
#define KERNEL_OFFSET 0x10000ULL
#define KERNEL_MAX_SIZE (64ULL * 1024ULL)
#define KERNEL_PAGES (KERNEL_MAX_SIZE / 4096ULL)

#define PAGE_TABLE_PAGES 8ULL

#define DIRECT_MAP_PML4_INDEX 256
#define KERNEL_PML4_INDEX 511
#define KERNEL_PDPT_INDEX 510

#define DIRECT_MAP_LIMIT 0x100000000ULL /* the loaders map the first 4 GiB */

/* Values the post-ExitBootServices phase needs. They cannot be ordinary locals:
   that phase runs on a stack of its own, so every RSP-relative slot of
   efi_main's frame is gone by then. */
static volatile UINT64 g_kernel_phys;
static volatile UINT64 g_rsdp;
/* Written and read after the stack switch, so they cannot be locals of
   efi_main: the frame they would live in is gone by then. */
static UINT16 g_gdt_limit;
static UINT64 g_kernel_byte;

/* ACPI 2.0 and 1.0 configuration table identifiers. */
static const EFI_GUID acpi_20_table_guid = {
    0x8868e871, 0xe4f1, 0x11d3,
    { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 }
};
static const EFI_GUID acpi_10_table_guid = {
    0xeb9d2d30, 0x2d88, 0x11d3,
    { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d }
};

static inline void outb(UINT16 port, UINT8 value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline UINT8 inb(UINT16 port)
{
    UINT8 value;

    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void serial_init(void)
{
    outb(COM1_PORT + 1, 0x00);
    outb(COM1_PORT + 3, 0x80);
    outb(COM1_PORT + 0, 0x01);
    outb(COM1_PORT + 1, 0x00);
    outb(COM1_PORT + 3, 0x03);
    outb(COM1_PORT + 2, 0xC7);
    outb(COM1_PORT + 4, 0x0B);
}

static void serial_putc(char c)
{
    while ((inb(COM1_PORT + 5) & 0x20) == 0) {
    }
    outb(COM1_PORT, (UINT8)c);
}

static void serial_puts(const char *text)
{
    while (*text != '\0') {
        serial_putc(*text);
        text++;
    }
}

static void serial_hex(UINT64 value, UINTN digits)
{
    static const char table[] = "0123456789abcdef";
    UINTN shift;

    for (shift = digits; shift > 0; shift--) {
        UINTN nibble = (UINTN)((value >> ((shift - 1) * 4)) & 0x0F);

        serial_putc(table[nibble]);
    }
}

static void serial_dec(UINT64 value)
{
    char buffer[21];
    UINTN index = 0;

    if (value == 0) {
        serial_putc('0');
        return;
    }
    while (value > 0 && index < sizeof(buffer)) {
        buffer[index++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (index > 0) {
        serial_putc(buffer[--index]);
    }
}

__attribute__((noreturn))
static void halt_forever(void)
{
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

/* ------------------------------------------------------------------------- */
/* Boot info block                                                           */
/* ------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    UINT64 base;
    UINT64 length;
    UINT32 type;
    UINT32 acpi;
} e820_entry_t;

static int guid_equal(const EFI_GUID *left, const EFI_GUID *right)
{
    const UINT8 *a = (const UINT8 *)left;
    const UINT8 *b = (const UINT8 *)right;
    UINTN index;

    for (index = 0; index < sizeof(EFI_GUID); index++) {
        if (a[index] != b[index]) {
            return 0;
        }
    }
    return 1;
}

/* A firmware does not leave an RSDP in the legacy 0xE0000 segment the way a
   BIOS does; it only publishes the root pointer through its configuration
   table, so the kernel cannot find ACPI by scanning memory on this path. */
static void find_acpi_rsdp(void)
{
    UINTN index;

    for (index = 0; index < ST->NumberOfTableEntries; index++) {
        EFI_CONFIGURATION_TABLE *entry = &ST->ConfigurationTable[index];

        if (guid_equal(&entry->VendorGuid, &acpi_20_table_guid)
            || guid_equal(&entry->VendorGuid, &acpi_10_table_guid)) {
            g_rsdp = (UINT64)(UINTN)entry->VendorTable;
            return;
        }
    }
}

static UINT8 mask_size(UINT32 mask)
{
    UINT8 size = 0;

    while (mask != 0) {
        size = (UINT8)(size + (mask & 1u));
        mask >>= 1;
    }
    return size;
}

static UINT8 mask_position(UINT32 mask)
{
    UINT8 position = 0;

    if (mask == 0) {
        return 0;
    }
    while ((mask & 1u) == 0) {
        position++;
        mask >>= 1;
    }
    return position;
}

static void bootinfo_publish(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info,
                             UINT64 framebuffer)
{
    boot_info_t *block = (boot_info_t *)(UINTN)BOOT_INFO_ADDR;

    block->magic = BOOT_INFO_MAGIC;
    block->source = BOOT_INFO_SOURCE_UEFI;
    block->framebuffer = framebuffer;
    block->width = info->HorizontalResolution;
    block->height = info->VerticalResolution;
    block->pitch = info->PixelsPerScanLine * 4u;
    block->bpp = 32;
    block->red_size = 8;
    block->green_size = 8;
    block->blue_size = 8;
    block->reserved = 0;
    block->rsdp = g_rsdp;

    switch (info->PixelFormat) {
    case PixelRedGreenBlueReserved8BitPerColor:
        block->red_pos = 16;
        block->green_pos = 8;
        block->blue_pos = 0;
        break;
    case PixelBlueGreenRedReserved8BitPerColor:
        block->red_pos = 0;
        block->green_pos = 8;
        block->blue_pos = 16;
        break;
    case PixelBitMask:
        block->red_size = mask_size(info->PixelInformation.RedMask);
        block->red_pos = mask_position(info->PixelInformation.RedMask);
        block->green_size = mask_size(info->PixelInformation.GreenMask);
        block->green_pos = mask_position(info->PixelInformation.GreenMask);
        block->blue_size = mask_size(info->PixelInformation.BlueMask);
        block->blue_pos = mask_position(info->PixelInformation.BlueMask);
        break;
    default:
        block->red_size = 0;
        block->green_size = 0;
        block->blue_size = 0;
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Graphics                                                                  */
/* ------------------------------------------------------------------------- */

static EFI_GRAPHICS_OUTPUT_PROTOCOL *find_gop(void)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_STATUS status;

    status = uefi_call_wrapper(BS->LocateProtocol, 3,
                               &GraphicsOutputProtocol, NULL, (VOID **)&gop);
    if (EFI_ERROR(status)) {
        return NULL;
    }
    return gop;
}

static void select_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop)
{
    UINT32 index;

    for (index = 0; index < gop->Mode->MaxMode; index++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        UINTN info_size = 0;
        EFI_STATUS status;

        status = uefi_call_wrapper(gop->QueryMode, 4, gop, index, &info_size, &info);
        if (EFI_ERROR(status) || info == NULL) {
            continue;
        }
        if (info->HorizontalResolution == 1024 && info->VerticalResolution == 768 &&
            info->PixelFormat != PixelBltOnly) {
            uefi_call_wrapper(gop->SetMode, 2, gop, index);
            uefi_call_wrapper(BS->FreePool, 1, info);
            return;
        }
        uefi_call_wrapper(BS->FreePool, 1, info);
    }
}

/* ------------------------------------------------------------------------- */
/* Kernel image                                                              */
/* ------------------------------------------------------------------------- */

static EFI_FILE_PROTOCOL *open_root(EFI_HANDLE image)
{
    EFI_LOADED_IMAGE *loaded = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    EFI_FILE_PROTOCOL *root = NULL;
    EFI_STATUS status;

    status = uefi_call_wrapper(BS->HandleProtocol, 3, image,
                               &LoadedImageProtocol, (VOID **)&loaded);
    if (EFI_ERROR(status)) {
        return NULL;
    }

    status = uefi_call_wrapper(BS->HandleProtocol, 3, loaded->DeviceHandle,
                               &FileSystemProtocol, (VOID **)&fs);
    if (EFI_ERROR(status)) {
        return NULL;
    }

    status = uefi_call_wrapper(fs->OpenVolume, 2, fs, &root);
    if (EFI_ERROR(status)) {
        return NULL;
    }
    return root;
}

static UINT64 allocate_kernel(void)
{
    static const UINT64 candidates[] = {
        0x10000ULL, 0x210000ULL, 0x410000ULL, 0x810000ULL, 0x1010000ULL
    };
    UINTN index;

    for (index = 0; index < sizeof(candidates) / sizeof(candidates[0]); index++) {
        EFI_PHYSICAL_ADDRESS address = (EFI_PHYSICAL_ADDRESS)candidates[index];
        EFI_STATUS status;

        status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAddress,
                                   EfiLoaderData, KERNEL_PAGES, &address);
        if (!EFI_ERROR(status)) {
            return (UINT64)address;
        }
    }
    return 0;
}

static UINT64 load_kernel(EFI_HANDLE image)
{
    EFI_FILE_PROTOCOL *root;
    EFI_FILE_PROTOCOL *file = NULL;
    EFI_STATUS status;
    UINT64 address;
    UINTN size = KERNEL_MAX_SIZE;

    root = open_root(image);
    if (root == NULL) {
        serial_puts("uefi: cannot open the EFI system partition\r\n");
        return 0;
    }

    status = uefi_call_wrapper(root->Open, 5, root, &file,
                               L"\\EFI\\BOOT\\KERNEL.BIN", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        serial_puts("uefi: cannot open \\EFI\\BOOT\\KERNEL.BIN\r\n");
        return 0;
    }

    address = allocate_kernel();
    if (address == 0) {
        serial_puts("uefi: no free low memory for the kernel\r\n");
        return 0;
    }

    status = uefi_call_wrapper(file->Read, 3, file, &size, (VOID *)(UINTN)address);
    uefi_call_wrapper(file->Close, 1, file);
    if (EFI_ERROR(status)) {
        serial_puts("uefi: kernel read failed\r\n");
        return 0;
    }

    serial_puts("uefi: kernel ");
    serial_dec((UINT64)size);
    serial_puts(" bytes at 0x");
    serial_hex(address, 8);
    serial_puts("\r\n");

    return address;
}

/* ------------------------------------------------------------------------- */
/* Memory map                                                                */
/* ------------------------------------------------------------------------- */

static void e820_emit(UINT32 index, UINT64 base, UINT64 length, UINT32 type)
{
    e820_entry_t *entries = (e820_entry_t *)(UINTN)E820_BUF_ADDR;

    entries[index].base = base;
    entries[index].length = length;
    entries[index].type = type;
    entries[index].acpi = 0;
}

static UINT32 e820_publish(EFI_MEMORY_DESCRIPTOR *map, UINTN map_size, UINTN desc_size)
{
    UINTN offset;
    UINT32 count = 0;
    UINT64 pending_base = 0;
    UINT64 pending_length = 0;
    UINT32 pending_type = 0;
    int pending = 0;

    for (offset = 0; offset + desc_size <= map_size; offset += desc_size) {
        EFI_MEMORY_DESCRIPTOR *descriptor =
            (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map + offset);
        UINT64 base = (UINT64)descriptor->PhysicalStart;
        UINT64 length = (UINT64)descriptor->NumberOfPages * 4096ULL;
        UINT32 type = descriptor->Type == EfiConventionalMemory ? 1u : 2u;

        if (length == 0 || base >= DIRECT_MAP_LIMIT) {
            continue;
        }
        if (base + length > DIRECT_MAP_LIMIT) {
            length = DIRECT_MAP_LIMIT - base;
        }

        if (pending && pending_type == type && pending_base + pending_length == base) {
            pending_length += length;
            continue;
        }
        if (pending && count < E820_MAX) {
            e820_emit(count, pending_base, pending_length, pending_type);
            count++;
        }
        pending = 1;
        pending_base = base;
        pending_length = length;
        pending_type = type;
    }

    if (pending && count < E820_MAX) {
        e820_emit(count, pending_base, pending_length, pending_type);
        count++;
    }

    *(volatile UINT32 *)(UINTN)E820_COUNT_ADDR = count;
    return count;
}

/* ------------------------------------------------------------------------- */
/* Long mode hand-off                                                        */
/* ------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    UINT16 limit_low;
    UINT16 base_low;
    UINT8 base_mid;
    UINT8 access;
    UINT8 granularity;
    UINT8 base_high;
} gdt_entry_t;

typedef struct __attribute__((packed)) {
    UINT16 limit;
    UINT64 base;
} gdt_pointer_t;

static void gdt_set(gdt_entry_t *entry, UINT8 access, UINT8 granularity)
{
    entry->limit_low = 0xFFFF;
    entry->base_low = 0;
    entry->base_mid = 0;
    entry->base_high = 0;
    entry->access = access;
    entry->granularity = granularity;
}

static void zero_pages(UINT64 address, UINTN pages)
{
    volatile UINT64 *slots = (volatile UINT64 *)(UINTN)address;
    UINTN count = pages * 512;
    UINTN index;

    for (index = 0; index < count; index++) {
        slots[index] = 0;
    }
}

/* Identity map of the first 4 GiB, the same window as a direct map, and the
   kernel at 0xFFFFFFFF80000000 + (kernel_phys - 0x10000). */
static void build_page_tables(UINT64 tables, UINT64 kernel_phys)
{
    UINT64 pml4 = tables;
    UINT64 pdpt_direct = tables + 0x1000;
    UINT64 pdpt_kernel = tables + 0x2000;
    UINT64 pd0 = tables + 0x3000;
    UINT64 pd_kernel = tables + 0x7000;
    UINT64 *entries;
    UINTN index;

    zero_pages(tables, PAGE_TABLE_PAGES);

    entries = (UINT64 *)(UINTN)pml4;
    entries[0] = pdpt_direct | 3;                          /* identity */
    entries[DIRECT_MAP_PML4_INDEX] = pdpt_direct | 3;      /* direct map */
    entries[KERNEL_PML4_INDEX] = pdpt_kernel | 3;          /* kernel window */

    entries = (UINT64 *)(UINTN)pdpt_direct;
    for (index = 0; index < (DIRECT_MAP_LIMIT >> 30); index++) {
        entries[index] = (pd0 + index * 0x1000ULL) | 3;
    }

    entries = (UINT64 *)(UINTN)pdpt_kernel;
    entries[KERNEL_PDPT_INDEX] = pd_kernel | 3;

    for (index = 0; index < 2048; index++) {
        ((UINT64 *)(UINTN)pd0)[index] = ((UINT64)index * 0x200000ULL) | 0x83;
    }

    entries = (UINT64 *)(UINTN)pd_kernel;
    entries[0] = (kernel_phys - KERNEL_OFFSET) | 0x83;
}

static UINT16 build_gdt(UINT64 address)
{
    gdt_entry_t *gdt = (gdt_entry_t *)(UINTN)address;

    gdt[0].limit_low = 0;
    gdt[0].base_low = 0;
    gdt[0].base_mid = 0;
    gdt[0].access = 0;
    gdt[0].granularity = 0;
    gdt[0].base_high = 0;
    gdt_set(&gdt[1], 0x9A, 0xAF); /* 64-bit code, ring 0 */
    gdt_set(&gdt[2], 0x92, 0xCF); /* flat data, ring 0 */

    return (UINT16)(sizeof(gdt_entry_t) * 3 - 1);
}

__attribute__((noinline, noreturn))
static void enter_kernel(UINT64 gdt_base, UINT16 gdt_limit, UINT64 entry)
{
    gdt_pointer_t pointer;

    pointer.limit = gdt_limit;
    pointer.base = gdt_base;

    __asm__ volatile(
        "lgdt %0\n\t"
        "pushq $0x08\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "movw $0x10, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "jmp *%1\n\t"
        :
        : "m"(pointer), "r"(entry)
        : "rax", "memory");

    halt_forever();
}

/* ------------------------------------------------------------------------- */
/* Entry point                                                               */
/* ------------------------------------------------------------------------- */

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mode_info;
    EFI_MEMORY_DESCRIPTOR *memory_map = NULL;
    UINTN memory_map_size = 0;
    UINTN map_key = 0;
    UINTN descriptor_size = 0;
    UINT32 descriptor_version = 0;
    EFI_STATUS status;
    UINT64 kernel_phys;
    UINT32 e820_entries;

    InitializeLib(image, systab);

    serial_init();
    serial_puts("\r\nuefi: loader started\r\n");

    gop = find_gop();
    if (gop == NULL) {
        serial_puts("uefi: no graphics output protocol\r\n");
        return EFI_UNSUPPORTED;
    }

    select_mode(gop);
    mode_info = gop->Mode->Info;

    if (mode_info->PixelFormat == PixelBltOnly) {
        serial_puts("uefi: framebuffer is not CPU addressable\r\n");
        return EFI_UNSUPPORTED;
    }

    find_acpi_rsdp();
    bootinfo_publish(mode_info, (UINT64)gop->Mode->FrameBufferBase);
    serial_puts("uefi: rsdp 0x");
    serial_hex(g_rsdp, 16);
    serial_puts("\r\n");
    serial_puts("uefi: gop ");
    serial_dec(mode_info->HorizontalResolution);
    serial_puts("x");
    serial_dec(mode_info->VerticalResolution);
    serial_puts(" pitch=");
    serial_dec((UINT64)mode_info->PixelsPerScanLine * 4ULL);
    serial_puts(" fb=0x");
    serial_hex((UINT64)gop->Mode->FrameBufferBase, 8);
    serial_puts("\r\n");

    if ((UINT64)gop->Mode->FrameBufferBase + (UINT64)gop->Mode->FrameBufferSize >
        DIRECT_MAP_LIMIT) {
        serial_puts("uefi: framebuffer above the 4 GiB direct map\r\n");
        return EFI_UNSUPPORTED;
    }

    kernel_phys = load_kernel(image);
    if (kernel_phys == 0) {
        halt_forever();
    }
    g_kernel_phys = kernel_phys;

    memory_map_size = 0;
    uefi_call_wrapper(BS->GetMemoryMap, 5, &memory_map_size, memory_map,
                      &map_key, &descriptor_size, &descriptor_version);
    memory_map_size += 4 * descriptor_size + 4096;
    status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData,
                               memory_map_size, (VOID **)&memory_map);
    if (EFI_ERROR(status)) {
        serial_puts("uefi: cannot allocate the memory map buffer\r\n");
        halt_forever();
    }
    status = uefi_call_wrapper(BS->GetMemoryMap, 5, &memory_map_size, memory_map,
                               &map_key, &descriptor_size, &descriptor_version);
    if (EFI_ERROR(status)) {
        serial_puts("uefi: cannot read the memory map\r\n");
        halt_forever();
    }

    e820_entries = e820_publish(memory_map, memory_map_size, descriptor_size);
    serial_puts("uefi: memory map ");
    serial_dec((UINT64)e820_entries);
    serial_puts(" entries published\r\n");

    serial_puts("uefi: exiting boot services\r\n");

    status = uefi_call_wrapper(BS->ExitBootServices, 2, image, map_key);
    if (EFI_ERROR(status)) {
        memory_map_size = 0;
        uefi_call_wrapper(BS->GetMemoryMap, 5, &memory_map_size, memory_map,
                          &map_key, &descriptor_size, &descriptor_version);
        status = uefi_call_wrapper(BS->ExitBootServices, 2, image, map_key);
        if (EFI_ERROR(status)) {
            serial_puts("uefi: exit boot services failed\r\n");
            halt_forever();
        }
    }

    /* From here the firmware's memory authority is gone and it is free to hand
       out the pages it gave us earlier, so take a stack of our own and build
       everything the kernel needs in fixed low memory, using only globals and
       constants: the old frame is unreachable from the new stack. */
    __asm__ volatile("cli; movq %0, %%rsp; xorq %%rbp, %%rbp"
                     :
                     : "r"(BOOT_STACK_TOP)
                     : "rbp", "memory");

    build_page_tables(PAGE_TABLE_ADDR, g_kernel_phys);
    g_gdt_limit = build_gdt(GDT_ADDR);

    /* CR3: the tables above are built, and this code, the GDT and the stack all
       live in low memory that the new tables map identity, so the very next
       instruction fetch still resolves after the switch. */
    __asm__ volatile("movq %0, %%cr3" : : "r"(PAGE_TABLE_ADDR) : "memory");
    serial_puts("uefi: cr3 switched\r\n");

    __asm__ volatile("movzbq (%1), %0" : "=r"(g_kernel_byte) : "r"(KERNEL_VIRT) : "memory");
    serial_puts("uefi: kernel byte 0x");
    serial_hex(g_kernel_byte, 2);
    serial_puts("\r\n");

    serial_puts("uefi: jumping to the kernel\r\n");

    enter_kernel(GDT_ADDR, g_gdt_limit, KERNEL_VIRT);
}
