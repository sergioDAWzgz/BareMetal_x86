/* The phase 4 user program.

   It runs in ring 3 with no kernel access beyond the int 0x80 gate, so every
   message it prints travels through the write syscall and the status the kernel
   reports is the value main returns. */

#include "libc.h"

/* Lives in .bss: the image carries no bytes for it, so a non-zero value here
   means the loader did not zero fill the page it mapped. */
static volatile unsigned int counter;

static unsigned int current_ring(void)
{
    uint16_t selector;

    __asm__ volatile("mov %%cs, %0" : "=r"(selector));

    return (unsigned int)(selector & 3);
}

static void say(const char *text)
{
    counter++;
    puts(text);
}

int main(void)
{
    if (counter != 0) {
        puts("user: the writable segment was not zeroed");
        return 1;
    }

    say("user: hello from ring 3");

    /* The program reads its own privilege level instead of trusting the kernel to
       report it: this message only appears if the CPU executed this code with
       CPL 3. */
    if (current_ring() == 3) {
        say("user: running in ring 3");
    } else {
        puts("user: not running in ring 3");
    }

    return 0;
}
