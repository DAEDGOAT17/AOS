#include "drivers/serial.h"
#include "io.h"
#include "string.h"

#define SERIAL_PORT_BASE 0x3F8

static int serial_is_transmit_empty(void) {
    return (inb(SERIAL_PORT_BASE + 5) & 0x20) != 0;
}

void serial_init(void) {
    outb(SERIAL_PORT_BASE + 1, 0x00);
    outb(SERIAL_PORT_BASE + 3, 0x80);
    outb(SERIAL_PORT_BASE + 0, 0x01);
    outb(SERIAL_PORT_BASE + 1, 0x00);
    outb(SERIAL_PORT_BASE + 3, 0x03);
    outb(SERIAL_PORT_BASE + 2, 0xC7);
    outb(SERIAL_PORT_BASE + 4, 0x0B);
}

int serial_can_read(void) {
    return (inb(SERIAL_PORT_BASE + 5) & 0x01) != 0;
}

int serial_read_char(void) {
    if (!serial_can_read()) {
        return -1;
    }
    return inb(SERIAL_PORT_BASE);
}

void serial_write_char(char c) {
    while (!serial_is_transmit_empty()) {
        asm volatile("pause");
    }
    outb(SERIAL_PORT_BASE, (unsigned char)c);
}

void serial_write_string(const char *str) {
    if (!str) {
        return;
    }

    while (*str) {
        serial_write_char(*str++);
    }
}

void serial_write_hex32(uint32_t value) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[9];
    int i;

    for (i = 7; i >= 0; --i) {
        buf[i] = hex[value & 0xF];
        value >>= 4;
    }
    buf[8] = '\0';
    serial_write_string(buf);
}

void serial_write_hex64(uint64_t value) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[17];
    int i;

    for (i = 15; i >= 0; --i) {
        buf[i] = hex[value & 0xF];
        value >>= 4;
    }
    buf[16] = '\0';
    serial_write_string(buf);
}

int serial_read_block(const char *begin_marker, const char *end_marker, char *buffer, size_t buffer_size) {
    size_t begin_len = begin_marker ? strlen(begin_marker) : 0;
    size_t end_len = end_marker ? strlen(end_marker) : 0;
    size_t len = 0;
    int ch;

    if (!buffer || buffer_size == 0) {
        return 0;
    }

    if (begin_marker && begin_len > 0) {
        char temp[64];
        size_t temp_len = 0;
        while (temp_len < begin_len) {
            ch = serial_read_char();
            if (ch < 0) {
                continue;
            }
            temp[temp_len++] = (char)ch;
        }
        if (strncmp(temp, begin_marker, begin_len) != 0) {
            return 0;
        }
    }

    while (len + 1 < buffer_size) {
        ch = serial_read_char();
        if (ch < 0) {
            continue;
        }
        buffer[len++] = (char)ch;
        if (end_marker && end_len > 0 && len >= end_len) {
            size_t tail = len - end_len;
            if (strncmp(buffer + tail, end_marker, end_len) == 0) {
                buffer[tail] = '\0';
                return 1;
            }
        }
    }

    buffer[len] = '\0';
    return 0;
}
