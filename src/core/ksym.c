#include "core/ksym.h"
#include "io.h"
#include "kmalloc.h"
#include "screen.h"
#include "string.h"
#include "vmm.h"

extern const kernel_symbol_t __start_ksymtab[];
extern const kernel_symbol_t __stop_ksymtab[];

extern unsigned char inb(unsigned short port);
extern void outb(unsigned short port, unsigned char val);
extern void* kmalloc(size_t size);
extern void kfree(void* ptr);
extern void vmm_map_page(uint64_t virtual_addr, uint64_t physical_addr);
extern void* vmm_alloc_exec_pages(uint32_t num_pages);
extern void print_string(const char* str);
extern void serial_write_string(const char *str);
extern int aos_apply_network_setup(const char *request);
extern int shell_install_runtime_command(const char *name);
extern int shell_install_lisp_command(const char *definition);

EXPORT_SYMBOL(ksym_lookup);
EXPORT_SYMBOL(kmalloc_init);
EXPORT_SYMBOL(kmalloc);
EXPORT_SYMBOL(kfree);
EXPORT_SYMBOL(inb);
EXPORT_SYMBOL(outb);
EXPORT_SYMBOL(vmm_map_page);
EXPORT_SYMBOL(vmm_alloc_exec_pages);
EXPORT_SYMBOL(print_string);
EXPORT_SYMBOL(serial_write_string);
EXPORT_SYMBOL(aos_apply_network_setup);
EXPORT_SYMBOL(shell_install_runtime_command);
EXPORT_SYMBOL(shell_install_lisp_command);

uintptr_t ksym_lookup(const char *name) {
    if (!name) {
        return 0;
    }

    for (const kernel_symbol_t *sym = __start_ksymtab; sym < __stop_ksymtab; ++sym) {
        if (sym->name && strcmp(sym->name, name) == 0) {
            return sym->addr;
        }
    }

    return 0;
}
