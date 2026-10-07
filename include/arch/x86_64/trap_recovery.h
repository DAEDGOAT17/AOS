#ifndef TRAP_RECOVERY_H
#define TRAP_RECOVERY_H

#include <stdint.h>

typedef struct {
    uint64_t rip;
    uint64_t rsp;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t fault_cr2;
    uint64_t fault_rip;
    uint64_t fault_rsp;
} kernel_jmp_buf;

extern kernel_jmp_buf g_jit_recovery_env;
extern volatile int g_jit_executing;

int kernel_setjmp(kernel_jmp_buf *env);
void kernel_longjmp(kernel_jmp_buf *env, int val) __attribute__((noreturn));
void trap_capture_and_dump(uint64_t fault_rip, uint64_t fault_cr2, uint64_t fault_rsp, uint64_t error_code);

#endif
