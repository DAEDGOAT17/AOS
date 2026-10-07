#include "kmalloc.h"
#include "pmm.h"
#include "vmm.h"
#include <stdint.h>

// Heap starts at 8GB (after 4GB identity-mapped region)
#define HEAP_START 0x200000000ULL
#define KMALLOC_ALIGNMENT 16
#define KMALLOC_MAX_HEAP_SIZE (UINT32_MAX & ~((uint64_t)4095))
uint64_t dynamic_heap_size = 0;

// Block header structure
typedef struct block_header {
    uint32_t size;              // Size of block (excluding header)
    uint32_t is_free;           // 1 = free, 0 = allocated
    struct block_header* next;  // Next block in list
} block_header_t;

static block_header_t* heap_start = NULL;

void kmalloc_init() {
    uint64_t total_ram = (uint64_t)pmm_get_total_memory_kb() * 1024;
    
    // Dynamically claim all remaining RAM above 8MB, leaving an 8MB safety buffer
    if (total_ram > HEAP_START + (8 * 1024 * 1024)) {
        dynamic_heap_size = total_ram - HEAP_START - (8 * 1024 * 1024);
    } else {
        dynamic_heap_size = 16 * 1024 * 1024; // 16MB minimum fallback
    }

    if (dynamic_heap_size > KMALLOC_MAX_HEAP_SIZE)
        dynamic_heap_size = KMALLOC_MAX_HEAP_SIZE;

    // Map heap region in virtual memory
    for (uint64_t addr = HEAP_START; addr < HEAP_START + dynamic_heap_size; addr += 4096) {
        void* phys = pmm_alloc_block();
        if (phys) {
            vmm_map_page(addr, (uint64_t)phys);
        } else {
            // Out of physical RAM, cap the heap size!
            dynamic_heap_size = addr - HEAP_START;
            break;
        }
    }

    if (dynamic_heap_size < sizeof(block_header_t) + KMALLOC_ALIGNMENT) {
        heap_start = NULL;
        dynamic_heap_size = 0;
        return;
    }
    
    // Initialize first free block
    heap_start = (block_header_t*)HEAP_START;
    heap_start->size = dynamic_heap_size - sizeof(block_header_t);
    heap_start->is_free = 1;
    heap_start->next = NULL;
}

void* kmalloc(size_t size) {
    if (size == 0 || !heap_start || size > UINT32_MAX - (KMALLOC_ALIGNMENT - 1)) return NULL;
    
    size = (size + KMALLOC_ALIGNMENT - 1) & ~(size_t)(KMALLOC_ALIGNMENT - 1);
    
    block_header_t* current = heap_start;
    
    // First-fit algorithm
    while (current) {
        if (current->is_free && current->size >= size) {
            // Found suitable block
            
            // Split block if there's enough space for another block
            if (current->size >= size + sizeof(block_header_t) + 16) {
                block_header_t* new_block = (block_header_t*)((uint8_t*)current + sizeof(block_header_t) + size);
                new_block->size = current->size - size - sizeof(block_header_t);
                new_block->is_free = 1;
                new_block->next = current->next;
                
                current->size = size;
                current->next = new_block;
            }
            
            current->is_free = 0;
            return (void*)((uint8_t*)current + sizeof(block_header_t));
        }
        current = current->next;
    }
    
    return NULL;  // Out of memory
}

void kfree(void* ptr) {
    if (!ptr || !heap_start) return;
    
    block_header_t* block = heap_start;
    while (block && (uint8_t*)block + sizeof(block_header_t) != (uint8_t*)ptr)
        block = block->next;
    if (!block || block->is_free) return;

    block->is_free = 1;
    
    // Coalesce with next block if it's free
    if (block->next && block->next->is_free) {
        block->size += sizeof(block_header_t) + block->next->size;
        block->next = block->next->next;
    }
    
    // Coalesce with previous block (simple linear search)
    block_header_t* current = heap_start;
    while (current && current != block) {
        if (current->next == block) {
            if (current->is_free) {
                current->size += sizeof(block_header_t) + block->size;
                current->next = block->next;
            }
            break;
        }
        current = current->next;
    }
}

void kmalloc_get_stats(heap_stats_t* stats) {
    if (!stats) return;

    stats->total_size = dynamic_heap_size;
    stats->used_size = 0;
    stats->free_size = 0;
    stats->num_blocks = 0;
    stats->num_free_blocks = 0;
    
    block_header_t* current = heap_start;
    while (current) {
        stats->num_blocks++;
        if (current->is_free) {
            stats->num_free_blocks++;
            stats->free_size += current->size;
        } else {
            stats->used_size += current->size;
        }
        current = current->next;
    }
}