#ifndef KSYM_H
#define KSYM_H

#include <stdint.h>

typedef struct {
    const char *name;
    uintptr_t addr;
} kernel_symbol_t;

#define EXPORT_SYMBOL(sym) \
    __attribute__((section(".ksymtab"), used)) \
    const kernel_symbol_t __ksym_##sym = { #sym, (uintptr_t)&sym }

uintptr_t ksym_lookup(const char *name);

#endif
