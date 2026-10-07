#ifndef JIT_RUNTIME_H
#define JIT_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uintptr_t entry_vaddr;
    uintptr_t fault_cr2;
    uintptr_t fault_rip;
    uintptr_t fault_rsp;
    uintptr_t error_code;
    int status;
} jit_runtime_status_t;

int jit_runtime_exec_driver(const char *source_code,
                           const char *entry_name,
                           jit_runtime_status_t *status);

#endif
