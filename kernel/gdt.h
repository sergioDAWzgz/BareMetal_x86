#ifndef BMX_GDT_H
#define BMX_GDT_H

#include <stdint.h>

#define GDT_SELECTOR_CODE 0x08
#define GDT_SELECTOR_DATA 0x10
#define GDT_SELECTOR_TSS 0x18

void gdt_init(void);

#endif
