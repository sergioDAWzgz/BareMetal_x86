#include "linuxboot.h"

#include "acpi.h"
#include "console.h"
#include "fat32.h"
#include "lib.h"
#include "memory.h"

/* The setup header sits at 0x1f1 of the image and at the same offset inside
   struct boot_params, so one set of offsets serves both. The values come from
   Documentation/arch/x86/boot.rst; they are ABI and do not move. */
#define HDR_SETUP_SECTS        0x01F1u
#define HDR_JUMP_OFFSET        0x0201u
#define HDR_MAGIC              0x0202u
#define HDR_PROTOCOL           0x0206u
#define HDR_TYPE_OF_LOADER     0x0210u
#define HDR_CODE32_START       0x0214u
#define HDR_CMD_LINE_PTR       0x0228u
#define HDR_KERNEL_ALIGNMENT   0x0230u
#define HDR_RELOCATABLE        0x0234u
#define HDR_XLOADFLAGS         0x0236u
#define HDR_PREF_ADDRESS       0x0258u
#define HDR_INIT_SIZE          0x0260u

#define HDR_MAGIC_VALUE 0x53726448u     /* "HdrS", the protocol 2.00 signature */
#define XLF_KERNEL_64   0x0001u         /* the 64-bit entry point at +0x200 exists */

/* Documentation/arch/x86/boot.rst lists type_of_loader as a write the boot
   loader must perform, and names 0xFF for one without an assigned id. It is not
   bookkeeping: the kernel treats a zero there as "no bootloader supplied an
   initrd", so early_reserve_initrd() and reserve_initrd() both return without
   reserving anything, initrd_start stays zero and populate_rootfs() never
   unpacks the archive that is already in memory. The failure is silent because
   the panic only says the root device is missing. header.S ships the field as
   zero with a comment telling loaders to change it. */
#define LOADER_TYPE_UNASSIGNED 0xFFu

/* The setup header starts at 0x1f1 and runs past 0x260, so it does not fit the
   512-byte boot sector: it continues into the setup sectors behind it. Two
   sectors cover the longest header the protocol allows (0x202 + 0x7f). */
#define SETUP_BYTES 1024u

/* Where the 64-bit entry point sits inside the protected-mode part of the image:
   head_64.S fixes startup_64 at this offset with .org, and calls it immutable. */
#define ENTRY_OFFSET 0x200u

/* Offsets inside struct boot_params that are not part of the copied header.
   Documentation/arch/x86/zero-page.rst is the reference for these. */
#define BP_ACPI_RSDP_ADDR 0x0070u
#define BP_E820_ENTRIES   0x01E8u
#define BP_E820_TABLE     0x02D0u
#define BP_SIZE           4096u
/* The table has room for 128 entries; the loader's own map is bounded at 64. */
#define BP_E820_MAX       128u

/* struct e820_entry in boot_params is addr, size, type and nothing else. The
   loader's own entry carries a fourth field the firmware needs and Linux does
   not, so the map is repacked here instead of copied. */
#define BP_E820_ENTRY_SIZE 20u

#define DEFAULT_ALIGNMENT 0x200000ULL   /* what a relocatable x86_64 kernel asks for */
#define DEFAULT_LOAD_ADDRESS 0x100000ULL /* where a kernel without pref_address runs */
#define PAGE_SIZE 4096u

/* The real root is the Alpine ext4 partition, named by the MBR disk signature
   build.sh writes. No initramfs is passed: ext4, AHCI and the block layer are
   built in, so the kernel mounts the root itself and runs /sbin/init. */
static const char CMD_LINE[] =
    "root=PARTUUID=424d5836-02 rw console=ttyS0,115200 "
    "earlyprintk=serial,ttyS0,115200";

static const char *failure = "none";
static uint8_t setup[SETUP_BYTES];

extern void linux_jump(uint64_t entry, uint64_t boot_params_phys);

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

static uint64_t read64(const uint8_t *data)
{
    return (uint64_t)read32(data) | ((uint64_t)read32(data + 4) << 32);
}

static void write32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value & 0xFF);
    data[1] = (uint8_t)((value >> 8) & 0xFF);
    data[2] = (uint8_t)((value >> 16) & 0xFF);
    data[3] = (uint8_t)((value >> 24) & 0xFF);
}

static void write64(uint8_t *data, uint64_t value)
{
    write32(data, (uint32_t)(value & 0xFFFFFFFFu));
    write32(data + 4, (uint32_t)(value >> 32));
}

static uint64_t align_up(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

/* Fills the parts of struct boot_params the loader is responsible for. The
   header has already been copied in, so its fields are read back from there like
   the protocol says a loader must: read, modify where the protocol allows it,
   write back. */
static void build_boot_params(uint8_t *boot_params, uint32_t pm_start,
                              uint64_t cmd_line_phys)
{
    uint32_t entries = memmap_count();
    uint32_t index;

    /* Before anything else that depends on it: the initrd reservation and the
       unpack both key off this byte. */
    boot_params[HDR_TYPE_OF_LOADER] = LOADER_TYPE_UNASSIGNED;

    write32(boot_params + HDR_CODE32_START, pm_start);
    write32(boot_params + HDR_CMD_LINE_PTR, (uint32_t)cmd_line_phys);

    /* On the UEFI path the RSDP only exists in the firmware configuration table,
       which cannot be scanned once boot services are gone; passing the address
       we already validated is what keeps ACPI working there. */
    write64(boot_params + BP_ACPI_RSDP_ADDR, acpi_rsdp_physical());

    if (entries > BP_E820_MAX) {
        entries = BP_E820_MAX;
    }
    for (index = 0; index < entries; index++) {
        const e820_entry_t *source = memmap_entries() + index;
        uint8_t *target = boot_params + BP_E820_TABLE +
                          (uint64_t)index * BP_E820_ENTRY_SIZE;

        write64(target, source->base);
        write64(target + 8, source->length);
        write32(target + 16, source->type);
    }
    boot_params[BP_E820_ENTRIES] = (uint8_t)entries;
}

/* The addresses a copy of the image at `load` involves. pm_start is where the
   protected-mode part lands: the file is copied in one piece, boot sector first,
   so it starts setup_sects + 1 sectors in, and that is what code32_start has to
   name. The 64-bit entry point is 0x200 bytes into it.

   runtime_start is where the decompressor relocates the image. head_64.S takes
   the runtime address of startup_32, rounds it up to kernel_alignment and clamps
   it to LOAD_PHYSICAL_ADDR, which for an aligned load above 1 MiB is pm_start
   rounded up.

   The decompression then writes align_up(init_size, kernel_alignment) bytes
   there: misc.c works with ALIGN(max(output_len, kernel_total_size), 2 MiB), and
   init_size bounds that product from below. The image itself can be larger than
   the decompression window, so the caller reserves the larger of the two. */
static void load_window(uint32_t setup_sects, uint32_t alignment,
                        uint32_t init_size, uint64_t load, uint64_t file_size,
                        uint64_t *pm_start, uint64_t *runtime_start,
                        uint64_t *reserved_end)
{
    *pm_start = load + (uint64_t)(setup_sects + 1) * FAT32_SECTOR_SIZE;
    *runtime_start = align_up(*pm_start, alignment);
    *reserved_end = *runtime_start + align_up(init_size, alignment);
    if (load + file_size > *reserved_end) {
        *reserved_end = load + file_size;
    }
}

int linux_payload_boot(void)
{
    fat32_file_t file;
    uint8_t *boot_params;
    char *cmd_line;
    uint64_t boot_params_phys;
    uint64_t cmd_line_phys;
    uint64_t entry;
    uint64_t load;
    uint64_t pm_start;
    uint64_t reserved_end;
    uint64_t runtime_start;
    uint32_t alignment;
    uint32_t header_end;
    uint32_t init_size;
    uint32_t protocol;
    uint32_t setup_sects;
    int32_t bytes;

    failure = "none";

    if (fat32_open("LINUX.BIN", &file) != 0) {
        failure = fat32_error();
        return -1;
    }
    if (file.size < sizeof(setup)) {
        failure = "LINUX.BIN is smaller than a setup header";
        return -1;
    }

    /* The address the rest of the file goes to comes out of the header, so the
       head of the file is read before anything is reserved. */
    bytes = fat32_read_at(&file, 0, setup, sizeof(setup), sizeof(setup));
    if (bytes < 0) {
        failure = fat32_error();
        return -1;
    }

    if (read32(setup + HDR_MAGIC) != HDR_MAGIC_VALUE) {
        failure = "LINUX.BIN does not start with the HdrS signature";
        return -1;
    }
    /* 2.12 is the first protocol with xloadflags, and bit 0 of that field is the
       kernel's own promise that the 64-bit entry point exists. */
    protocol = read16(setup + HDR_PROTOCOL);
    if (protocol < 0x020C) {
        failure = "LINUX.BIN speaks a boot protocol older than 2.12";
        return -1;
    }
    if ((read16(setup + HDR_XLOADFLAGS) & XLF_KERNEL_64) == 0) {
        failure = "LINUX.BIN has no 64-bit entry point";
        return -1;
    }
    /* A non-relocatable kernel decompresses to 1 MiB whatever the loader asks,
       which is where this kernel is still running from. */
    if (setup[HDR_RELOCATABLE] == 0) {
        failure = "LINUX.BIN is not relocatable and would decompress over this kernel";
        return -1;
    }

    /* Zero means four, for a boot sector written before protocol 2.00. */
    setup_sects = setup[HDR_SETUP_SECTS];
    if (setup_sects == 0) {
        setup_sects = 4;
    }

    alignment = read32(setup + HDR_KERNEL_ALIGNMENT);
    if (alignment == 0) {
        alignment = (uint32_t)DEFAULT_ALIGNMENT;
    }
    if (alignment < PAGE_SIZE || (alignment & (alignment - 1)) != 0) {
        failure = "LINUX.BIN declares an unusable load alignment";
        return -1;
    }

    init_size = read32(setup + HDR_INIT_SIZE);
    if (init_size < 0x100000u) {
        failure = "LINUX.BIN declares an init_size too small to decompress into";
        return -1;
    }

    load = read64(setup + HDR_PREF_ADDRESS);
    if (load == 0) {
        load = DEFAULT_LOAD_ADDRESS;
    }
    load = align_up(load, alignment);

    /* The kernel is relocatable -- a non-relocatable one was refused above -- so
       the address in its header is a preference, not a constraint. Firmware owns
       memory in places the header cannot know: on the UEFI path a reserved block
       sits immediately above 16 MiB, exactly where pref_address points. Walk up
       from the preferred address to the first window that is actually free;
       code32_start below names where the image really landed. */
    for (;;) {
        load_window(setup_sects, alignment, init_size, load, file.size,
                    &pm_start, &runtime_start, &reserved_end);
        if (reserved_end < load || load >= pmm_limit() ||
            alignment > pmm_limit() - load) {
            failure = "no free window is large enough for the Linux image";
            return -1;
        }
        if (pmm_reserve_range(load, reserved_end - load) == 0) {
            break;
        }
        load += alignment;
    }
    entry = pm_start + ENTRY_OFFSET;

    if (pm_start > 0xFFFFFFFFu) {
        failure = "the image would land above the 4 GiB code32_start can name";
        return -1;
    }

    bytes = fat32_read_at(&file, 0, PHYS_TO_VIRT(load),
                          (uint32_t)(reserved_end - load), file.size);
    if (bytes < 0) {
        failure = fat32_error();
        return -1;
    }

    boot_params_phys = pmm_alloc_frame();
    cmd_line_phys = pmm_alloc_frame();
    if (boot_params_phys == 0 || cmd_line_phys == 0) {
        failure = "no frame for the zero page or the command line";
        return -1;
    }
    if (cmd_line_phys > 0xFFFFFFFFu) {
        failure = "the command line would land above the 4 GiB pointer limit";
        return -1;
    }

    boot_params = (uint8_t *)PHYS_TO_VIRT(boot_params_phys);
    cmd_line = (char *)PHYS_TO_VIRT(cmd_line_phys);
    memset(boot_params, 0, BP_SIZE);
    memset(cmd_line, 0, PAGE_SIZE);
    memcpy(cmd_line, CMD_LINE, sizeof(CMD_LINE));

    /* The header is copied verbatim, starting at its base offset, so every field
       the kernel reads back from boot_params is the kernel's own value. The end
       comes from the header itself: the protocol defines the second byte of the
       boot sector's jump instruction as a signed offset relative to 0x202 and
       says it "can be used to determine the size of the header". Reading past
       that end would import setup code into the zero page. Signed because a
       header longer than 127 bytes pushes that byte past 0x7f. */
    header_end = 0x202u + (uint32_t)(int32_t)(int8_t)setup[HDR_JUMP_OFFSET];
    if (header_end <= HDR_MAGIC || header_end > sizeof(setup)) {
        failure = "LINUX.BIN declares a setup header length outside the bytes that were read";
        return -1;
    }
    memcpy(boot_params + HDR_SETUP_SECTS, setup + HDR_SETUP_SECTS,
           header_end - HDR_SETUP_SECTS);

    build_boot_params(boot_params, (uint32_t)pm_start, cmd_line_phys);

    console_write("linux: LINUX.BIN ");
    console_dec(file.size);
    console_write(" bytes protocol ");
    console_dec(protocol >> 8);
    console_write(".");
    console_dec(protocol & 0xFFu);
    console_write(" setup ");
    console_dec(setup_sects);
    console_write(" sectors\n");

    console_write("linux: load ");
    console_hex64(load);
    console_write(" init_size ");
    console_hex64(init_size);
    console_write(" runtime ");
    console_hex64(runtime_start);
    console_write("\n");

    console_write("linux: boot_params ");
    console_hex64(boot_params_phys);
    console_write(" cmdline ");
    console_hex64(cmd_line_phys);
    console_write("\n");

    console_write("linux: e820 ");
    console_dec(boot_params[BP_E820_ENTRIES]);
    console_write(" entries\n");

    console_write("linux: jumping to the 64-bit entry at ");
    console_hex64(entry);
    console_write("\n");

    linux_jump(entry, boot_params_phys);

    /* The 64-bit entry point cannot return: a CPU in long mode faults rather
       than unwinding the jump. Reaching this line means the handoff assembly
       itself is broken, not that Linux declined to start. */
    failure = "linux_jump returned";
    return -1;
}

const char *linux_payload_error(void)
{
    return failure;
}
