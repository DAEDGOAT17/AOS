#ifndef SERIAL_H
#define SERIAL_H

#include <stddef.h>
#include <stdint.h>

void serial_init(void);
int serial_can_read(void);
int serial_read_char(void);
void serial_write_char(char c);
void serial_write_string(const char *str);
void serial_write_hex32(uint32_t value);
void serial_write_hex64(uint64_t value);
int serial_read_block(const char *begin_marker, const char *end_marker, char *buffer, size_t buffer_size);

#endif
