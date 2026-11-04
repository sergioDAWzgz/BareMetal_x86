/* The phase 4 user program.

   This step only proves the loader places it in memory. The ring-3 transition
   and the syscall interface arrive with the next step, and they are what make
   the message reach the console. */

static const char message[] __attribute__((used)) = "user: hello from ring 3\n";

/* Volatile so this store survives optimisation: it is what gives the image a
   writable segment with no file bytes, the case the loader has to zero fill. */
static volatile unsigned int writable;

int main(void)
{
    writable = (unsigned int)message[0];

    return 0;
}
