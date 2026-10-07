#ifndef E1000_H
#define E1000_H

#include <stdint.h>

int e1000_init(uint32_t bus, uint32_t device, uint32_t function);
void e1000_poll(void);

#endif