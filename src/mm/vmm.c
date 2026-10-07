#include "vmm.h"
#include "pmm.h"
#include "io.h"
#include "screen.h"
#include "arch/x86_64/trap_recovery.h"
#include <stdint.h>

extern uint64_t pml4_table[512]; // Defined in loader.s

extern void vmm_load_pml4(uint64_t phys_addr);

static uint64_t g_exec_page_base = 0x100000000ULL;

void* vmm_alloc_exec_pages(uint32_t num_pages) {
    if (num_pages == 0) {
        return (void*)0;
    }

    uint64_t virtual_addr = g_exec_page_base;
    uint64_t total_size = (uint64_t)num_pages * 4096ULL;
    g_exec_page_base += total_size;

    for (uint32_t page_index = 0; page_index < num_pages; ++page_index) {
        uint64_t physical_addr = (uint64_t)pmm_alloc_block();
        if (!physical_addr) {
            return (void*)0;
        }
        vmm_map_page(virtual_addr + (page_index * 4096ULL), physical_addr);
    }

    return (void*)virtual_addr;
}

void vmm_init() {
    extern uint32_t* fb_ptr;
    extern uint32_t fb_height;
    extern uint32_t fb_pitch;

    // 1. Map Framebuffer
    if (fb_ptr) {
        uint64_t fb_size = (uint64_t)fb_height * fb_pitch;
        fb_size = (fb_size + 4095) & ~4095;
        for (uint64_t offset = 0; offset < fb_size; offset += 4096) {
            uint64_t addr = (uint64_t)fb_ptr + offset;
            vmm_map_page(addr, addr);
        }
    }

    // 2. Map Ramdisk Module (if loaded)
    if (ramdisk_loaded && ramdisk_start != 0) {
        uint64_t rd_size = (ramdisk_size + 4095) & ~4095;
        for (uint64_t offset = 0; offset < rd_size; offset += 4096) {
            uint64_t addr = ramdisk_start + offset;
            vmm_map_page(addr, addr);
        }
    }

    if (whisper_model_loaded && whisper_model_end > whisper_model_start) {
        uint64_t model_start = whisper_model_start & ~4095ULL;
        uint64_t model_end = (whisper_model_end + 4095) & ~4095ULL;
        for (uint64_t addr = model_start; addr < model_end; addr += 4096)
            vmm_map_page(addr, addr);
    }

    // 3. Identity map first 16MB to ensure kernel and page tables are available
    for (uint64_t addr = 0; addr < 16 * 1024 * 1024; addr += 4096) {
        vmm_map_page(addr, addr);
    }

    vmm_load_pml4((uint64_t)pml4_table);
}

void vmm_map_page(uint64_t virtual_addr, uint64_t physical_addr) {
    uint64_t pml4_idx = (virtual_addr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virtual_addr >> 30) & 0x1FF;
    uint64_t pd_idx   = (virtual_addr >> 21) & 0x1FF;
    uint64_t pt_idx   = (virtual_addr >> 12) & 0x1FF;

    if (!(pml4_table[pml4_idx] & VMM_PRESENT)) {
        uint64_t* new_pdpt = (uint64_t*)pmm_alloc_block();
        if(!new_pdpt) return;
        for(int i=0; i<512; i++) new_pdpt[i] = 0;
        pml4_table[pml4_idx] = ((uint64_t)new_pdpt) | (VMM_PRESENT | VMM_WRITABLE);
    }

    uint64_t* pdpt = (uint64_t*)(pml4_table[pml4_idx] & ~0xFFFULL);
    if (!(pdpt[pdpt_idx] & VMM_PRESENT)) {
        uint64_t* new_pd = (uint64_t*)pmm_alloc_block();
        if(!new_pd) return;
        for(int i=0; i<512; i++) new_pd[i] = 0;
        pdpt[pdpt_idx] = ((uint64_t)new_pd) | (VMM_PRESENT | VMM_WRITABLE);
    }

    uint64_t* pd = (uint64_t*)(pdpt[pdpt_idx] & ~0xFFFULL);
    if ((pd[pd_idx] & VMM_PRESENT) && (pd[pd_idx] & 0x80)) {
        return; // Huge page, don't overwrite
    } else if (!(pd[pd_idx] & VMM_PRESENT)) {
        uint64_t* new_pt = (uint64_t*)pmm_alloc_block();
        if(!new_pt) return;
        for(int i=0; i<512; i++) new_pt[i] = 0;
        pd[pd_idx] = ((uint64_t)new_pt) | (VMM_PRESENT | VMM_WRITABLE);
    }

    uint64_t* pt = (uint64_t*)(pd[pd_idx] & ~0xFFFULL);
    pt[pt_idx] = (physical_addr & ~0xFFFULL) | (VMM_PRESENT | VMM_WRITABLE);

    asm volatile("invlpg (%0)" : : "r"(virtual_addr) : "memory");
}

int vmm_set_page_writable(uint64_t virtual_addr, int writable) {
    uint64_t pml4_idx = (virtual_addr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virtual_addr >> 30) & 0x1FF;
    uint64_t pd_idx = (virtual_addr >> 21) & 0x1FF;
    uint64_t pt_idx = (virtual_addr >> 12) & 0x1FF;
    uint64_t *pdpt;
    uint64_t *pd;
    uint64_t *pt;
    uint64_t *pte;
    uint64_t cr0;

    if (!(pml4_table[pml4_idx] & VMM_PRESENT)) {
        return 0;
    }
    pdpt = (uint64_t *)(pml4_table[pml4_idx] & ~0xFFFULL);
    if (!(pdpt[pdpt_idx] & VMM_PRESENT)) {
        return 0;
    }
    pd = (uint64_t *)(pdpt[pdpt_idx] & ~0xFFFULL);
    if (!(pd[pd_idx] & VMM_PRESENT) || (pd[pd_idx] & 0x80)) {
        return 0;
    }
    pt = (uint64_t *)(pd[pd_idx] & ~0xFFFULL);
    pte = &pt[pt_idx];
    if (!(*pte & VMM_PRESENT)) {
        return 0;
    }

    if (writable) {
        *pte |= VMM_WRITABLE;
    } else {
        asm volatile("mov %%cr0, %0" : "=r"(cr0));
        cr0 |= 1ULL << 16;
        asm volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
        *pte &= ~((uint64_t)VMM_WRITABLE);
    }

    virtual_addr &= ~0xFFFULL;
    asm volatile("invlpg (%0)" : : "r"(virtual_addr) : "memory");
    return 1;
}

void page_fault_handler(uint64_t error_code, uint64_t faulting_addr, uint64_t fault_rip) {
    uintptr_t rsp = 0;

    asm volatile("cli");
    asm volatile("mov %%rsp, %0" : "=r"(rsp));

    print_string("\n\n\n");
    print_string("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    print_string("          KERNEL PANIC: PAGE FAULT               \n");
    print_string("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n\n");

    trap_capture_and_dump(fault_rip, faulting_addr, rsp, error_code);

    print_string("Faulting Address: 0x");
    kprint_hex((uint32_t)(faulting_addr >> 32));
    kprint_hex((uint32_t)faulting_addr);
    print_string("\n");

    print_string("Error Code: 0x");
    kprint_hex((uint32_t)error_code);
    print_string(" (");

    int present  = !(error_code & 0x1);
    int rw       = error_code & 0x2;
    int us       = error_code & 0x4;
    int reserved = error_code & 0x8;
    int id       = error_code & 0x10;

    if (present)  print_string("not-present ");
    if (rw)       print_string("read-only ");
    if (us)       print_string("user-mode ");
    if (reserved) print_string("reserved ");
    if (id)       print_string("instruction-fetch ");
    print_string(")\n\n");

    print_string("JIT: page fault captured, serial dump emitted, halting\n");
    for (;;) {
        asm volatile("hlt");
    }
}