#include "cpu.h"

#define CPUID_EXTENDED_MAX 0x80000000u
#define CPUID_BRAND_FIRST 0x80000002u
#define CPUID_BRAND_LAST 0x80000004u

static void cpuid(unsigned int leaf, unsigned int *a, unsigned int *b,
                  unsigned int *c, unsigned int *d)
{
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(0));
}

static void copy_register(char *dst, unsigned int value)
{
    dst[0] = (char)(value & 0xFF);
    dst[1] = (char)((value >> 8) & 0xFF);
    dst[2] = (char)((value >> 16) & 0xFF);
    dst[3] = (char)((value >> 24) & 0xFF);
}

int cpu_brand(char out[49])
{
    unsigned int a;
    unsigned int b;
    unsigned int c;
    unsigned int d;
    unsigned int max_extended;
    unsigned int leaf;

    cpuid(CPUID_EXTENDED_MAX, &max_extended, &b, &c, &d);
    if (max_extended < CPUID_BRAND_LAST) {
        return -1;
    }

    for (leaf = CPUID_BRAND_FIRST; leaf <= CPUID_BRAND_LAST; leaf++) {
        unsigned int offset = (leaf - CPUID_BRAND_FIRST) * 16u;

        cpuid(leaf, &a, &b, &c, &d);
        copy_register(out + offset + 0, a);
        copy_register(out + offset + 4, b);
        copy_register(out + offset + 8, c);
        copy_register(out + offset + 12, d);
    }
    out[48] = '\0';

    /* Drop the leading spaces the brand leaves are padded with. */
    {
        unsigned int start = 0;

        while (start < 48 && out[start] == ' ') {
            start++;
        }
        if (start > 0) {
            unsigned int i = 0;

            while (start + i < 48) {
                out[i] = out[start + i];
                i++;
            }
            out[i] = '\0';
        }
    }

    return 0;
}
