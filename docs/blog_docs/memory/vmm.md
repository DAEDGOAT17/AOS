# Virtual Memory Manager (VMM) - 4-Level Paging

## Overview

The Virtual Memory Manager handles the translation between virtual addresses (what programs see) and physical addresses (actual RAM locations). AOS uses **4-level paging** for 64-bit address space management.

---

## Why Virtual Memory?

Virtual memory solves critical OS problems:

| Problem                 | Solution                                                |
| ----------------------- | ------------------------------------------------------- |
| **Memory isolation**    | Each process has isolated address space                 |
| **Large address space** | 48-bit virtual space (256TB) for 64-bit systems         |
| **Flexible allocation** | Physical RAM can be fragmented; virtual is contiguous   |
| **Higher-half kernel**  | Kernel mapped to high addresses separate from user code |
| **Memory protection**   | Page-level read/write/execute permissions               |

---

## Paging Overview

Instead of direct memory access, the CPU translates addresses:

```
Virtual Address (from program)
   ↓
Translation Lookaside Buffer (TLB) cache?
   ↓
Walk page tables
   ↓
Physical Address (actual RAM)
   ↓
Memory access
```

---

## 4-Level Page Table Structure

64-bit virtual addresses are divided into 5 parts:

```
Virtual Address (48-bit significant part)

63-48: Sign extension (copies bit 47)
47-39: PML4 (Page Map Level 4) index - 9 bits
38-30: PDPT (Page Directory Pointer Table) index - 9 bits
29-21: PD (Page Directory) index - 9 bits
20-12: PT (Page Table) index - 9 bits
11-0:  Offset within 4KB page - 12 bits
```

### Example Address Translation

For virtual address `0x7f8000a050`:

```
Address: 0x7f8000a050
Binary:  0000000001111111100000000000000010100000 01010000

PML4 Index:  000000001 (1)
PDPT Index:  111111100 (508)
PD Index:    000000000 (0)
PT Index:    010100000 (160)
Offset:      01010000 (80)

Translation:
PML4 table[1] → PDPT address
PDPT address[508] → PD address
PD address[0] → PT address
PT address[160] → Physical frame
Physical frame + 80 = Final physical address
```

---

## Page Structure in AOS

### Page Table Entry (PTE)

Each page table entry is 64 bits:

```
0-11:   Reserved
12-51:  Physical frame address (40 bits)
52-61:  Available (software)
62:     NX (No Execute)
63:     Bit 51 of address / various flags
```

### Page Flags

```c
#define PAGE_PRESENT          (1UL << 0)    // Page is in physical memory
#define PAGE_WRITE            (1UL << 1)    // Writable
#define PAGE_USER             (1UL << 2)    // Usermode accessible
#define PAGE_WRITETHROUGH     (1UL << 3)    // Write-through caching
#define PAGE_NOCACHE          (1UL << 4)    // No caching
#define PAGE_ACCESSED         (1UL << 5)    // CPU set on access
#define PAGE_DIRTY            (1UL << 6)    // CPU set on write
#define PAGE_PAT              (1UL << 7)    // Memory type
#define PAGE_GLOBAL           (1UL << 8)    // Global (not flushed on TLB clear)

#define PAGE_NX               (1UL << 63)   // No Execute bit

// Common combinations
#define PAGE_KERNEL           (PAGE_PRESENT | PAGE_WRITE)
#define PAGE_USER_ACCESSIBLE  (PAGE_PRESENT | PAGE_WRITE | PAGE_USER)
#define PAGE_READONLY         (PAGE_PRESENT)
```

---

## VMM Implementation

### Core Data Structures

```c
// From include/vmm.h
typedef uint64_t pte_t;                // Page table entry

typedef struct {
    pte_t entries[512];                // 512 entries per table
} __attribute__((aligned(4096))) page_table_t;

// PML4 (top level)
extern page_table_t* pml4;

// Functions
void vmm_init(void);
void vmm_map_page(uint64_t virt, uint64_t phys, uint16_t flags);
pte_t* vmm_get_pte(uint64_t virt);
void vmm_unmap_page(uint64_t virt);
void vmm_switch_page_table(page_table_t* table);
```

### Virtual Address to Page Table Walk

```c
// Get the page table entry for a virtual address
pte_t* vmm_get_pte(uint64_t virt) {
    // Extract indices from virtual address
    uint16_t pml4_idx = (virt >> 39) & 0x1FF;
    uint16_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint16_t pd_idx   = (virt >> 21) & 0x1FF;
    uint16_t pt_idx   = (virt >> 12) & 0x1FF;

    // Get PML4 entry
    pte_t pml4e = pml4->entries[pml4_idx];
    if (!(pml4e & PAGE_PRESENT)) {
        return NULL;  // PDPT not present
    }

    // Get PDPT entry
    page_table_t* pdpt = (page_table_t*)(pml4e & ~0xFFF);
    pte_t pdpte = pdpt->entries[pdpt_idx];
    if (!(pdpte & PAGE_PRESENT)) {
        return NULL;  // PD not present
    }

    // Get PD entry
    page_table_t* pd = (page_table_t*)(pdpte & ~0xFFF);
    pte_t pde = pd->entries[pd_idx];
    if (!(pde & PAGE_PRESENT)) {
        return NULL;  // PT not present
    }

    // Get PT entry
    page_table_t* pt = (page_table_t*)(pde & ~0xFFF);
    return &pt->entries[pt_idx];
}
```

### Mapping a Page

```c
void vmm_map_page(uint64_t virt, uint64_t phys, uint16_t flags) {
    uint16_t pml4_idx = (virt >> 39) & 0x1FF;
    uint16_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint16_t pd_idx   = (virt >> 21) & 0x1FF;
    uint16_t pt_idx   = (virt >> 12) & 0x1FF;

    // Get or create PDPT
    pte_t pml4e = pml4->entries[pml4_idx];
    page_table_t* pdpt;
    if (!(pml4e & PAGE_PRESENT)) {
        pdpt = (page_table_t*)pmm_alloc_page();
        memset(pdpt, 0, 4096);
        pml4->entries[pml4_idx] = (uint64_t)pdpt | flags;
    } else {
        pdpt = (page_table_t*)(pml4e & ~0xFFF);
    }

    // Get or create PD
    pte_t pdpte = pdpt->entries[pdpt_idx];
    page_table_t* pd;
    if (!(pdpte & PAGE_PRESENT)) {
        pd = (page_table_t*)pmm_alloc_page();
        memset(pd, 0, 4096);
        pdpt->entries[pdpt_idx] = (uint64_t)pd | flags;
    } else {
        pd = (page_table_t*)(pdpte & ~0xFFF);
    }

    // Get or create PT
    pte_t pde = pd->entries[pd_idx];
    page_table_t* pt;
    if (!(pde & PAGE_PRESENT)) {
        pt = (page_table_t*)pmm_alloc_page();
        memset(pt, 0, 4096);
        pd->entries[pd_idx] = (uint64_t)pt | flags;
    } else {
        pt = (page_table_t*)(pde & ~0xFFF);
    }

    // Set the final page entry
    pt->entries[pt_idx] = phys | flags;

    // Invalidate TLB entry for this address
    asm volatile ("invlpg (%0)" ::"r"(virt) : "memory");
}
```

---

## Kernel Memory Layout

AOS maps memory as:

```
Virtual Address Space:

0x0000000000000000 - 0x00007FFFFFFFFFFF    User space (optional)
0x0000800000000000 - 0xFFFF7FFFFFFFFFFF    Canonical hole (invalid)
0xFFFF800000000000 - 0xFFFFFFFFFFFFFFFF    Kernel space (higher half)
   │
   ├─ 0xFFFF800000000000 - 0xFFFF800000000000+1GB
   │  │ (Identity mapping - contains bootloader and first MB)
   │  │
   ├─ 0xFFFF800000100000 - 0xFFFF800007FFFFFF
   │  │ (Kernel code and data)
   │  │
   └─ 0xFFFF800008000000 - Higher addresses
      (Available for kernel heap, page tables, kernel stack, etc.)
```

---

## Initialization

### Step 1: Allocate PML4

```c
void vmm_init(void) {
    // Allocate PML4 (top-level page table)
    pml4 = (page_table_t*)pmm_alloc_page();
    memset(pml4, 0, 4096);

    // ... rest of initialization
}
```

### Step 2: Map Bootloader Region

The bootloader places the kernel at physical address 0x100000 (1MB). We need identity mapping (virtual = physical) for this region until paging is active.

```c
// Identity map kernel region
for (uint64_t addr = 0; addr < 0x1000000; addr += 0x1000) {
    vmm_map_page(addr, addr, PAGE_KERNEL);
}
```

### Step 3: Map Kernel to High Address

Map kernel code to the higher-half:

```c
// Map 1GB of kernel space
for (uint64_t i = 0; i < 0x40000000; i += 0x1000) {
    uint64_t virt = 0xFFFF800000000000 + i;
    uint64_t phys = i;
    vmm_map_page(virt, phys, PAGE_KERNEL);
}
```

### Step 4: Load PML4 and Enable Paging

```nasm
; From loader.s
    mov rax, pml4           ; PML4 physical address
    mov cr3, rax            ; Load page directory

    ; Enable paging
    mov rax, cr0
    or  rax, 0x80000000     ; Set PG bit
    mov cr0, rax
```

---

## Large Pages (2MB/1GB)

Instead of mapping individual 4KB pages, we can use larger pages:

- **2MB pages**: Skip page table level, saves memory
- **1GB pages**: Skip page directory too, saves more memory

### 2MB Page Mapping

```c
void vmm_map_2mb_page(uint64_t virt, uint64_t phys, uint16_t flags) {
    uint16_t pml4_idx = (virt >> 39) & 0x1FF;
    uint16_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint16_t pd_idx   = (virt >> 21) & 0x1FF;

    // ... get/create PML4 and PDPT ...

    // Set PD entry directly to physical address
    // with PAGE_SIZE bit (bit 7) to indicate 2MB page
    pde = phys | flags | 0x80;  // 0x80 = PS (Page Size) bit
    pd->entries[pd_idx] = pde;
}
```

---

## TLB (Translation Lookaside Buffer)

The TLB is CPU cache of recent translations. After modifying page tables, we must flush relevant entries:

```c
// Flush single TLB entry
static inline void tlb_flush_entry(uint64_t virt) {
    asm volatile("invlpg (%0)" ::"r"(virt) : "memory");
}

// Flush entire TLB
static inline void tlb_flush_all(void) {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %0, %%cr3" ::"r"(cr3));
}

// INVLPG is better for single pages, TLB flush all for many changes
```

---

## Memory Protection

Page table entries control access:

```c
// Read-only page (code segment)
vmm_map_page(code_addr, phys, PAGE_PRESENT);

// Readable and writable (data segment)
vmm_map_page(data_addr, phys, PAGE_PRESENT | PAGE_WRITE);

// User-accessible page
vmm_map_page(user_addr, phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);

// No-execute page
vmm_map_page(data2_addr, phys, PAGE_PRESENT | PAGE_WRITE | PAGE_NX);
```

---

## Page Faults

When the CPU accesses a virtual address and the page is not present, a page fault exception (#14) occurs.

Common causes and responses:

| Cause                | Response                                    |
| -------------------- | ------------------------------------------- |
| Page not mapped      | Map it (if lazy allocation) or kill process |
| Page swapped to disk | Load from disk (not in AOS yet)          |
| Access violation     | Kill process                                |
| Stack overflow       | Allocate more stack pages                   |

---

## Common Patterns

### Map Range of Pages

```c
void vmm_map_range(uint64_t virt_start, uint64_t phys_start,
                   uint64_t size, uint16_t flags) {
    for (uint64_t i = 0; i < size; i += 0x1000) {
        vmm_map_page(virt_start + i, phys_start + i, flags);
    }
}

// Usage: Map 64MB of video memory
vmm_map_range(VRAM_VIRT_START, VRAM_PHYS_START, 64*1024*1024,
              PAGE_KERNEL | PAGE_NOCACHE);
```

### Get Physical Address from Virtual

```c
uint64_t vmm_get_physical(uint64_t virt) {
    pte_t* pte = vmm_get_pte(virt);
    if (!pte || !(*pte & PAGE_PRESENT)) {
        return 0;  // Invalid or not mapped
    }

    // Extract physical frame number and add offset
    uint64_t offset = virt & 0xFFF;
    uint64_t frame = *pte & ~0xFFF;
    return frame + offset;
}
```

---

## Key Takeaways

✓ 4-level paging translates 48-bit virtual to physical addresses  
✓ 4KB pages are standard; 2MB/1GB pages save memory  
✓ Higher-half kernel separates kernel from user space  
✓ Page table walking requires careful NULL checking  
✓ TLB invalidation needed after page table changes  
✓ Page flags control access (read/write/execute)  
✓ Page faults handled by exception handler

---

## Related Components

- [Physical Memory Manager (PMM)](pmm.html)
- [Kernel Memory Allocator (Kmalloc)](kmalloc.html)
- [IDT Exception Handling](../interrupt_io/idt.html)
- [Kernel Initialization](../core_systems/kernel_init.html)

---

**Source Files:**

- `include/vmm.h` - VMM header
- `src/mm/vmm.c` - Paging implementation
- `src/arch/x86_64/asm/vmm_asm.s` - Assembly routines
