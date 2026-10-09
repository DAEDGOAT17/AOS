# Interrupt Descriptor Table (IDT) - Exception and Interrupt Handling

## Overview

The Interrupt Descriptor Table (IDT) is a critical CPU data structure that maps interrupt numbers to handler functions. When a CPU exception occurs or external hardware signals an interrupt, the processor uses the IDT to determine which handler code to execute.

---

## What is the IDT?

The IDT is similar to the GDT but for interrupts:

- **256 possible entries** (0-255)
- **Entry 0-31**: CPU exceptions (division by zero, page fault, etc.)
- **Entry 32-47**: Hardware interrupts (timer, keyboard, disk)
- **Entry 48-255**: Software interrupts and reserved

Each entry points to an **interrupt handler** function.

---

## Purpose in AOS

| Purpose                       | Benefit                                    |
| ----------------------------- | ------------------------------------------ |
| **Handle Exceptions**         | Catch CPU errors (faults, traps, aborts)   |
| **Route Hardware Interrupts** | Keyboard, timer, disk I/O signals          |
| **Multitasking Foundation**   | Timer interrupts trigger context switches  |
| **Error Recovery**            | Graceful handling instead of triple faults |

---

## IDT Entry Structure

### Gate Descriptor (64-bit)

In 64-bit mode, each IDT entry is 16 bytes:

```
    0-15:   Handler offset (bits 0-15)
   16-31:   Selector (segment, usually kernel code)
   32-34:   IST (Interrupt Stack Table), 0 for kernel
   35:      Reserved (0)
   36-39:   Gate type + flags
   40-63:   Handler offset (bits 16-63)
   64-127:  Handler offset (bits 64-95) + reserved
```

### Gate Type and Flags

```
Bits 36-39: Type
  0x0E = Interrupt Gate (disables interrupts)
  0x0F = Trap Gate (keeps interrupts enabled)

Bit 39: Present (1 = valid entry)
Bits 45-46: Privilege level (0 = Ring 0, 3 = Ring 3)
```

---

## AOS IDT Implementation

### IDT Data Structure

```c
// From include/idt.h
typedef struct {
    uint16_t offset_low;      // Handler address bits 0-15
    uint16_t selector;        // Code segment selector (kernel)
    uint8_t  ist;             // IST (0 for kernel)
    uint8_t  flags;           // Type and attributes
    uint16_t offset_mid;      // Handler address bits 16-31
    uint32_t offset_high;     // Handler address bits 32-63
    uint32_t reserved;        // Reserved (must be 0)
} __attribute__((packed)) idt_entry_t;

// IDT Pointer for LIDT instruction
typedef struct {
    uint16_t limit;           // IDT size - 1
    uint64_t base;            // IDT base address
} __attribute__((packed)) idt_ptr_t;

// The actual IDT
idt_entry_t idt[256];
idt_ptr_t   idtp;
```

### Setting IDT Gates

```c
void idt_set_gate(uint8_t num, uint64_t handler, uint16_t selector,
                  uint8_t flags) {
    // Extract address parts
    idt[num].offset_low  = handler & 0xFFFF;
    idt[num].offset_mid  = (handler >> 16) & 0xFFFF;
    idt[num].offset_high = (handler >> 32) & 0xFFFFFFFF;

    // Set selector and flags
    idt[num].selector = selector;  // Kernel code segment
    idt[num].flags = flags;
    idt[num].ist = 0;              // No separate stack
}
```

### IDT Initialization

```c
void init_idt() {
    // Set IDT pointer
    idtp.limit = (sizeof(idt_entry_t) * 256) - 1;
    idtp.base  = (uint64_t)&idt;

    // Install CPU exception handlers
    idt_set_gate(0,  isr0,  0x08, 0x8E);    // Division by zero
    idt_set_gate(1,  isr1,  0x08, 0x8E);    // Debug
    idt_set_gate(2,  isr2,  0x08, 0x8E);    // NMI
    idt_set_gate(3,  isr3,  0x08, 0x8E);    // Breakpoint
    idt_set_gate(4,  isr4,  0x08, 0x8E);    // Overflow
    // ... more exception handlers (14, 17, 30 for common ones)

    // Install hardware interrupt handlers
    idt_set_gate(32, irq0,  0x08, 0x8E);    // Timer
    idt_set_gate(33, irq1,  0x08, 0x8E);    // Keyboard
    idt_set_gate(34, irq2,  0x08, 0x8E);    // Cascade
    // ... more IRQ handlers

    // Load IDT into CPU
    idt_flush((uint64_t)&idtp);
}
```

---

## CPU Exception Handlers

### Exception Numbers 0-31

| #     | Name                | Cause                        | Action    |
| ----- | ------------------- | ---------------------------- | --------- |
| 0     | Divide Error        | Division by zero             | Fault     |
| 1     | Debug               | Debug trap                   | Trap      |
| 2     | NMI                 | Non-maskable interrupt       | Interrupt |
| 3     | Breakpoint          | INT 3 instruction            | Trap      |
| 4     | Overflow            | INTO on overflow             | Trap      |
| 5     | BOUND Range         | BOUND instruction            | Fault     |
| 6     | Invalid Opcode      | Invalid instruction          | Fault     |
| 7     | Device Not Avail    | FPU not available            | Fault     |
| 8     | Double Fault        | Multiple exceptions          | Abort     |
| 9     | Coproc Seg Overrun  | Reserved                     | Abort     |
| 10    | Invalid TSS         | Invalid TSS                  | Fault     |
| 11    | Segment Not Present | Segment not in memory        | Fault     |
| 12    | Stack Fault         | Stack segment error          | Fault     |
| 13    | General Protection  | General protection violation | Fault     |
| 14    | Page Fault          | Page not in memory           | Fault     |
| 16    | Floating Exception  | FPU error                    | Fault     |
| 17    | Alignment Check     | Unaligned memory access      | Fault     |
| 18    | Machine Check       | Hardware error               | Abort     |
| 19-31 | Reserved            | -                            | -         |

---

## Hardware Interrupt Handlers (IRQs)

After PIC/APIC remapping, hardware interrupts 32-47 map as:

| IRQ | Source                | Handler |
| --- | --------------------- | ------- |
| 0   | Timer (PIT/APIC)      | irq0    |
| 1   | Keyboard              | irq1    |
| 2   | Cascade (reserved)    | irq2    |
| 3   | Serial Port 2         | irq3    |
| 4   | Serial Port 1         | irq4    |
| 5   | Parallel Port / Sound | irq5    |
| 6   | Floppy Disk           | irq6    |
| 7   | Parallel Port         | irq7    |
| 8   | Real-Time Clock       | irq8    |
| 9   | Redirected            | irq9    |
| 10  | Reserved              | irq10   |
| 11  | Reserved              | irq11   |
| 12  | Mouse                 | irq12   |
| 13  | Math Coprocessor      | irq13   |
| 14  | ATA Primary           | irq14   |
| 15  | ATA Secondary         | irq15   |

---

## Assembly Exception Handlers

Handlers are written in assembly to:

1. Save CPU state
2. Call C handler function
3. Restore CPU state
4. Return from interrupt

### Example: Page Fault Handler (IRQ 14)

```nasm
extern page_fault_handler
global isr14

isr14:
    ; Error code already pushed by CPU
    push rbp
    mov rbp, rsp

    ; Save all registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi

    ; Call C handler
    mov rdi, [rbp + 8]  ; Error code
    mov rsi, cr2        ; Faulting address (in CR2)
    call page_fault_handler

    ; Restore registers
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    pop rbp
    iretq               ; Return from interrupt
```

---

## Interrupt Handler Stubs

In `src/arch/x86_64/asm/interrupts.s`, there are stub handlers for each ISR/IRQ:

```nasm
; Handlers that push an error code automatically
extern isr_handler
global isr0
... (more isrs)

isr0:
    push 0              ; Push dummy error code
    push 0              ; Push ISR number
    jmp isr_common

isr1:
    push 0
    push 1
    jmp isr_common

; ... repeated for all 256...

isr_common:
    ; Save all registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp

    ; Call C handler
    mov rdi, rsp        ; Pointer to interrupt frame
    call isr_handler

    ; Restore registers
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16         ; Remove ISR number and error code
    iretq
```

---

## C Interrupt Handler

A central C function processes all interrupts:

```c
extern void isr_handler(struct interrupt_frame* frame) {
    uint8_t int_num = frame->isr_num;

    // Dispatch to appropriate handler
    switch (int_num) {
    case 13:  // General Protection Fault
        handle_general_protection_fault(frame);
        break;

    case 14:  // Page Fault
        handle_page_fault(frame);
        break;

    case 32:  // Timer
        handle_timer_interrupt();
        break;

    case 33:  // Keyboard
        handle_keyboard_interrupt();
        break;

    default:
        // Generic exception handler
        handle_unknown_interrupt(int_num);
        break;
    }
}

struct interrupt_frame {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t isr_num;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};
```

---

## Loading the IDT

The LIDT instruction loads the IDT:

```nasm
global idt_flush:function
idt_flush:
    mov rax, [rdi]       ; Load IDT pointer from parameter
    lidt [rdi]           ; Load IDT (LIDT instruction)
    ret
```

---

## Interrupt Flow

```
1. Hardware Event or CPU Exception
   ↓
2. CPU looks up IDT entry (based on vector number)
   ↓
3. CPU pushes registers and return address
   ↓
4. CPU jumps to handler address from IDT
   ↓
5. Interrupt stub handler (assembly)
   ↓
6. C interrupt dispatcher (isr_handler)
   ↓
7. Specific handler (page_fault_handler, etc.)
   ↓
8. Handler returns
   ↓
9. Assembly restores registers
   ↓
10. IRETQ returns to original code
   ↓
11. Execution continues normally
```

---

## Enabling/Disabling Interrupts

```c
// Enable interrupts (set IF flag in RFLAGS)
static inline void enable_interrupts(void) {
    asm volatile("sti");
}

// Disable interrupts (clear IF flag)
static inline void disable_interrupts(void) {
    asm volatile("cli");
}

// Save state and disable
static inline uint64_t save_interrupts(void) {
    uint64_t flags;
    asm volatile("pushfq; popq %0" : "=r"(flags));
    disable_interrupts();
    return flags;
}

// Restore interrupt state
static inline void restore_interrupts(uint64_t flags) {
    if (flags & (1 << 9)) {  // IF flag is bit 9
        enable_interrupts();
    }
}
```

---

## Exception vs Trap vs Interrupt

| Type          | Characteristics                                         |
| ------------- | ------------------------------------------------------- |
| **Fault**     | Correction possible; IP points to offending instruction |
| **Trap**      | For debugging; IP points after offending instruction    |
| **Abort**     | Unrecoverable; halts the processor                      |
| **Interrupt** | External signal; IP points to next instruction          |

---

## Common Issues and Debugging

| Problem               | Cause                    | Solution                          |
| --------------------- | ------------------------ | --------------------------------- |
| Triple fault          | Invalid IDT              | Verify LIDT executed, entries set |
| Wrong handler called  | IDT entries corrupted    | Check initialization order        |
| Interrupts not firing | IDT not loaded           | Verify LIDT instruction called    |
| Page fault loops      | Handler not fixing issue | Check VMM integration             |

---

## Key Takeaways

✓ IDT maps 256 interrupt vectors to handlers  
✓ First 32 entries for CPU exceptions  
✓ Entries 32-47 for hardware interrupts  
✓ LIDT loads IDT into CPU  
✓ Assembly stubs coordinate with C handlers  
✓ Interrupt frame contains register state  
✓ FLAGS register controls masking

---

## Related Components

- [APIC/IO-APIC Interrupt Controller](apic.html)
- [Timer Subsystem](timer.html)
- [Global Descriptor Table (GDT)](../architecture/gdt.html)
- [Keyboard Driver](../drivers/keyboard_driver.html)

---

**Source Files:**

- `include/idt.h` - IDT definitions
- `src/arch/x86_64/idt.c` - IDT initialization
- `src/arch/x86_64/asm/interrupts.s` - Assembly handlers
