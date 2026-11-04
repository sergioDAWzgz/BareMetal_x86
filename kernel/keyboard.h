#ifndef BMX_KEYBOARD_H
#define BMX_KEYBOARD_H

/* PS/2 keyboard on the 8042 controller, scancode set 1, IRQ1.
   Decoded ASCII is queued and drained with keyboard_getchar. */
#define KEY_ESCAPE 0x1B
#define KEY_BACKSPACE '\b'
#define KEY_ENTER '\n'

int keyboard_init(void);
int keyboard_ready(void);

/* Called from the interrupt dispatcher for IRQ1. */
void keyboard_handle(void);

/* Returns the next queued character, or -1 when the queue is empty. */
int keyboard_getchar(void);

/* Feeds a known scancode sequence through the decoder without touching the
   hardware, so the gate can prove the translation table is intact. */
int keyboard_self_test(void);

#endif
