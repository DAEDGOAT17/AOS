#include "rtl8169.h"
#include "pci.h"
#include "io.h"
#include "screen.h"
#include "kmalloc.h"
#include "net.h"
#include "timer.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/pbuf.h"
#include "lwip/dhcp.h"
#include "lwip/ip4_addr.h"

static void rtl8169_netif_status_cb(struct netif *netif) {
    print_string("\n[RTL8169] Network initialized! IP Address: ");
    print_string(ip4addr_ntoa(netif_ip4_addr(netif)));
    print_string("\nJARVIS [/] $ ");
}

// Explicit cache line flusher to push CPU writes to main memory for DMA safely.
static inline void flush_cache_line(volatile void *p) {
    asm volatile("clflush (%0)" :: "r"(p) : "memory");
}

// Track the memory mapped I/O base
static uint8_t *mmio_base = 0;
static int rtl8169_initialized;

// Descriptor Rings — MUST be static BSS, NOT kmalloc'd!
// kmalloc uses virtual 0x200000000 mapped to arbitrary physical pages.
// The NIC DMA engine needs PHYSICAL addresses. Static BSS is identity-mapped
// (virtual == physical), so these addresses are safe to hand to the hardware.
#define NUM_DESCS 64
static rtl_desc_t rx_ring[NUM_DESCS] __attribute__((aligned(256)));
static rtl_desc_t tx_ring[NUM_DESCS] __attribute__((aligned(256)));

// Receive/Transmit buffer memory (also in BSS, also identity-mapped)
static uint8_t rx_buffers[NUM_DESCS][1536] __attribute__((aligned(256)));
static uint8_t tx_buffers[NUM_DESCS][1536] __attribute__((aligned(256)));

// Direct MMIO Reading and Writing
static void rtl_outb(uint16_t reg, uint8_t val) {
    if (mmio_base) *(volatile uint8_t *)(mmio_base + reg) = val;
}

static uint8_t rtl_inb(uint16_t reg) {
    return mmio_base ? *(volatile uint8_t *)(mmio_base + reg) : 0;
}

static void rtl_outd(uint16_t reg, uint32_t val) {
    if (mmio_base) *(volatile uint32_t *)(mmio_base + reg) = val;
}

static uint32_t rtl_ind(uint16_t reg) {
    if (!mmio_base) return 0;
    return *(volatile uint32_t *)(mmio_base + reg);
}

// Write to the physical PHY over MDIO (to wake it up from ALDPS sleep)
static void rtl_mdio_write(uint8_t reg, uint16_t val) {
    uint32_t cmd = 0x80000000 | ((reg & 0x1F) << 16) | (val & 0xFFFF);
    rtl_outd(0x60, cmd); // 0x60 is PHYAR
    
    // Poll bit 31 until it clears (Write Complete)
    for (int i = 0; i < 100000; i++) {
        if (!(rtl_ind(0x60) & 0x80000000)) break;
    }
}

static struct netif rtl_netif;
static int tx_idx = 0;

uint32_t rtl8169_tx_packets = 0;
uint32_t rtl8169_rx_packets = 0;
uint32_t rtl8169_tx_bytes = 0;
uint32_t rtl8169_rx_bytes = 0;

uint8_t rtl8169_last_tx_packet[1536];
uint16_t rtl8169_last_tx_len = 0;

uint8_t rtl8169_last_rx_packet[1536];
uint16_t rtl8169_last_rx_len = 0;

int rtl8169_live_pktdump = 0;

void rtl8169_print_packet_parsed(uint8_t* data, int len) {
    char* hex = "0123456789ABCDEF";
    if (len >= 14) {
        uint8_t* dest_mac = &data[0];
        uint8_t* src_mac = &data[6];
        uint16_t eth_type = (data[12] << 8) | data[13];
        
        set_text_color(MAKE_COLOR(COLOR_LIGHT_MAGENTA, COLOR_BLACK));
        print_string("  Eth: [");
        for (int i=0; i<6; i++) { print_char(hex[(src_mac[i]>>4)&0xF]); print_char(hex[src_mac[i]&0xF]); if(i<5)print_char(':'); }
        print_string("] -> [");
        for (int i=0; i<6; i++) { print_char(hex[(dest_mac[i]>>4)&0xF]); print_char(hex[dest_mac[i]&0xF]); if(i<5)print_char(':'); }
        print_string("] Type: 0x");
        print_char(hex[(eth_type>>12)&0xF]); print_char(hex[(eth_type>>8)&0xF]);
        print_char(hex[(eth_type>>4)&0xF]); print_char(hex[eth_type&0xF]);
        print_string("\n");
        reset_text_color();

        if (eth_type == 0x0800 && len >= 34) { // IPv4
            uint8_t* ip_hdr = &data[14];
            uint8_t proto = ip_hdr[9];
            uint8_t* src_ip = &ip_hdr[12];
            uint8_t* dest_ip = &ip_hdr[16];
            set_text_color(MAKE_COLOR(COLOR_LIGHT_BLUE, COLOR_BLACK));
            print_string("  IPv4: ");
            kprint_dec(src_ip[0]); print_char('.'); kprint_dec(src_ip[1]); print_char('.'); kprint_dec(src_ip[2]); print_char('.'); kprint_dec(src_ip[3]);
            print_string(" -> ");
            kprint_dec(dest_ip[0]); print_char('.'); kprint_dec(dest_ip[1]); print_char('.'); kprint_dec(dest_ip[2]); print_char('.'); kprint_dec(dest_ip[3]);
            
            print_string("  Proto: ");
            if (proto == 1) print_string("ICMP");
            else if (proto == 6) print_string("TCP");
            else if (proto == 17) print_string("UDP");
            else kprint_dec(proto);
            
            if (proto == 6 && len >= 54) { // TCP
                uint16_t src_port = (data[34] << 8) | data[35];
                uint16_t dest_port = (data[36] << 8) | data[37];
                print_string("  Ports: "); kprint_dec(src_port); print_string(" -> "); kprint_dec(dest_port);
            } else if (proto == 17 && len >= 42) { // UDP
                uint16_t src_port = (data[34] << 8) | data[35];
                uint16_t dest_port = (data[36] << 8) | data[37];
                print_string("  Ports: "); kprint_dec(src_port); print_string(" -> "); kprint_dec(dest_port);
            }
            print_string("\n");
            reset_text_color();
        } else if (eth_type == 0x0806 && len >= 42) { // ARP
            uint8_t* arp_hdr = &data[14];
            uint16_t op = (arp_hdr[6] << 8) | arp_hdr[7];
            uint8_t* sender_ip = &arp_hdr[14];
            uint8_t* target_ip = &arp_hdr[24];
            set_text_color(MAKE_COLOR(COLOR_YELLOW, COLOR_BLACK));
            print_string("  ARP: ");
            if (op == 1) print_string("Request who-has ");
            else if (op == 2) print_string("Reply ");
            else print_string("Unknown ");
            kprint_dec(target_ip[0]); print_char('.'); kprint_dec(target_ip[1]); print_char('.'); kprint_dec(target_ip[2]); print_char('.'); kprint_dec(target_ip[3]);
            if (op == 1) {
                print_string(" tell ");
                kprint_dec(sender_ip[0]); print_char('.'); kprint_dec(sender_ip[1]); print_char('.'); kprint_dec(sender_ip[2]); print_char('.'); kprint_dec(sender_ip[3]);
            }
            print_string("\n");
            reset_text_color();
        }
    }
}

void rtl8169_dump_packet(const char* prefix, uint8_t* data, int len) {
    if (!rtl8169_live_pktdump) return;
    set_text_color(MAKE_COLOR(COLOR_LIGHT_CYAN, COLOR_BLACK));
    print_string(prefix);
    print_string(" LIVE DUMP (");
    kprint_dec(len);
    print_string(" bytes):\n");
    print_string("------------------------------------------------\n");
    reset_text_color();
    rtl8169_print_packet_parsed(data, len);

    char* hex = "0123456789ABCDEF";
    for (int i = 0; i < len; i++) {
        print_char(hex[(data[i] >> 4) & 0xF]);
        print_char(hex[data[i] & 0xF]);
        print_char(' ');
        if ((i + 1) % 16 == 0) print_char('\n');
    }
    if (len % 16 != 0) print_char('\n');
    print_char('\n');
}

err_t rtl8169_linkoutput(struct netif *netif, struct pbuf *p) {
    (void)netif;
    
    // Copy lwIP packet buffer into flat memory for hardware DMA
    uint8_t* flat_buf = tx_buffers[tx_idx];
    pbuf_copy_partial(p, flat_buf, p->tot_len, 0);
    
    int tx_len = p->tot_len;
    // 802.3 Ethernet Spec: Frames under 60 bytes are illegal "RUNT" frames. 
    // Physical hardware blocks and drops them! We MUST pad them with zeroes!
    if (tx_len < 60) {
        for (int i = tx_len; i < 60; i++) {
            flat_buf[i] = 0;
        }
        tx_len = 60;
    }
    
    // Ensure CPU is looking at the actual memory to check OWN bit
    flush_cache_line(&tx_ring[tx_idx]);
    asm volatile("mfence" ::: "memory");
    
    if (tx_ring[tx_idx].command & 0x80000000) {
        // Hardware still owns this descriptor (Ring Full)
        return ERR_BUF;
    }

    
    rtl8169_last_tx_len = tx_len;
    for (int i = 0; i < tx_len; i++) {
        rtl8169_last_tx_packet[i] = flat_buf[i];
    }
    
    rtl8169_dump_packet("RTL8169 TX", flat_buf, tx_len);
    
    // Assign to physical transmit ring location (64-bit DMA)
    tx_ring[tx_idx].buf_low = (uint32_t)((uint64_t)flat_buf & 0xFFFFFFFF);
    tx_ring[tx_idx].buf_high = (uint32_t)((uint64_t)flat_buf >> 32);
    
    // Flush the packet data out of CPU cache into Physical RAM (64 bytes per cache line)
    // We flush up to tx_len to ensure the padded zeros are also sent to RAM!
    for (int i = 0; i < tx_len + 64; i += 64) {
        flush_cache_line(&flat_buf[i]);
    }
    
    // Setting OWN bit (Hardware assumes control), First Segment, Last Segment, and Length
    uint32_t cmd = 0x80000000 | 0x20000000 | 0x10000000 | (tx_len & 0xFFFF);
    if (tx_idx == NUM_DESCS - 1) cmd |= 0x40000000; // End of Ring wrap marker
    
    tx_ring[tx_idx].command = cmd;
    
    // FLUSH THE DESCRIPTOR COMMAND so the Network Card can see the OWN bit!
    flush_cache_line(&tx_ring[tx_idx]);
    
    // Ensure all memory writes and cache flushes hit RAM before we ring the doorbell!
    asm volatile("mfence" ::: "memory");
    
    // Write 0x40 to TPPOLL to tell hardware memory is ready
    rtl_outb(RTL_TPPOLL, 0x40);
    
    rtl8169_tx_packets++;
    rtl8169_tx_bytes += p->tot_len;
    
    tx_idx = (tx_idx + 1) % NUM_DESCS;
    return ERR_OK;
}

static int rx_idx = 0;
void rtl8169_poll(void) {
    
    // Process all available packets in the ring
    while (1) {
        // Force the CPU to fetch from RAM instead of L1 Cache before reading the descriptor!
        flush_cache_line(&rx_ring[rx_idx]);
        asm volatile("mfence" ::: "memory");
        
        uint32_t cmd = rx_ring[rx_idx].command;
        if (cmd & RTL_DESC_OWN) {
            break; // Hardware still owns this descriptor
        }

        int len = cmd & 0x3FFF; // Extract packet length from lower 14 bits
        
        if (len > 0 && len <= 1536) {
            rtl8169_rx_packets++;
            rtl8169_rx_bytes += len;
            
            // Invalidate the CPU cache for the incoming packet payload! 
            // The hardware wrote to RAM via DMA, the CPU has stale zeroes in cache!
            for (int i = 0; i < len; i += 64) {
                flush_cache_line(&rx_buffers[rx_idx][i]);
            }
            
            rtl8169_last_rx_len = len;
            for (int i = 0; i < len; i++) {
                rtl8169_last_rx_packet[i] = rx_buffers[rx_idx][i];
            }
            
            rtl8169_dump_packet("RTL8169 RX", rx_buffers[rx_idx], len);
            
            // Allocate a packet buffer for lwIP
            struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
            if (p != NULL) {
                pbuf_take(p, rx_buffers[rx_idx], len);
                
                // Inject the packet directly into the lwIP network stack!
                if (rtl_netif.input(p, &rtl_netif) != ERR_OK) {
                    pbuf_free(p);
                }
            }
        }
        
        // Reset the descriptor and give ownership back to the Hardware
        uint32_t reset_cmd = RTL_DESC_OWN | 1536;
        if (rx_idx == NUM_DESCS - 1) reset_cmd |= 0x40000000; // Ring wrap marker
        rx_ring[rx_idx].command = reset_cmd;
        flush_cache_line(&rx_ring[rx_idx]);
        asm volatile("mfence" ::: "memory");
        
        rx_idx = (rx_idx + 1) % NUM_DESCS;
    }
}

err_t rtl8169_netif_init(struct netif *netif) {
    netif->name[0] = 'r';
    netif->name[1] = 't';
    netif->linkoutput = rtl8169_linkoutput;     // Bind local transmission to lwIP
    netif->output = etharp_output;              // Use standard ARP resolver for IPv4
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    
    // Pull the real hardware MAC Address directly from config registers
    for (int i=0; i<6; i++) {
        netif->hwaddr[i] = rtl_inb(RTL_MAC0 + i);
    }
    netif->hwaddr_len = 6;
    return ERR_OK;
}

int rtl8169_init(uint32_t bus, uint32_t device, uint32_t function) {
    if (rtl8169_initialized) {
        print_string("RTL8169: Already initialized. Skipping to avoid lwIP crashes.\n");
        return 0;
    }

    print_string("RTL8169: Initializing hardware...\n");

    // CRITICAL: Enable PCI Bus Mastering (bit 2) AND Memory Space (bit 1).
    // Bit 1 = Memory Space Enable: lets the CPU read/write MMIO BARs.
    // Bit 2 = Bus Master Enable: lets the NIC do DMA to RAM.
    // Without BOTH bits, nothing works!
    uint32_t pci_cmd = pci_read_config_dword(bus, device, function, 0x04);
    if ((pci_cmd & 0x0006) != 0x0006) {
        print_string("RTL8169: Enabling PCI Memory Space + Bus Mastering...\n");
        pci_cmd |= 0x0006; // bit1=MemSpace, bit2=BusMaster
        pci_write_config_dword(bus, device, function, 0x04, pci_cmd);
    }
    
    // Scan PCI BARs (offsets 0x10-0x24) to find the MMIO register space.
    // RTL8169 uses BAR1 (offset 0x14) for MMIO.
    // RTL8168/RTL8111 uses BAR2 (offset 0x18) — BAR1 on this chip is an I/O port BAR.
    // We scan all BARs and take the first non-zero MEMORY BAR (bit 0 = 0).
    uint64_t mmio_phys = 0;
    for (uint32_t bar_off = 0x10; bar_off <= 0x24; bar_off += 4) {
        uint32_t bar = pci_read_config_dword(bus, device, function, bar_off);
        if (bar == 0 || bar == 0xFFFFFFFF) continue;
        if (bar & 0x01) {
            // I/O space BAR — skip it
            continue;
        }
        // Memory BAR: bits 2:1 = type (00=32-bit, 10=64-bit)
        int bar_type = (bar >> 1) & 0x3;
        uint64_t base = (uint64_t)(bar & 0xFFFFFFF0);
        if (bar_type == 0x2) {
            // 64-bit BAR: next DWORD is the upper 32 bits
            bar_off += 4;
            uint32_t bar_high = pci_read_config_dword(bus, device, function, bar_off);
            base |= ((uint64_t)bar_high << 32);
        }
        if (base != 0) {
            mmio_phys = base;
            break;
        }
    }
    mmio_base = (uint8_t *)mmio_phys;

    // Print the BAR address for diagnostics (lower 32 bits)
    print_string("RTL8169: MMIO mapped at 0x");
    char hex[9];
    uint32_t v = (uint32_t)(mmio_phys & 0xFFFFFFFF);
    for (int i = 7; i >= 0; i--) { hex[i] = "0123456789ABCDEF"[v & 0xF]; v >>= 4; }
    hex[8] = '\0';
    print_string(hex);
    print_string("\n");

    if (!mmio_base) {
        print_string("RTL8169: FATAL - No MMIO BAR found! NIC unusable.\n");
        return -1;
    }

    // Step 1: Issue Software Reset FIRST to bring hardware to a known clean state.
    // The reset MUST happen before any config writes (including MDIO),
    // because config writes before reset are immediately wiped by the reset!
    print_string("RTL8169: Performing hardware reset...\n");
    rtl_outb(RTL_COMMAND, RTL_CMD_RESET);
    
    uint32_t timeout = 100000;
    while ((rtl_inb(RTL_COMMAND) & RTL_CMD_RESET) && timeout > 0) {
        timeout--;
    }
    if (timeout == 0) {
        print_string("RTL8169: WARNING - HW Reset timeout.\n");
    }
    print_string("RTL8169: Reset complete.\n");

    // Step 2: Unlock Configuration Registers AFTER reset
    print_string("RTL8169: Unlocking Config...\n");
    rtl_outb(RTL_9346CR, RTL_9346CR_UNLOCK);

    // Step 3: Wake up the PHY via MDIO NOW (after reset, so it sticks!)
    print_string("RTL8169: Waking up PHY via MDIO & Restarting Auto-MDIX...\n");
    // PHY Register 0 (Basic Mode Control Register):
    // Bit 12 = Auto-Negotiation Enable, Bit 9 = Restart Auto-Negotiation.
    // This forces the PHY to power up and negotiate Auto-MDIX over a crossover cable.
    // We set full speed + full duplex as forced fallback (bits 6, 8, 13).
    rtl_mdio_write(0, 0x1200);

    // Lock config back
    rtl_outb(RTL_9346CR, RTL_9346CR_LOCK);

    // Step 4 (setup rings): Descriptor rings are static BSS — already aligned and ready.
    // Just initialize them. No kmalloc needed!
    print_string("RTL8169: Initializing DMA Rings (static BSS)...\n");

    for (int i = 0; i < NUM_DESCS; i++) {
        // RX ring: hardware owns all descriptors, max size 1536
        rx_ring[i].vlan    = 0;
        rx_ring[i].buf_low  = (uint32_t)((uint64_t)rx_buffers[i] & 0xFFFFFFFF);
        rx_ring[i].buf_high = (uint32_t)((uint64_t)rx_buffers[i] >> 32);
        uint32_t rx_cmd = RTL_DESC_OWN | 1536;
        if (i == NUM_DESCS - 1) rx_cmd |= 0x40000000; // EOR
        rx_ring[i].command = rx_cmd;
        flush_cache_line(&rx_ring[i]);

        // TX ring: CPU owns, cleared
        tx_ring[i].command  = 0;
        tx_ring[i].vlan     = 0;
        tx_ring[i].buf_low  = 0;
        tx_ring[i].buf_high = 0;
        flush_cache_line(&tx_ring[i]);
    }

    // Tell hardware the physical addresses of the rings
    rtl_outd(RTL_TX_DESC_START,     (uint32_t)((uint64_t)tx_ring & 0xFFFFFFFF));
    rtl_outd(RTL_TX_DESC_START + 4, (uint32_t)((uint64_t)tx_ring >> 32));
    rtl_outd(RTL_RX_DESC_START,     (uint32_t)((uint64_t)rx_ring & 0xFFFFFFFF));
    rtl_outd(RTL_RX_DESC_START + 4, (uint32_t)((uint64_t)rx_ring >> 32));

    // Step 4: Enable Receiver (RE) and Transmitter (TE)
    print_string("RTL8169: Configuring RX Filters & Enabling RX/TX logic...\n");
    
    // Set Receive Maximum Size (RMS) to 1536. If we do not set this, hardware drops all RX packets!
    if (mmio_base) *(volatile uint16_t *)(mmio_base + 0xDA) = 1536;

    // TCR: Set Interframe Gap and Max DMA Burst Size (7 = unlimited)
    rtl_outd(0x40, 0x03000700);

    // RCR: Max DMA Burst Size (7=unlimited) | AAP=Promiscuous(0x01) | APM=MyMAC(0x02)
    //      AM=Multicast(0x04) | AB=Broadcast(0x08)
    // AAP (bit 0) is essential early on: if our MAC read is wrong, we still accept ARP replies!
    rtl_outd(RTL_RCR, 0x0000070F);
    
    rtl_outb(RTL_COMMAND, RTL_CMD_RE | RTL_CMD_TE);

    print_string("RTL8169: HW init done. Waiting for physical link...\n");

    // Poll PHYStatus register (offset 0x6C). Bit 1 = LinkStatus.
    // Wait up to ~3 seconds (300000 iterations). Without a real link the NIC receives nothing!
    {
        int link_up = 0;
        for (int i = 0; i < 300000; i++) {
            uint8_t phystatus = rtl_inb(0x6C);
            if (phystatus & 0x02) { link_up = 1; break; }
            // Small delay
            for (volatile int d = 0; d < 100; d++);
        }
        if (link_up) {
            print_string("RTL8169: Physical Link is UP!\n");
        } else {
            print_string("RTL8169: WARNING - No physical link detected after timeout!\n");
            print_string("RTL8169: Check Ethernet cable. Continuing anyway...\n");
        }
    }
    
    // Mount hardware into software network stack using DHCP
    lwip_init();
    ip4_addr_t ipaddr, netmask, gw;
    
    ip4_addr_set_zero(&ipaddr);
    ip4_addr_set_zero(&netmask);
    ip4_addr_set_zero(&gw);

    netif_add(&rtl_netif, &ipaddr, &netmask, &gw, NULL, rtl8169_netif_init, netif_input);
    netif_set_default(&rtl_netif);
    
    netif_set_status_callback(&rtl_netif, rtl8169_netif_status_cb);
    netif_set_up(&rtl_netif);
    
    dhcp_start(&rtl_netif);

    print_string("RTL8169: lwIP Bound to Hardware! Requesting DHCP...\n");

    // Enable timer-IRQ-driven network polling now that lwIP is fully initialized.
    // From this point the timer handler calls rtl8169_poll() + sys_check_timeouts()
    // every 10 ms — unconditionally, regardless of what the shell task is doing.
    // This is what prevents TCP retransmit backoff from ballooning to ~2 minutes.
    timer_set_net_ready();
    print_string("RTL8169: Timer-driven RX poll ACTIVE (10 ms interval).\n");
    rtl8169_initialized = 1;
    return 0;
}
