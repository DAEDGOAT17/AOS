#include "timer.h"
#include "io.h"
#include "idt.h"
#include <stdint.h>

// Tick counter - incremented by timer interrupt
static volatile uint32_t timer_ticks = 0;

// Forward declarations for network polling driven from IRQ context.
// These must be lightweight and re-entrant-safe (no dynamic allocation).
extern void rtl8169_poll(void);
extern void e1000_poll(void);
extern void hda_audio_poll(void);
extern void sys_check_timeouts(void);

// Initialize the PIT (Programmable Interval Timer)
// PIT runs at 1193182 Hz, so divisor = 1193182 / frequency
void timer_init(uint32_t frequency) {
    uint32_t divisor = 1193182 / frequency;
    
    // Send command byte: channel 0, lobyte/hibyte, mode 3 (square wave), binary
    outb(0x43, 0x36);
    
    // Send divisor (low byte then high byte)
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
    
    // Reset tick counter
    timer_ticks = 0;
}

// Get total ticks since boot
uint32_t timer_get_ticks() {
    return timer_ticks;
}

// Wait for specified number of ticks
void timer_wait(uint32_t ticks) {
    uint32_t end = timer_ticks + ticks;
    while (timer_ticks < end) {
        // Busy wait - could be improved with interrupts
        asm volatile("hlt");
    }
}

// Get uptime in hours, minutes, seconds
// Assuming 100 Hz (10ms per tick)
void timer_get_uptime(uint32_t* hours, uint32_t* minutes, uint32_t* seconds) {
    uint32_t total_seconds = timer_ticks / 100;  // 100 ticks per second
    
    *hours = total_seconds / 3600;
    *minutes = (total_seconds % 3600) / 60;
    *seconds = total_seconds % 60;
}

// -----------------------------------------------------------------------
// Network-ready flag — set to 1 by the NIC driver after lwIP is up.
// Guards against calling poll before the network stack is initialized.
// -----------------------------------------------------------------------
static volatile int net_ready = 0;

void timer_set_net_ready(void) {
    net_ready = 1;
}

// Timer interrupt handler — called every 10 ms at 100 Hz.
//
// LATENCY FIX: We drive the NIC RX poll and lwIP timeout engine directly from
// here so TCP timers fire on schedule even when the shell task is blocked
// (e.g. waiting for keyboard input or executing a slow command). Without this,
// TCP retransmit backoff could accumulate to ~2 minutes.
void timer_handler() {
    timer_ticks++;

    // Poll hardware RX ring + advance lwIP timers every tick (every 10 ms).
    // lwIP's TCP retransmit timer fires at 500 ms intervals; polling every 10 ms
    // gives us 50x the margin needed — completely eliminating missed timeouts.
    if (net_ready) {
        rtl8169_poll();
        e1000_poll();
        sys_check_timeouts();
    }
    hda_audio_poll();

    // Send EOI to interrupt controller
    apic_eoi();
}
