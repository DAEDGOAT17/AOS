#include "arch/x86_64/trap_recovery.h"
#include "drivers/serial.h"
#include "screen.h"
#include "string.h"

kernel_jmp_buf g_jit_recovery_env = {0};
volatile int g_jit_executing = 0;

static void trap_dump_registers(uint64_t fault_rip, uint64_t fault_cr2, uint64_t fault_rsp) {
    serial_write_string("JIT: fault report -> CR2=0x");
    serial_write_hex64(fault_cr2);
    serial_write_string(" RIP=0x");
    serial_write_hex64(fault_rip);
    serial_write_string(" RSP=0x");
    serial_write_hex64(fault_rsp);
    serial_write_string("\n");
}

void trap_capture_and_dump(uint64_t fault_rip, uint64_t fault_cr2, uint64_t fault_rsp, uint64_t error_code) {
    serial_write_string("--- KERNEL TRAP DUMP ---\n");
    serial_write_string("error_code=0x");
    serial_write_hex64(error_code);
    serial_write_string("\n");
    trap_dump_registers(fault_rip, fault_cr2, fault_rsp);
    serial_write_string("--- END TRAP DUMP ---\n");
}

int kernel_setjmp(kernel_jmp_buf *env) {
    if (!env) {
        return -1;
    }

    asm volatile(
        "mov %%rsp, %0\n\t"
        "mov %%rbp, %1\n\t"
        "mov %%rbx, %2\n\t"
        "mov %%r12, %3\n\t"
        "mov %%r13, %4\n\t"
        "mov %%r14, %5\n\t"
        "mov %%r15, %6\n\t"
        : "=r"(env->rsp), "=r"(env->rbp), "=r"(env->rbx),
          "=r"(env->r12), "=r"(env->r13), "=r"(env->r14), "=r"(env->r15)
        :
        : "memory"
    );

    env->rip = (uint64_t)&&resume_label;
    env->fault_cr2 = 0;
    env->fault_rsp = 0;

resume_label:
    return 0;
}

void kernel_longjmp(kernel_jmp_buf *env, int val) {
    uintptr_t cr2 = 0;
    uintptr_t rip = 0;
    uintptr_t rsp = 0;

    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    asm volatile("mov $1f, %0\n1:" : "=r"(rip));

    if (env) {
        env->fault_cr2 = cr2;
        env->fault_rip = rip;
        env->fault_rsp = rsp;
    }

    trap_dump_registers(rip, cr2, rsp);
    print_string("JIT: trap recovery activated: halting for serial inspection\n");

    (void)val;
    for (;;) {
        asm volatile("hlt");
    }
}

void general_protection_handler(uint64_t error_code, uint64_t fault_rip) {
    uintptr_t rsp = 0;
    uintptr_t cr2 = 0;

    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    asm volatile("mov %%cr2, %0" : "=r"(cr2));

    trap_capture_and_dump(fault_rip, cr2, rsp, error_code);
    print_string("JIT: general protection fault detected; halting\n");

    for (;;) {
        asm volatile("hlt");
    }
}
