#include "keyboard.h"
#include "io.h"
#include "pic.h"

#define KBD_DATA_PORT 0x60
#define KBD_COMMAND_PORT 0x64

#define STATUS_OUTPUT_FULL 0x01
#define STATUS_INPUT_FULL 0x02
#define STATUS_AUX_DATA 0x20

#define KBD_ACK 0xFA
#define KBD_IRQ 1
#define KBD_QUEUE_SIZE 64
#define KBD_SPIN_LIMIT 100000u

#define SCAN_ESCAPE 0x01
#define SCAN_LEFT_SHIFT 0x2A
#define SCAN_RIGHT_SHIFT 0x36
#define SCAN_CONTROL 0x1D
#define SCAN_CAPS_LOCK 0x3A
#define SCAN_EXTENDED 0xE0

/* Scancode set 1, unshifted and shifted. Unassigned entries stay zero. */
static const char kbd_map[128] = {
    [0x01] = KEY_ESCAPE,
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=',
    [0x0E] = KEY_BACKSPACE,
    [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '[', [0x1B] = ']',
    [0x1C] = KEY_ENTER,
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
    [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm',
    [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x39] = ' '
};

static const char kbd_map_shift[128] = {
    [0x01] = KEY_ESCAPE,
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
    [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0A] = '(', [0x0B] = ')',
    [0x0C] = '_', [0x0D] = '+',
    [0x0E] = KEY_BACKSPACE,
    [0x0F] = '\t',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T',
    [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I', [0x18] = 'O', [0x19] = 'P',
    [0x1A] = '{', [0x1B] = '}',
    [0x1C] = KEY_ENTER,
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G',
    [0x23] = 'H', [0x24] = 'J', [0x25] = 'K', [0x26] = 'L',
    [0x27] = ':', [0x28] = '"', [0x29] = '~',
    [0x2B] = '|',
    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V', [0x30] = 'B',
    [0x31] = 'N', [0x32] = 'M',
    [0x33] = '<', [0x34] = '>', [0x35] = '?',
    [0x39] = ' '
};

typedef struct {
    int shift;
    int control;
    int caps;
    int extended;
} kbd_state_t;

static kbd_state_t state;
static volatile char queue[KBD_QUEUE_SIZE];
static volatile unsigned int queue_head;
static volatile unsigned int queue_tail;
static volatile int ready;

static void queue_push(char c)
{
    unsigned int next = (queue_head + 1) % KBD_QUEUE_SIZE;

    if (next == queue_tail) {
        return; /* full: drop the newest character */
    }
    queue[queue_head] = c;
    queue_head = next;
}

/* Pure translation step: no ports, no globals. Returns 1 and writes *out when
   the scancode completes a character. */
static int kbd_translate(uint8_t scancode, kbd_state_t *st, char *out)
{
    char value;

    if (scancode == SCAN_EXTENDED) {
        st->extended = 1;
        return 0;
    }

    if ((scancode & 0x80) != 0) {
        uint8_t code = (uint8_t)(scancode & 0x7F);

        st->extended = 0;
        if (code == SCAN_LEFT_SHIFT || code == SCAN_RIGHT_SHIFT) {
            st->shift = 0;
        } else if (code == SCAN_CONTROL) {
            st->control = 0;
        }
        return 0;
    }

    if (st->extended) {
        st->extended = 0;
        return 0;
    }

    if (scancode == SCAN_LEFT_SHIFT || scancode == SCAN_RIGHT_SHIFT) {
        st->shift = 1;
        return 0;
    }
    if (scancode == SCAN_CONTROL) {
        st->control = 1;
        return 0;
    }
    if (scancode == SCAN_CAPS_LOCK) {
        st->caps = !st->caps;
        return 0;
    }

    value = st->shift ? kbd_map_shift[scancode] : kbd_map[scancode];
    if (value == '\0') {
        return 0;
    }

    if (st->caps && value >= 'a' && value <= 'z') {
        value = (char)(value - 'a' + 'A');
    } else if (st->caps && st->shift && value >= 'A' && value <= 'Z') {
        value = (char)(value - 'A' + 'a');
    }

    if (st->control) {
        if (value >= 'a' && value <= 'z') {
            value = (char)(value - 'a' + 1);
        } else if (value >= 'A' && value <= 'Z') {
            value = (char)(value - 'A' + 1);
        }
    }

    *out = value;
    return 1;
}

static int wait_input_empty(void)
{
    unsigned int spin;

    for (spin = 0; spin < KBD_SPIN_LIMIT; spin++) {
        if ((inb(KBD_COMMAND_PORT) & STATUS_INPUT_FULL) == 0) {
            return 0;
        }
    }
    return -1;
}

static int wait_output_full(void)
{
    unsigned int spin;

    for (spin = 0; spin < KBD_SPIN_LIMIT; spin++) {
        if ((inb(KBD_COMMAND_PORT) & STATUS_OUTPUT_FULL) != 0) {
            return 0;
        }
    }
    return -1;
}

static void drain_output(void)
{
    unsigned int spin;

    for (spin = 0; spin < 64; spin++) {
        if ((inb(KBD_COMMAND_PORT) & STATUS_OUTPUT_FULL) == 0) {
            break;
        }
        (void)inb(KBD_DATA_PORT);
    }
}

static int controller_command(uint8_t command)
{
    if (wait_input_empty() != 0) {
        return -1;
    }
    outb(KBD_COMMAND_PORT, command);
    return 0;
}

static int device_command(uint8_t command)
{
    if (wait_input_empty() != 0) {
        return -1;
    }
    outb(KBD_DATA_PORT, command);
    return 0;
}

int keyboard_init(void)
{
    /* The firmware leaves its own state behind, so start from a known one:
       disable the port, flush everything, re-enable it and turn scanning on. */
    drain_output();

    if (controller_command(0xAD) != 0) {
        return -1;
    }
    drain_output();

    if (controller_command(0xAE) != 0) {
        return -1;
    }
    (void)wait_output_full();
    drain_output();

    if (device_command(0xF4) != 0) {
        return -1;
    }
    (void)wait_output_full();
    drain_output();

    state.shift = 0;
    state.control = 0;
    state.caps = 0;
    state.extended = 0;
    queue_head = 0;
    queue_tail = 0;

    ready = 1;
    pic_unmask(KBD_IRQ);
    return 0;
}

int keyboard_ready(void)
{
    return ready;
}

void keyboard_handle(void)
{
    unsigned int count;

    for (count = 0; count < 16; count++) {
        uint8_t status = inb(KBD_COMMAND_PORT);
        uint8_t scancode;
        char decoded;

        if ((status & STATUS_OUTPUT_FULL) == 0) {
            break;
        }
        if ((status & STATUS_AUX_DATA) != 0) {
            (void)inb(KBD_DATA_PORT); /* mouse byte, not ours yet */
            continue;
        }

        scancode = inb(KBD_DATA_PORT);
        if (kbd_translate(scancode, &state, &decoded)) {
            queue_push(decoded);
        }
    }

    pic_eoi(KBD_IRQ);
}

int keyboard_getchar(void)
{
    char value;

    if (queue_tail == queue_head) {
        return -1;
    }
    value = queue[queue_tail];
    queue_tail = (queue_tail + 1) % KBD_QUEUE_SIZE;
    return (int)(unsigned char)value;
}

int keyboard_self_test(void)
{
    /* A chronological scancode stream: "hello", then shift + 1, then caps lock
       toggled on and a. Every press and release appears in the order the 8042
       would deliver it. */
    static const uint8_t stream[] = {
        0x23, 0xA3,                     /* h */
        0x12, 0x92,                     /* e */
        0x26, 0xA6,                     /* l */
        0x26, 0xA6,                     /* l */
        0x18, 0x98,                     /* o */
        0x2A, 0x02, 0x82, 0xAA,         /* shift down, 1, 1 up, shift up */
        0x3A, 0xBA,                     /* caps lock on */
        0x1E, 0x9E                      /* a */
    };
    static const char expected[] = "hello!A";
    kbd_state_t probe = { 0, 0, 0, 0 };
    unsigned int index;
    unsigned int count = 0;
    char decoded;

    for (index = 0; index < sizeof(stream); index++) {
        if (!kbd_translate(stream[index], &probe, &decoded)) {
            continue;
        }
        if (count >= sizeof(expected) - 1 || decoded != expected[count]) {
            return -1;
        }
        count++;
    }

    return count == sizeof(expected) - 1 ? 0 : -1;
}
