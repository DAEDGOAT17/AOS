# Physical Memory Manager (PMM) - Bitmap-Based Allocation

## Overview

The Physical Memory Manager (PMM) is responsible for tracking and allocating physical RAM. AOS uses a **bitmap allocation strategy** to efficiently manage memory pages.

---

## Purpose

The PMM solves these critical problems:

1. **Track Used/Free Pages** - Know which physical addresses are available
2. **Prevent Conflicts** - Ensure no two structures use the same memory
3. **Support Virtual Memory** - Provide pages for page tables and data
4. **Efficient Allocation** - Quick operations for fast boot and runtime

---

## Memory Layout After Boot

```
0x0000 - 0x1000    [Null Page + Real Mode IDT]
0x1000 - 0x80000   [Bootloader + Kernel code/data]
0x80000 - ...      [Bitmap]
... - 0xFF000000   [Available for allocation]
```

---

## Bitmap Allocation Strategy

### How It Works

Each **bit** in a bitmap represents one **4KB page**:

- Bit = 0: Page is free
- Bit = 1: Page is in use

### Memory Calculation

```
Total RAM = 4GB (32-bit addressing limit for now)
Page size = 4KB (2^12 bytes)
Total pages = 4GB / 4KB = 1,048,576 pages
Bitmap size = 1,048,576 bits / 8 = 131,072 bytes = 128KB
```

### Benefits

| Advantage           | Explanation                                |
| ------------------- | ------------------------------------------ |
| **Fast**            | Bit operations are CPU native instructions |
| **Space Efficient** | 1 byte tracks 8 pages (128KB per 4GB)      |
| **Simple**          | No complex data structures needed          |
| **Aligned**         | Works perfectly with 4KB page size         |

---

## PMM Implementation

### Core Data Structures

```c
// From include/pmm.h
typedef unsigned char byte;

extern byte* bitmap;         // Pointer to bitmap in memory
extern uint32_t bitmap_size; // Size of bitmap in bytes
extern uint32_t max_pages;   // Total pages addressable
extern uint32_t used_pages;  // Pages currently allocated
```

### Key Functions

```c
void pmm_init(uint32_t magic, multiboot_info_t* mbd);
void* pmm_alloc_page(void);
void pmm_free_page(void* page);
int pmm_test_page(uint32_t page_num);
void pmm_set_page(uint32_t page_num);
void pmm_unset_page(uint32_t page_num);
uint32_t pmm_get_total_memory_kb(void);
```

### Bitmap Bit Operations

```c
// Core bit-level operations

// Mark a page as used
void pmm_set_page(uint32_t page_num) {
    uint32_t byte_idx = page_num / 8;      // Which byte?
    uint32_t bit_idx  = page_num % 8;      // Which bit in byte?

    bitmap[byte_idx] |= (1 << bit_idx);    // Set the bit
    used_pages++;
}

// Mark a page as free
void pmm_unset_page(uint32_t page_num) {
    uint32_t byte_idx = page_num / 8;
    uint32_t bit_idx  = page_num % 8;

    bitmap[byte_idx] &= ~(1 << bit_idx);   // Clear the bit
    used_pages--;
}

// Check if page is in use
int pmm_test_page(uint32_t page_num) {
    uint32_t byte_idx = page_num / 8;
    uint32_t bit_idx  = page_num % 8;

    return (bitmap[byte_idx] >> bit_idx) & 1;
}
```

---

## Initialization Process

### Step 1: Parse Multiboot Memory Map

```c
void pmm_init(uint32_t magic, multiboot_info_t* mbd) {
    // Check magic
    if (magic != MULTIBOOT2_MAGIC) {
        return;
    }

    // Find memory map tag from bootloader
    uint32_t total_mem = 0;
    struct multiboot_tag* tag = (struct multiboot_tag*)(mbd + 8);

    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        if (tag->type == MULTIBOOT_TAG_TYPE_MMAP) {
            struct multiboot_mmap_entry* entry =
                (struct multiboot_mmap_entry*)(tag + 1);

            // Process each memory entry
            while ((void*)entry < (void*)tag + tag->size) {
                // Only track available RAM (type 1)
                if (entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
                    total_mem = max(total_mem,
                                   entry->addr + entry->len);
                }
                entry++;
            }
        }

        tag = (struct multiboot_tag*)((uint8_t*)tag +
              ((tag->size + 7) & ~7));
    }
```

### Step 2: Calculate Bitmap Size

```c
    // Calculate pages and bitmap
    max_pages = total_mem / PAGE_SIZE;      // 4KB pages
    uint32_t bitmap_bytes = max_pages / 8;

    // Place bitmap right after kernel
    extern uint32_t end;  // From linker script
    bitmap = (byte*)(&end);
```

### Step 3: Mark Reserved Regions

```c
    // Process memory map again to mark used regions
    tag = (struct multiboot_tag*)(mbd + 8);

    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        if (tag->type == MULTIBOOT_TAG_TYPE_MMAP) {
            // For non-available entries, mark as in use
            // For available entries, ensure they're free
        }
        tag = (struct multiboot_tag*)((uint8_t*)tag +
              ((tag->size + 7) & ~7));
    }
```

---

## Page Allocation

### Finding Free Pages

```c
void* pmm_alloc_page(void) {
    // Find first free bit in bitmap
    for (uint32_t byte_idx = 0; byte_idx < bitmap_size; byte_idx++) {
        if (bitmap[byte_idx] != 0xFF) {  // If not all bits set (0xFF)
            // Find first 0 bit in this byte
            for (uint8_t bit_idx = 0; bit_idx < 8; bit_idx++) {
                if (!(bitmap[byte_idx] & (1 << bit_idx))) {
                    // Found free bit!
                    uint32_t page_num = byte_idx * 8 + bit_idx;
                    pmm_set_page(page_num);

                    // Return physical address
                    return (void*)(page_num * PAGE_SIZE);
                }
            }
        }
    }

    // No free pages!
    return NULL;
}
```

### Bulk Allocation

For contiguous allocations, extended routines find consecutive free pages:

```c
void* pmm_alloc_pages(uint32_t count) {
    // Find 'count' consecutive free pages
    uint32_t found = 0;
    uint32_t start_page = 0;

    for (uint32_t i = 0; i < max_pages; i++) {
        if (!pmm_test_page(i)) {
            if (found == 0) start_page = i;
            found++;

            if (found == count) {
                // Found enough consecutive pages
                for (uint32_t j = 0; j < count; j++) {
                    pmm_set_page(start_page + j);
                }
                return (void*)(start_page * PAGE_SIZE);
            }
        } else {
            found = 0;  // Reset counter
        }
    }

    return NULL;
}
```

---

## Page Frame Allocation Flow

```
User: "Allocate a page"
   ↓
PMM_ALLOC_PAGE:
   - Scan bitmap byte by byte
   - Find first free bit
   - Mark bit as set (used)
   - Increment used_pages counter
   ↓
Return physical address to user
   ↓
User registers address in page tables
```

---

## Memory Statistics

### Checking Memory Status

```c
// Query memory usage
uint32_t used_memory = used_pages * PAGE_SIZE;
uint32_t free_memory = (max_pages - used_pages) * PAGE_SIZE;
uint32_t total_memory = max_pages * PAGE_SIZE;

// Print usage
print_string("Total: ");
print_int(total_memory / (1024*1024));
print_string(" MB\n");

print_string("Used:  ");
print_int(used_memory / (1024*1024));
print_string(" MB\n");

print_string("Free:  ");
print_int(free_memory / (1024*1024));
print_string(" MB\n");
```

### Boot Output Example

```
Kernel: PMM initialized
Total RAM: 4096 MB
Bitmap size: 128 KB
Pages: 1048576
Used: 512 pages (2.0 MB)
Free: 1048064 pages (4094.0 MB)
```

---

## Integration with Virtual Memory

PMM and VMM work together:

```
Virtual Memory Manager (VMM)
   ↓
"I need a page for page table"
   ↓
Physical Memory Manager (PMM)
   ↓
"Here's physical address 0x200000"
   ↓
VMM maps 0x200000 to virtual address 0xFFFF800000200000
   ↓
VMM uses page table to manage memory
```

---

## Performance Characteristics

| Operation           | Time Complexity | Details                           |
| ------------------- | --------------- | --------------------------------- |
| Allocate page       | O(n)            | Linear scan worst case, avg ~O(1) |
| Free page           | O(1)            | Single bit operation              |
| Test page           | O(1)            | Single bit test                   |
| Allocate contiguous | O(n²)           | Worst case scans twice            |

### Optimization: Hint Pointer

Better implementations maintain a "hint" pointer to last free page:

```c
static uint32_t last_free_page = 0;

void* pmm_alloc_page_optimized(void) {
    // Start from last successful allocation
    for (uint32_t i = last_free_page; i < max_pages; i++) {
        if (!pmm_test_page(i)) {
            pmm_set_page(i);
            last_free_page = i + 1;  // Remember for next time
            return (void*)(i * PAGE_SIZE);
        }
    }

    // Wrap around if needed
    for (uint32_t i = 0; i < last_free_page; i++) {
        if (!pmm_test_page(i)) {
            pmm_set_page(i);
            last_free_page = i + 1;
            return (void*)(i * PAGE_SIZE);
        }
    }

    return NULL;
}
```

---

## Common Patterns

### Allocate and Clear

```c
void* page = pmm_alloc_page();
if (page) {
    memset(page, 0, PAGE_SIZE);  // Clear to zeros
}
```

### Allocate Multiple Pages

```c
// Page table needs 1 page
void* pt_page = pmm_alloc_page();

// Heap needs multiple pages
void* heap_pages = pmm_alloc_pages(256);  // 1MB
```

---

## Key Takeaways

✓ Bitmap tracks used/free pages efficiently  
✓ Simple bit operations for allocation/deallocation  
✓ Calculated from bootloader memory map  
✓ Minimal overhead (128 KB for 4GB RAM)  
✓ O(1) or O(n) operations sufficient for OS  
✓ Foundation for virtual memory manager  
✓ Statistic tracking for debugging

---

## Related Components

- [Virtual Memory Manager](vmm.html)
- [Kernel Memory Allocator](kmalloc.html)
- [Bootloader & Multiboot](../architecture/bootloader.html)
- [Kernel Initialization](../core_systems/kernel_init.html)

---

**Source Files:**

- `include/pmm.h` - Header definitions
- `src/mm/pmm.c` - Implementation
