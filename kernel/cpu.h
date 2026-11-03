#ifndef BMX_CPU_H
#define BMX_CPU_H

/* Writes the 48-character CPU brand string into out (NUL terminated).
   Returns 0 on success, -1 when the CPU lacks extended brand leaves. */
int cpu_brand(char out[49]);

#endif
