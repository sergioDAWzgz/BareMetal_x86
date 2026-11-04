#include "vmm.h"

#include "lib.h"
#include "memory.h"

#define PAGE_SIZE 4096ULL
#define PAGE_MASK (PAGE_SIZE - 1)
#define PAGE_ENTRIES 512ULL
#define DIRECTORY_SPAN (2ULL * 1024 * 1024)     /* one page directory entry */

#define USER_SPAN (USER_LIMIT - USER_BASE)
#define USER_TABLES (USER_SPAN / DIRECTORY_SPAN)

#define ENTRY_PRESENT 0x1ULL
#define ENTRY_WRITE 0x2ULL
#define ENTRY_USER 0x4ULL

static uintptr_t pml4_phys;
static uintptr_t pdpt_phys;
static uintptr_t pd_phys;
static uint64_t mapped_pages;
static const char *failure = "none";

static uint64_t *table_at(uintptr_t phys)
{
    return (uint64_t *)PHYS_TO_VIRT(phys);
}

/* A page table entry the CPU may already have cached has to be dropped by hand:
   x86 does not snoop software writes to page tables. New mappings of addresses
   the kernel never touched have no cached entry, but a table created and then
   reused does, so every write below is followed by one of these. */
static void invalidate(uint64_t address)
{
    __asm__ volatile("invlpg (%0)" : : "r"(address) : "memory");
}

int vmm_init(void)
{
    uint64_t cr3;
    uint64_t *pml4;
    uint64_t *pdpt;

    failure = "none";
    mapped_pages = 0;
    pml4_phys = 0;
    pdpt_phys = 0;
    pd_phys = 0;

    /* Neither loader publishes the address of the table it built, and the two
       place it in different memory, so the register the CPU is already walking
       is the only portable source. */
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    pml4_phys = (uintptr_t)(cr3 & ~PAGE_MASK);

    pdpt_phys = pmm_alloc_frames(1);
    pd_phys = pmm_alloc_frames(1);
    if (pdpt_phys == 0 || pd_phys == 0) {
        failure = "no frames for the user page tables";
        return -1;
    }
    memset(table_at(pdpt_phys), 0, PAGE_SIZE);
    memset(table_at(pd_phys), 0, PAGE_SIZE);

    pml4 = table_at(pml4_phys);
    if ((pml4[USER_PML4_INDEX] & ENTRY_PRESENT) != 0) {
        failure = "the user page map slot is already in use";
        return -1;
    }

    pml4[USER_PML4_INDEX] = (uint64_t)pdpt_phys |
                            ENTRY_PRESENT | ENTRY_WRITE | ENTRY_USER;

    /* One PDPT entry already covers a whole GiB, which is sixteen times the
       window above. */
    pdpt = table_at(pdpt_phys);
    pdpt[0] = (uint64_t)pd_phys | ENTRY_PRESENT | ENTRY_WRITE | ENTRY_USER;

    invalidate(USER_BASE);
    return 0;
}

/* Returns the page table entry that maps page, creating the page table for its
   2 MiB slot on first use, or 0 on failure. */
static uint64_t *user_pte(uint64_t page)
{
    uint64_t slot = page / PAGE_ENTRIES;
    uint64_t *pd;
    uint64_t *pt;

    if (slot >= USER_TABLES) {
        failure = "the page is outside the user window";
        return 0;
    }

    pd = table_at(pd_phys);
    if ((pd[slot] & ENTRY_PRESENT) == 0) {
        uintptr_t pt_phys = pmm_alloc_frames(1);

        if (pt_phys == 0) {
            failure = "no frames for a user page table";
            return 0;
        }
        memset(table_at(pt_phys), 0, PAGE_SIZE);
        pd[slot] = (uint64_t)pt_phys | ENTRY_PRESENT | ENTRY_WRITE | ENTRY_USER;
        invalidate(USER_BASE + slot * DIRECTORY_SPAN);
    }

    pt = table_at(pd[slot] & ~PAGE_MASK);
    return &pt[page % PAGE_ENTRIES];
}

int64_t vmm_map_user(uint64_t address, uint64_t size, const void *source,
                     uint64_t source_size)
{
    const uint8_t *bytes = (const uint8_t *)source;
    uint64_t start;
    uint64_t end;
    uint64_t virtual_address;
    int64_t pages = 0;

    if (size == 0) {
        return 0;
    }
    if (pml4_phys == 0) {
        failure = "the user page map is not initialised";
        return -1;
    }
    if (address < USER_BASE || size > USER_LIMIT - address) {
        failure = "the range is outside the user window";
        return -1;
    }

    start = address & ~PAGE_MASK;
    end = address + size;

    for (virtual_address = start; virtual_address < end;
         virtual_address += PAGE_SIZE) {
        uint64_t *entry = user_pte((virtual_address - USER_BASE) / PAGE_SIZE);
        uintptr_t frame;

        if (entry == 0) {
            return -1;
        }
        if ((*entry & ENTRY_PRESENT) != 0) {
            failure = "the page is already mapped";
            return -1;
        }

        frame = pmm_alloc_frames(1);
        if (frame == 0) {
            failure = "no frames for the user image";
            return -1;
        }
        /* A segment with a memory size larger than its file size relies on this
           zero fill for its .bss tail. */
        memset(PHYS_TO_VIRT(frame), 0, PAGE_SIZE);

        if (bytes != 0 && source_size != 0) {
            uint64_t begin = virtual_address > address ? virtual_address
                                                       : address;
            uint64_t finish = virtual_address + PAGE_SIZE < end
                                  ? virtual_address + PAGE_SIZE : end;
            uint64_t from = begin - address;
            uint64_t to = finish > address + source_size
                              ? source_size : finish - address;

            if (from < to) {
                memcpy((uint8_t *)PHYS_TO_VIRT(frame) +
                           (begin - virtual_address),
                       bytes + from, to - from);
            }
        }

        *entry = (uint64_t)frame | ENTRY_PRESENT | ENTRY_WRITE | ENTRY_USER;
        invalidate(virtual_address);
        pages++;
        mapped_pages++;
    }
    return pages;
}

uintptr_t vmm_user_phys(uint64_t address)
{
    uint64_t page;
    uint64_t *pml4;
    uint64_t *pdpt;
    uint64_t *pd;
    uint64_t *pt;

    if (pml4_phys == 0 || address < USER_BASE || address >= USER_LIMIT) {
        return 0;
    }

    pml4 = table_at(pml4_phys);
    if ((pml4[USER_PML4_INDEX] & ENTRY_PRESENT) == 0) {
        return 0;
    }
    pdpt = table_at(pml4[USER_PML4_INDEX] & ~PAGE_MASK);
    if ((pdpt[0] & ENTRY_PRESENT) == 0) {
        return 0;
    }
    pd = table_at(pdpt[0] & ~PAGE_MASK);

    page = (address - USER_BASE) / PAGE_SIZE;
    if (page / PAGE_ENTRIES >= USER_TABLES) {
        return 0;
    }
    if ((pd[page / PAGE_ENTRIES] & ENTRY_PRESENT) == 0) {
        return 0;
    }
    pt = table_at(pd[page / PAGE_ENTRIES] & ~PAGE_MASK);
    if ((pt[page % PAGE_ENTRIES] & ENTRY_PRESENT) == 0) {
        return 0;
    }
    return (uintptr_t)(pt[page % PAGE_ENTRIES] & ~PAGE_MASK) +
           (address & PAGE_MASK);
}

uint64_t vmm_user_pages(void)
{
    return mapped_pages;
}

uint64_t vmm_audit_supervisor(void)
{
    /* The slots the loaders fill: identity, direct map and the kernel window. */
    static const uint64_t slots[3] = { 0, 256, 511 };
    uint64_t flagged = 0;
    uint64_t *pml4;
    uint32_t index;

    if (pml4_phys == 0) {
        return 0;
    }
    pml4 = table_at(pml4_phys);

    for (index = 0; index < 3; index++) {
        uint64_t *pdpt;
        uint32_t pdpt_entry;

        if ((pml4[slots[index]] & ENTRY_PRESENT) == 0) {
            continue;
        }
        if ((pml4[slots[index]] & ENTRY_USER) != 0) {
            flagged++;
        }
        pdpt = table_at(pml4[slots[index]] & ~PAGE_MASK);

        for (pdpt_entry = 0; pdpt_entry < PAGE_ENTRIES; pdpt_entry++) {
            uint64_t *pd;
            uint32_t pd_entry;

            if ((pdpt[pdpt_entry] & ENTRY_PRESENT) == 0) {
                continue;
            }
            if ((pdpt[pdpt_entry] & ENTRY_USER) != 0) {
                flagged++;
            }
            pd = table_at(pdpt[pdpt_entry] & ~PAGE_MASK);

            /* The loaders map every window with 2 MiB pages, so the page
               directory is the last level on these subtrees. */
            for (pd_entry = 0; pd_entry < PAGE_ENTRIES; pd_entry++) {
                if ((pd[pd_entry] & ENTRY_PRESENT) == 0) {
                    continue;
                }
                if ((pd[pd_entry] & ENTRY_USER) != 0) {
                    flagged++;
                }
            }
        }
    }
    return flagged;
}

const char *vmm_error(void)
{
    return failure;
}
