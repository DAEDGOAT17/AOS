#include "idt.h"
#include "io.h"
#include "timer.h"
#include "screen.h"
#include "acpi.h"
#include <stdint.h>
#include "vmm.h"

struct idt_entry idt[256];
struct idt_ptr idtp;

extern void idt_load(uint64_t ptr);
extern void keyboard_asm_handler();
extern void timer_asm_handler();
extern void page_fault_asm_handler();
extern void general_protection_asm_handler();
extern void dummy_exception_handler();

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    asm volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

void pic_disable() {
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

volatile uint32_t* lapic_ptr = 0;

void init_apic() {
    // 1. Find APIC Base using IA32_APIC_BASE MSR
    uint64_t apic_msr = rdmsr(0x1B);
    uint32_t apic_phys = (apic_msr & 0xFFFFF000);
    
    // 2. Map into Higher-Half space (32 GB boundary) to prevent overlaps
    uint64_t apic_virt = 0x800000000ULL;
    vmm_map_page(apic_virt, apic_phys);
    lapic_ptr = (volatile uint32_t*)apic_virt;
    
    // 3. Set Spurious Interrupt Vector to 0x1FF (enables APIC)
    lapic_ptr[0xF0 / 4] = 0x1FF;
}

void apic_eoi() {
    if (lapic_ptr) {
        lapic_ptr[0xB0 / 4] = 0;
    }
}

volatile uint32_t* ioapic_ptr = 0;

void ioapic_write(uint8_t reg, uint32_t val) {
    ioapic_ptr[0] = reg;
    ioapic_ptr[4] = val;
}

void init_ioapic() {
    uint64_t ioapic_phys = 0xFEC00000;
    uint64_t ioapic_virt = 0x800001000ULL;
    vmm_map_page(ioapic_virt, ioapic_phys);
    ioapic_ptr = (volatile uint32_t*)ioapic_virt;

    // Route IRQ1 (Keyboard) to Vector 33. Lower 32 bits = Vector, Upper = LAPIC ID 0
    ioapic_write(0x12, 33);
    ioapic_write(0x13, 0);

    // Route PIT Timer dynamically based on MADT overrides
    uint32_t timer_gsi = get_timer_gsi();
    uint32_t timer_reg_low = 0x10 + (timer_gsi * 2);
    uint32_t timer_reg_high = timer_reg_low + 1;

    ioapic_write(timer_reg_low, 32); 
    ioapic_write(timer_reg_high, 0); 
    ioapic_write(0x15, 0);
}

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low = (base & 0xFFFF);
    idt[num].sel = sel;
    idt[num].ist = 0;
    idt[num].flags = flags;
    idt[num].base_mid = (base >> 16) & 0xFFFF;
    idt[num].base_high = (base >> 32) & 0xFFFFFFFF;
    idt[num].reserved = 0;
}

void init_idt() {
    idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idtp.base = (uint64_t)&idt;

    for(int i = 0; i < 256; i++) {
        idt_set_gate(i, (uint64_t)dummy_exception_handler, 0x08, 0x8E);
    }

    idt_load((uint64_t)&idtp);

    pic_disable();
    init_apic();
    init_ioapic();
    timer_init(100);

    idt_set_gate(13, (uint64_t)general_protection_asm_handler, 0x08, 0x8E);
    idt_set_gate(14, (uint64_t)page_fault_asm_handler, 0x08, 0x8E);
    idt_set_gate(32, (uint64_t)timer_asm_handler, 0x08, 0x8E);
    idt_set_gate(33, (uint64_t)keyboard_asm_handler, 0x08, 0x8E);

    asm volatile("sti");
}
