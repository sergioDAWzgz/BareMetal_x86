#include "libc.h"
#include "syscall-abi.h"

/* The kernel writes its result into the saved rax slot of the interrupt frame,
   so iretq hands it back in rax and this output operand picks it up. Memory is
   clobbered because the call reads the buffer between the two sides. */
static uint64_t syscall(uint64_t number, uint64_t first, uint64_t second)
{
    uint64_t result;

    __asm__ volatile("int $0x80"
                     : "=a"(result)
                     : "a"(number), "D"(first), "S"(second)
                     : "memory");

    return result;
}

uint64_t strlen(const char *text)
{
    uint64_t length = 0;

    while (text[length] != '\0') {
        length++;
    }
    return length;
}

void *memset(void *destination, int value, uint64_t count)
{
    uint8_t *bytes = (uint8_t *)destination;
    uint64_t index;

    for (index = 0; index < count; index++) {
        bytes[index] = (uint8_t)value;
    }
    return destination;
}

void *memcpy(void *destination, const void *source, uint64_t count)
{
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    uint64_t index;

    for (index = 0; index < count; index++) {
        out[index] = in[index];
    }
    return destination;
}

void write(const char *text)
{
    syscall(SYSCALL_WRITE, (uint64_t)(uintptr_t)text, strlen(text));
}

void puts(const char *text)
{
    static const char newline = '\n';

    write(text);
    syscall(SYSCALL_WRITE, (uint64_t)(uintptr_t)&newline, 1);
}

void exit(int status)
{
    syscall(SYSCALL_EXIT, (uint64_t)status, 0);

    /* Unreachable: a returned exit would mean the kernel resumed the program it
       was told to end, which is worse than halting here. */
    for (;;) {
    }
}
