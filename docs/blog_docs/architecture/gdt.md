# Global Descriptor Table (GDT) - Memory Segmentation in x86_64

## Overview

The Global Descriptor Table (GDT) is a fundamental data structure that defines memory segments and their protection levels in x86 and x86_64 architectures. In AOS, the GDT establishes the foundation for protected mode operation and 64-bit long mode execution.

---

## What is the GDT?

The GDT is a table in memory that contains **segment descriptors**. Each descriptor defines:

- **Base Address** - Where the segment starts in memory
- **Limit** - How large the segment can be
- **Access Rights** - Read/Write permissions and privilege levels
- **Flags** - Additional properties (32-bit, 64-bit, page granularity)

In 64-bit mode, the GDT plays a reduced role compared to 32-bit systems, but it's still mandatory.

---

## Purpose in AOS

| Purpose                     | Impact                                              |
| --------------------------- | --------------------------------------------------- |
| Establish memory protection | Prevents user programs from accessing kernel memory |
| Define privilege levels     | Separates Ring 0 (kernel) from Ring 3 (user mode)   |
| Mandatory for processor     | CPU requires GDT to function in protected mode      |
| Context switching           | Loaded when switching between kernel and user code  |

---

## GDT Structure in AOS

### Descriptor Table Definition

```c
// From include/gdt.h
struct gdt_entry {
    uint16_t limit_low;      // Lower 16 bits of limit
    uint16_t base_low;       // Lower 16 bits of base address
    uint8_t  base_middle;    // Middle 8 bits of base address
    uint8_t  access;         // Access byte (privilege, type)
    uint8_t  granularity;    // Granularity and upper limit
    uint8_t  base_high;      // Upper 8 bits of base address
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;          // Limit of the GDT itself
    uint64_t base;           // Pointer to GDT in memory
} __attribute__((packed));
```

### Access Byte Breakdown

```
Bit 7: Present (1 = segment is valid)
Bit 6: Privilege Level (0 = Ring 0, 3 = Ring 3)
Bit 5: User-defined
Bits 4-0: Type (Code, Data, etc.)
```

### Flags Byte

```
Bit 7: Granularity (0 = 1-byte, 1 = 4KB pages)
Bit 6: Size (0 = 16-bit, 1 = 32-bit)
Bit 5: Long mode (0 = 32-bit, 1 = 64-bit)
Bits 3-0: Upper limit nibble
```

---

## AOS GDT Configuration

AOS uses a minimal 3-entry GDT:

| Index | Type | Purpose              | Access | Flags         |
| ----- | ---- | -------------------- | ------ | ------------- |
| 0     | Null | Required (must be 0) | 0x00   | 0x00          |
| 1     | Code | Kernel code segment  | 0x9A   | 0x20 (64-bit) |
| 2     | Data | Kernel data segment  | 0x92   | 0x00          |

### Access Values Explained

- **Code segment (0x9A)**: `10011010`
  - Bit 7: Present (1)
  - Bit 6: Privilege Ring 0 (0)
  - Bit 4: Executable (1)
  - Bit 3: Code/Data (1)
  - Bit 1: Read/Write (1)

- **Data segment (0x92)**: `10010010`
  - Bit 7: Present (1)
  - Bit 6: Privilege Ring 0 (0)
  - Bit 0: Read/Write (1)

---

## Implementation Walkthrough

### Step 1: Define GDT and Pointer

```c
// From src/arch/x86_64/gdt.c
struct gdt_entry gdt[3];        // Our 3-entry GDT
struct gdt_ptr gp;              // Pointer for LGDT instruction
```

### Step 2: GDT Gate Setting Function

```c
void gdt_set_gate(int num, uint32_t base, uint32_t limit,
                  uint8_t access, uint8_t gran) {
    // Set base address
    gdt[num].base_low    = (base & 0xFFFF);           // Bits 0-15
    gdt[num].base_middle = (base >> 16) & 0xFF;       // Bits 16-23
    gdt[num].base_high   = (base >> 24) & 0xFF;       // Bits 24-31

    // Set limit
    gdt[num].limit_low   = (limit & 0xFFFF);          // Bits 0-15
    gdt[num].granularity = (limit >> 16) & 0x0F;      // Bits 16-19

    // Set granularity flags
    gdt[num].granularity |= gran & 0xF0;

    // Set access byte
    gdt[num].access = access;
}
```

### Step 3: Initialize GDT

```c
void init_gdt() {
    // Set GDT pointer (limit and base address)
    gp.limit = (sizeof(struct gdt_entry) * 3) - 1;
    gp.base  = (uint64_t)&gdt;

    // Entry 0: Null descriptor (required)
    gdt_set_gate(0, 0, 0, 0, 0);

    // Entry 1: Kernel code segment (64-bit)
    gdt_set_gate(1, 0, 0,
                 GDT_ACCESS_PRESENT | GDT_ACCESS_RING0 | GDT_ACCESS_CODE,
                 GDT_FLAG_64BIT);

    // Entry 2: Kernel data segment
    gdt_set_gate(2, 0, 0,
                 GDT_ACCESS_PRESENT | GDT_ACCESS_RING0 | GDT_ACCESS_DATA,
                 0);

    // Load GDT into CPU via assembly instruction
    gdt_flush((uint64_t)&gp);
}
```

### Step 4: Assembly - GDT Flush

The `gdt_flush` function is implemented in assembly (in `src/arch/x86_64/asm/loader.s`):

```nasm
extern gdt_flush

global gdt_flush:function
gdt_flush:
    mov rax, [rdi]           ; Load GDT pointer from RDI
    lgdt [rdi]               ; Load GDT Descriptor (LGDT instruction)
    mov ax, 0x10             ; Load data segment (index 2, ring 0)
    mov ds, ax               ; DS = data segment
    mov es, ax               ; ES = data segment
    mov fs, ax               ; FS = data segment
    mov gs, ax               ; GS = data segment
    mov ss, ax               ; SS = data segment
    pop rdi                  ; Get return address
    mov rax, 0x08            ; Code segment (index 1, ring 0)
    push rax                 ; Push code segment
    push rdi                 ; Push return address
    retfq                    ; Far return (updates CS)
```

---

## Memory Addresses in 64-bit Mode

In 64-bit long mode, base addresses and limits are effectively ignored:

- **Base Address**: Always 0 (flat memory address space)
- **Limit**: Effectively 2^64 (entire address space)

The GDT's primary function becomes **privilege level control** rather than memory segmentation.

---

## Why Null Descriptor at Index 0?

The null descriptor (all zeros) at index 0 is **mandatory** in x86 architectures. Attempting to use segment 0 (via setting a segment register to 0) will trigger an exception, preventing accidental access to segment 0.

---

## Privilege Levels Explained

In 32-bit protected mode, GDT entries could define:

- **Ring 0**: Kernel privilege (full access)
- **Ring 1-2**: Not typically used
- **Ring 3**: User mode privilege (restricted access)

AOS currently uses:

- **Ring 0 Code**: For kernel instructions
- **Ring 0 Data**: For kernel data access
- **No Ring 3**: User mode not yet implemented

---

## Segment Selectors

When you load a segment register (CS, DS, ES, etc.), you provide a **segment selector**:

```
15        3     2    1   0
[  Index  ] [TI] [RPL]
```

- **Index** (13 bits): Which GDT entry (0-8191)
- **TI** (1 bit): 0 = GDT, 1 = LDT (Local Descriptor Table)
- **RPL** (2 bits): Requested Privilege Level

### Examples:

- Selector `0x08` = Index 1, GDT, Ring 0 (kernel code)
- Selector `0x10` = Index 2, GDT, Ring 0 (kernel data)
- Selector `0x1B` = Index 3, GDT, Ring 3 (user code)

---

## Loading the GDT at Boot

The initialization sequence in `kmain()` demonstrates GDT loading:

```c
void kmain(uint32_t magic, multiboot_info_t* mbd) {
    // Initialize GDT FIRST before anything else
    init_gdt();

    // ... rest of initialization
}
```

**Why first?**

- Other CPU structures (IDT, TSS) depend on valid segment descriptors
- Memory protection won't work without a valid GDT
- Any CPU fault could occur without proper descriptors

---

## Advanced: Future User Mode Support

For future user mode implementation, the GDT would need:

```c
// Ring 3 Code Segment
gdt_set_gate(3, 0, 0,
             GDT_ACCESS_PRESENT | GDT_ACCESS_RING3 | GDT_ACCESS_CODE,
             GDT_FLAG_64BIT);

// Ring 3 Data Segment
gdt_set_gate(4, 0, 0,
             GDT_ACCESS_PRESENT | GDT_ACCESS_RING3 | GDT_ACCESS_DATA,
             0);

// Task State Segment (TSS) for privilege transitions
gdt_set_gate_tss(5, &tss_entry);
```

---

## Common Issues & Troubleshooting

| Issue                    | Cause                 | Solution                                |
| ------------------------ | --------------------- | --------------------------------------- |
| Triple fault immediately | Null GDT              | Verify `init_gdt()` is called first     |
| General protection fault | Invalid selector      | Check segment register values           |
| Long mode not working    | 64-bit flag not set   | Ensure `GDT_FLAG_64BIT` in granularity  |
| Wrong privilege level    | Access byte incorrect | Verify access bits match intended level |

---

## Key Takeaways

✓ GDT is mandatory for processor operation  
✓ AOS uses minimal 3-entry table  
✓ Segment descriptors define memory protection  
✓ 64-bit mode simplifies segmentation  
✓ LGDT instruction loads the GDT into CPU  
✓ Far JMP/RET instructions update CS after GDT load  
✓ Future Ring 3 support will require additional entries

---

## Related Components

- [IDT - Interrupt Descriptor Table](../interrupt_io/idt.html)
- [Long Mode Transition](long_mode.html)
- [Virtual Memory Management](../memory/vmm.html)
- [Kernel Initialization](../core_systems/kernel_init.html)

---

**Source Files:**

- `include/gdt.h` - Header definitions
- `src/arch/x86_64/gdt.c` - Implementation
- `src/arch/x86_64/asm/loader.s` - Assembly routines
