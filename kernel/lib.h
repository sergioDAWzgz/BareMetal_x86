#ifndef BMX_LIB_H
#define BMX_LIB_H

#include <stddef.h>

void *memset(void *dst, int value, size_t count);
void *memcpy(void *dst, const void *src, size_t count);
void *memmove(void *dst, const void *src, size_t count);

#endif
