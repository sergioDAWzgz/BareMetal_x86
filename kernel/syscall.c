#include "syscall.h"
#include "console.h"
#include "syscall-abi.h"
#include "usermode.h"
#include "vmm.h"

#define USER_PAGE_SIZE 4096ULL

static int reported;

/* A program can only hand the kernel pointers into its own window. A buffer that
   leaves the window, or a page the loader never mapped, is rejected before a
   single byte is read through it: ring 3 owns no other memory, so anything else
   is either a bug or an attempt to read the kernel. */
static int user_range(const void *buffer, uint64_t size)
{
    uint64_t address = (uint64_t)(uintptr_t)buffer;
    uint64_t end = address + size;
    uint64_t page;

    if (size == 0) {
        return 0;
    }
    /* The sum wrapped, so the range cannot be walked page by page. */
    if (end < address) {
        return -1;
    }

    for (page = address & ~(USER_PAGE_SIZE - 1); page < end;
         page += USER_PAGE_SIZE) {
        if (vmm_user_phys(page) == 0) {
            return -1;
        }
    }
    return 0;
}

static void write_user(const void *buffer, uint64_t size)
{
    const char *bytes = (const char *)buffer;
    uint64_t index;

    for (index = 0; index < size; index++) {
        console_putc(bytes[index]);
    }
}

void syscall_dispatch(registers_t *registers)
{
    /* Printed once, from the frame the CPU built on the transition: it is the
       only evidence that the program is not merely a kernel function chain. */
    if (!reported) {
        reported = 1;
        console_write("sys: first syscall from cs 0x");
        console_hex_width(registers->cs, 2);
        console_write("\n");
    }

    switch (registers->rax) {
    case SYSCALL_WRITE:
        if (user_range((const void *)(uintptr_t)registers->rdi,
                       registers->rsi) != 0) {
            console_write("sys: write outside the user window rejected\n");
            registers->rax = (uint64_t)-1;
            return;
        }
        write_user((const void *)(uintptr_t)registers->rdi, registers->rsi);
        registers->rax = registers->rsi;
        return;

    case SYSCALL_EXIT:
        console_write("user: exited with status ");
        console_dec(registers->rdi);
        console_write("\n");
        /* Leaves the abandoned user frames behind and returns to the kernel
           caller that started the process; there is nothing to unwind. */
        usermode_resume();
        return;

    default:
        console_write("sys: unknown call ");
        console_dec(registers->rax);
        console_write("\n");
        registers->rax = (uint64_t)-1;
        return;
    }
}
