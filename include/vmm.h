#ifndef VMM_H
#define VMM_H

#include <stdint.h>

#define VMM_PRESENT  0x1
#define VMM_WRITABLE 0x2
#define VMM_USER     0x4

void vmm_init();
void vmm_map_page(uint64_t virtual_addr, uint64_t physical_addr);
int vmm_set_page_writable(uint64_t virtual_addr, int writable);
void* vmm_alloc_exec_pages(uint32_t num_pages);
void page_fault_handler(uint64_t error_code, uint64_t faulting_addr, uint64_t fault_rip);

#endif