#include "io.h"
#include "shell.h"
#include "screen.h"
#include "idt.h"
#include <stdint.h>
#include <stdbool.h>

static bool shift_pressed = false;
static bool ctrl_pressed = false;
static bool caps_lock = false;
static bool extended_scancode = false;

unsigned char kbd_us[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};

unsigned char kbd_us_shift[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '\"', '~', 0, '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
};

static char kbd_buffer[256];
static int kbd_head = 0;
static int kbd_tail = 0;

void kbd_put(char c) {
    int next = (kbd_head + 1) % 256;
    if (next != kbd_tail) {
        kbd_buffer[kbd_head] = c;
        kbd_head = next;
    }
}

char kbd_get() {
    if (kbd_head == kbd_tail) return 0;
    char c = kbd_buffer[kbd_tail];
    kbd_tail = (kbd_tail + 1) % 256;
    return c;
}

void keyboard_handler() {
    uint8_t scancode = inb(0x60);

    if (scancode == 0xE0) {
        extended_scancode = true;
        apic_eoi();
        return;
    }

    if (extended_scancode) {
        extended_scancode = false;
        if (!(scancode & 0x80)) {
            if (scancode == 0x48) {
                kbd_put(SHELL_KEY_UP);
            } else if (scancode == 0x50) {
                kbd_put(SHELL_KEY_DOWN);
            }
        }
        apic_eoi();
        return;
    }

    if (scancode & 0x80) {
        uint8_t released = scancode & 0x7F;
        if (released == 0x2A || released == 0x36) shift_pressed = false;
        if (released == 0x1D) ctrl_pressed = false;
    } else {
        if (scancode == 0x2A || scancode == 0x36) {
            shift_pressed = true;
        } else if (scancode == 0x1D) {
            ctrl_pressed = true;
        } else if (scancode == 0x3A) {
            caps_lock = !caps_lock;
        } else if (scancode == 0x49) { // Page Up
            scroll_up();
        } else if (scancode == 0x51) { // Page Down
            scroll_down();
        } else {
            if (ctrl_pressed && scancode == 0x2E) { // Ctrl + C
                kbd_put(0x03); // ASCII ETX (End of Text)
            } else {
                char c = shift_pressed ? kbd_us_shift[scancode] : kbd_us[scancode];
                if (caps_lock && c >= 'a' && c <= 'z') c -= 32;
                if (c > 0) kbd_put(c);
            }
        }
    }
    apic_eoi();
}
