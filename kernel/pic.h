#ifndef BMX_PIC_H
#define BMX_PIC_H

#include <stdint.h>

void pic_init(void);
void pit_init(uint32_t hz);
void pic_eoi(uint8_t irq);
void pic_unmask(uint8_t irq);
void timer_handle(void);
uint64_t timer_ticks_get(void);

#endif
