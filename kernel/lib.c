#include "lib.h"

void *memset(void *dst, int value, size_t count)
{
    unsigned char *out = (unsigned char *)dst;

    while (count-- > 0) {
        *out++ = (unsigned char)value;
    }
    return dst;
}

void *memcpy(void *dst, const void *src, size_t count)
{
    unsigned char *out = (unsigned char *)dst;
    const unsigned char *in = (const unsigned char *)src;

    while (count-- > 0) {
        *out++ = *in++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t count)
{
    unsigned char *out = (unsigned char *)dst;
    const unsigned char *in = (const unsigned char *)src;

    if (out < in) {
        while (count-- > 0) {
            *out++ = *in++;
        }
    } else {
        out += count;
        in += count;
        while (count-- > 0) {
            *--out = *--in;
        }
    }
    return dst;
}
