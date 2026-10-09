#include "pmm.h"
#include "vmm.h"
#include "gdt.h"
#include "idt.h"
#include "whisper.h"
#include "screen.h"
#include "kmalloc.h" 
#include "task.h"
#include "shell.h"
#include "ata.h"
#include "fat32.h"
#include "pci.h"
#include "timer.h"
#include "core/self_evolve.h"
// Keep kmain minimal: boot prints general info only

void kmain(uint32_t magic, multiboot_info_t* mbd) {
    // Initialize GDT FIRST before anything else
    init_gdt();
    
    // Parse Multiboot headers early to detect Framebuffer & memory
    pmm_init(magic, mbd);
    
    // Now init IDT
    init_idt();
    
    // Initialize PIT at 100 Hz (10ms per tick) BEFORE enabling interrupts.
    // This is CRITICAL for lwIP: sys_now() returns timer_get_ticks() * 10.
    // Without this, PIT defaults to ~18.2 Hz (BIOS default), making lwIP's
    // clock run ~5.5x slow and causing TCP retransmit timeouts to balloon
    // from ~1 second to ~6 seconds each — with exponential backoff = ~2 min!
    timer_init(100);
    
    // Map framebuffer and identity map everything needed
    vmm_init();
    
    // Clear screen early so we can see output
    clear_screen();
    print_string("Kernel: Init GDT OK\n");
    print_string("Kernel: PMM OK\n");
    print_string("Kernel: Init IDT OK\n");
    print_string("Kernel: VMM OK\n");

    if (whisper_model_loaded) {
        if (whisper_model_init())
            print_string("Kernel: Whisper GGML model validated\n");
        else
            print_string("Kernel: Whisper model invalid or unsupported\n");
    }
    
    kmalloc_init();
    print_string("Kernel: Kmalloc OK\n");
    
    task_init();
    print_string("Kernel: Task OK\n");
    
    // Scan for PCI Storage (SSDs)
    pci_scan_storage();

    // Enumerate audio hardware before starting the network stack.
    pci_scan_multimedia();
    
    // Scan for PCI Network Controllers and Initialize TCP/IP
    pci_scan_network();
    
    // Initialize Filesystem
    if (ata_init() == 0) {
        uint32_t offset = ata_get_partition_offset();
        if (fat32_mount(offset) == 0) {
            print_string("Kernel: FAT32 OK\n");
            
            // Initialize the Agentic Context DB
            extern void agent_init(void);
            agent_init();
            print_string("Kernel: Agent DB OK\n");
        } else {
            print_string("Kernel: FAT32 Mount Fail\n");
        }
    } else {
        print_string("Kernel: Storage detection failed\n");
    }
    
    task_create("shell", shell_task, 1);
    asm volatile("sti");
    print_string("Kernel: Interrupts enabled\n");

    // -------------------------------------------------------------------
    // CPUID detection for boot banner
    // -------------------------------------------------------------------
    uint32_t eax, ebx, ecx, edx;

    // Vendor string
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    char vendor[13];
    vendor[0]=(ebx)&0xFF;    vendor[1]=(ebx>>8)&0xFF;  vendor[2]=(ebx>>16)&0xFF; vendor[3]=(ebx>>24)&0xFF;
    vendor[4]=(edx)&0xFF;    vendor[5]=(edx>>8)&0xFF;  vendor[6]=(edx>>16)&0xFF; vendor[7]=(edx>>24)&0xFF;
    vendor[8]=(ecx)&0xFF;    vendor[9]=(ecx>>8)&0xFF;  vendor[10]=(ecx>>16)&0xFF;vendor[11]=(ecx>>24)&0xFF;
    vendor[12]='\0';

    // Feature flags (APIC)
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    int has_apic = (edx >> 9) & 1;

    // Long Mode bit
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000001));
    int has_lm = (edx >> 29) & 1;

    // Total RAM
    uint32_t total_mb = pmm_get_total_memory_kb() / 1024;

    // -------------------------------------------------------------------
    // Boot Banner
    // -------------------------------------------------------------------
    clear_screen();

    // Large AOS mark - pure 7-bit ASCII, renders on every framebuffer.
    print_color_string("   ###      #####     ######\n", MAKE_COLOR(COLOR_LIGHT_CYAN, COLOR_BLACK));
    print_color_string("  #   #    #     #   #      \n", MAKE_COLOR(COLOR_LIGHT_CYAN, COLOR_BLACK));
    print_color_string(" #     #   #     #   #      \n", MAKE_COLOR(COLOR_CYAN, COLOR_BLACK));
    print_color_string(" #######   #     #    ##### \n", MAKE_COLOR(COLOR_LIGHT_CYAN, COLOR_BLACK));
    print_color_string(" #     #   #     #         #\n", MAKE_COLOR(COLOR_LIGHT_CYAN, COLOR_BLACK));
    print_color_string(" #     #   #     #         #\n", MAKE_COLOR(COLOR_LIGHT_CYAN, COLOR_BLACK));
    print_color_string(" #     #    #####    ###### \n", MAKE_COLOR(COLOR_LIGHT_CYAN, COLOR_BLACK));
    print_char('\n');

    print_color_string("  +--------------------------------------------------+\n", MAKE_COLOR(COLOR_LIGHT_BLUE, COLOR_BLACK));
    print_color_string("  |       AOS  --  AI-Assisted Operating System      |\n", MAKE_COLOR(COLOR_YELLOW, COLOR_BLACK));
    print_color_string("  +--------------------------------------------------+\n", MAKE_COLOR(COLOR_LIGHT_BLUE, COLOR_BLACK));
    print_char('\n');

    // Architecture block
    print_color_string("  =[ ARCHITECTURE ]==============================\n", MAKE_COLOR(COLOR_LIGHT_BLUE, COLOR_BLACK));
    print_color_string("  CPU Vendor   : ", MAKE_COLOR(COLOR_LIGHT_GREY, COLOR_BLACK));
    print_color_string(vendor,               MAKE_COLOR(COLOR_WHITE,      COLOR_BLACK));
    print_char('\n');

    print_color_string("  Mode         : ", MAKE_COLOR(COLOR_LIGHT_GREY, COLOR_BLACK));
    if (has_lm) print_color_string("[OK] x86_64  64-Bit Long Mode  ACTIVE\n", MAKE_COLOR(COLOR_LIGHT_GREEN, COLOR_BLACK));
    else        print_color_string("[!!] WARNING: Long Mode NOT detected!\n",  MAKE_COLOR(COLOR_RED,          COLOR_BLACK));

    print_color_string("  Paging       : ", MAKE_COLOR(COLOR_LIGHT_GREY, COLOR_BLACK));
    print_color_string("[OK] 4-Level Paging  PML4->PDPT->PD->PT\n",            MAKE_COLOR(COLOR_LIGHT_GREEN, COLOR_BLACK));

    print_color_string("  Interrupts   : ", MAKE_COLOR(COLOR_LIGHT_GREY, COLOR_BLACK));
    if (has_apic) print_color_string("[OK] Local APIC + I/O APIC  (Modern)\n", MAKE_COLOR(COLOR_LIGHT_GREEN, COLOR_BLACK));
    else          print_color_string("[>>] Legacy 8259 PIC  (BIOS Fallback)\n", MAKE_COLOR(COLOR_YELLOW,      COLOR_BLACK));
    print_char('\n');

    // System block
    print_color_string("  =[ SYSTEM ]=====================================\n", MAKE_COLOR(COLOR_LIGHT_BLUE, COLOR_BLACK));

    print_color_string("  RAM          : ", MAKE_COLOR(COLOR_LIGHT_GREY, COLOR_BLACK));
    kprint_dec(total_mb);
    print_color_string(" MB Physical Memory\n", MAKE_COLOR(COLOR_WHITE, COLOR_BLACK));

    print_color_string("  Heap         : ", MAKE_COLOR(COLOR_LIGHT_GREY, COLOR_BLACK));
    print_color_string("[OK] Dynamic  --  Base: 0x0000000200000000\n",  MAKE_COLOR(COLOR_LIGHT_GREEN, COLOR_BLACK));

    print_color_string("  File System  : ", MAKE_COLOR(COLOR_LIGHT_GREY, COLOR_BLACK));
    if (fat32_is_mounted()) {
        if (ata_is_ramdisk())
            print_color_string("[OK] FAT32  on  RAM Disk (64 MB, volatile)\n", MAKE_COLOR(COLOR_LIGHT_GREEN, COLOR_BLACK));
        else
            print_color_string("[OK] FAT32  on  Physical Drive (MBR/Raw)\n",    MAKE_COLOR(COLOR_LIGHT_GREEN, COLOR_BLACK));
    } else {
        print_color_string("[!!] No filesystem detected  --  storage commands unavailable\n",  MAKE_COLOR(COLOR_RED,         COLOR_BLACK));
    }

    print_char('\n');
    print_color_string("  ================================================\n",  MAKE_COLOR(COLOR_LIGHT_BLUE, COLOR_BLACK));
    print_color_string("  >> Type 'help' for commands  |  'sysinfo' for full report\n", MAKE_COLOR(COLOR_LIGHT_CYAN, COLOR_BLACK));
    print_color_string("  ================================================\n",  MAKE_COLOR(COLOR_LIGHT_BLUE, COLOR_BLACK));
    print_char('\n');

    self_evolve_init();

    while (1) {
        task_yield();
    }
}