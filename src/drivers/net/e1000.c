#include "e1000.h"
#include "pci.h"
#include "vmm.h"
#include "timer.h"
#include "screen.h"
#include "string.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/pbuf.h"
#include "lwip/dhcp.h"
#include "lwip/ip4_addr.h"

#define E1000_REG_CTRL  0x0000
#define E1000_REG_IMC   0x00D8
#define E1000_REG_RCTL  0x0100
#define E1000_REG_TCTL  0x0400
#define E1000_REG_TIPG  0x0410
#define E1000_REG_RDBAL 0x2800
#define E1000_REG_RDBAH 0x2804
#define E1000_REG_RDLEN 0x2808
#define E1000_REG_RDH   0x2810
#define E1000_REG_RDT   0x2818
#define E1000_REG_TDBAL 0x3800
#define E1000_REG_TDBAH 0x3804
#define E1000_REG_TDLEN 0x3808
#define E1000_REG_TDH   0x3810
#define E1000_REG_TDT   0x3818
#define E1000_REG_RAL   0x5400
#define E1000_REG_RAH   0x5404

#define E1000_RING_SIZE 64
#define E1000_BUFFER_SIZE 2048
#define E1000_BAR_SIZE 0x20000
#define E1000_CTRL_RST 0x04000000
#define E1000_CTRL_SLU 0x00000040
#define E1000_RCTL_EN  0x00000002
#define E1000_RCTL_BAM 0x00008000
#define E1000_RCTL_SECRC 0x04000000
#define E1000_TCTL_EN  0x00000002
#define E1000_TCTL_PSP 0x00000008
#define E1000_TX_CMD_EOP  0x01
#define E1000_TX_CMD_IFCS 0x02
#define E1000_TX_CMD_RS   0x08
#define E1000_DESC_DONE 0x01

typedef struct __attribute__((packed)) {
    uint64_t buffer_addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} e1000_rx_desc_t;

typedef struct __attribute__((packed)) {
    uint64_t buffer_addr;
    uint16_t length;
    uint8_t checksum_offset;
    uint8_t command;
    uint8_t status;
    uint8_t checksum_start;
    uint16_t special;
} e1000_tx_desc_t;

static volatile uint8_t* e1000_mmio;
static struct netif e1000_netif;
static uint8_t e1000_mac[6];
static uint32_t rx_index;
static uint32_t tx_index;
static int initialized;

static e1000_rx_desc_t rx_ring[E1000_RING_SIZE] __attribute__((aligned(128)));
static e1000_tx_desc_t tx_ring[E1000_RING_SIZE] __attribute__((aligned(128)));
static uint8_t rx_buffers[E1000_RING_SIZE][E1000_BUFFER_SIZE] __attribute__((aligned(16)));
static uint8_t tx_buffers[E1000_RING_SIZE][E1000_BUFFER_SIZE] __attribute__((aligned(16)));

static uint32_t e1000_read(uint32_t reg) {
    return *(volatile uint32_t*)(e1000_mmio + reg);
}

static void e1000_write(uint32_t reg, uint32_t value) {
    *(volatile uint32_t*)(e1000_mmio + reg) = value;
}

static void e1000_status_changed(struct netif* netif) {
    print_string("\n[E1000] Network status changed. IP: ");
    print_string(ip4addr_ntoa(netif_ip4_addr(netif)));
    print_string("\nJARVIS [/] $ ");
}

static err_t e1000_netif_init(struct netif* netif) {
    netif->name[0] = 'e';
    netif->name[1] = '0';
    netif->linkoutput = NULL;
    netif->output = etharp_output;
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, e1000_mac, sizeof(e1000_mac));
    return ERR_OK;
}

static err_t e1000_linkoutput(struct netif* netif, struct pbuf* packet) {
    (void)netif;
    if (!initialized || packet->tot_len > E1000_BUFFER_SIZE) return ERR_BUF;

    e1000_tx_desc_t* desc = &tx_ring[tx_index];
    if (!(desc->status & E1000_DESC_DONE)) return ERR_BUF;

    if (pbuf_copy_partial(packet, tx_buffers[tx_index], packet->tot_len, 0) != packet->tot_len)
        return ERR_BUF;

    desc->buffer_addr = (uint64_t)tx_buffers[tx_index];
    desc->length = packet->tot_len;
    desc->checksum_offset = 0;
    desc->command = E1000_TX_CMD_EOP | E1000_TX_CMD_IFCS | E1000_TX_CMD_RS;
    desc->status = 0;
    desc->checksum_start = 0;
    desc->special = 0;
    asm volatile("mfence" ::: "memory");

    tx_index = (tx_index + 1) % E1000_RING_SIZE;
    e1000_write(E1000_REG_TDT, tx_index);
    return ERR_OK;
}

void e1000_poll(void) {
    if (!initialized) return;

    while (rx_ring[rx_index].status & E1000_DESC_DONE) {
        e1000_rx_desc_t* desc = &rx_ring[rx_index];
        uint16_t packet_len = desc->length;

        if (packet_len > 0 && packet_len <= E1000_BUFFER_SIZE && desc->errors == 0) {
            struct pbuf* packet = pbuf_alloc(PBUF_RAW, packet_len, PBUF_POOL);
            if (packet) {
                if (pbuf_take(packet, rx_buffers[rx_index], packet_len) == ERR_OK) {
                    if (e1000_netif.input(packet, &e1000_netif) != ERR_OK)
                        pbuf_free(packet);
                } else {
                    pbuf_free(packet);
                }
            }
        }

        desc->status = 0;
        desc->errors = 0;
        desc->length = 0;
        asm volatile("mfence" ::: "memory");
        e1000_write(E1000_REG_RDT, rx_index);
        rx_index = (rx_index + 1) % E1000_RING_SIZE;
    }
}

int e1000_init(uint32_t bus, uint32_t device, uint32_t function) {
    if (initialized) return 0;

    uint32_t command = pci_read_config_dword(bus, device, function, 0x04);
    pci_write_config_dword(bus, device, function, 0x04, command | 0x0006);

    uint32_t bar_low = pci_read_config_dword(bus, device, function, 0x10);
    if (bar_low == 0 || bar_low == 0xFFFFFFFF || (bar_low & 0x01)) {
        print_string("E1000 ERROR: no memory-mapped BAR found.\n");
        return -1;
    }

    uint64_t mmio_base = (uint64_t)(bar_low & 0xFFFFFFF0);
    if (((bar_low >> 1) & 0x03) == 0x02) {
        uint32_t bar_high = pci_read_config_dword(bus, device, function, 0x14);
        mmio_base |= (uint64_t)bar_high << 32;
    }

    for (uint64_t address = mmio_base; address < mmio_base + E1000_BAR_SIZE; address += 4096)
        vmm_map_page(address, address);
    e1000_mmio = (volatile uint8_t*)mmio_base;

    e1000_write(E1000_REG_IMC, 0xFFFFFFFF);
    e1000_write(E1000_REG_CTRL, E1000_CTRL_RST);
    for (volatile uint32_t timeout = 0; timeout < 1000000; timeout++) {
        if (!(e1000_read(E1000_REG_CTRL) & E1000_CTRL_RST)) break;
    }

    uint32_t ral = e1000_read(E1000_REG_RAL);
    uint32_t rah = e1000_read(E1000_REG_RAH);
    for (int i = 0; i < 4; i++) e1000_mac[i] = (uint8_t)(ral >> (i * 8));
    e1000_mac[4] = (uint8_t)rah;
    e1000_mac[5] = (uint8_t)(rah >> 8);
    if (e1000_mac[0] == 0 || (e1000_mac[0] & 0x01)) {
        e1000_mac[0] = 0x52;
        e1000_mac[1] = 0x54;
        e1000_mac[2] = 0x00;
        e1000_mac[3] = 0x12;
        e1000_mac[4] = 0x34;
        e1000_mac[5] = 0x56;
        e1000_write(E1000_REG_RAL, 0x12005452);
        e1000_write(E1000_REG_RAH, 0x80005634);
    }

    for (uint32_t i = 0; i < E1000_RING_SIZE; i++) {
        rx_ring[i].buffer_addr = (uint64_t)rx_buffers[i];
        rx_ring[i].length = 0;
        rx_ring[i].checksum = 0;
        rx_ring[i].status = 0;
        rx_ring[i].errors = 0;
        rx_ring[i].special = 0;

        tx_ring[i].buffer_addr = (uint64_t)tx_buffers[i];
        tx_ring[i].length = 0;
        tx_ring[i].checksum_offset = 0;
        tx_ring[i].command = 0;
        tx_ring[i].status = E1000_DESC_DONE;
        tx_ring[i].checksum_start = 0;
        tx_ring[i].special = 0;
    }

    e1000_write(E1000_REG_RDBAL, (uint32_t)(uint64_t)rx_ring);
    e1000_write(E1000_REG_RDBAH, (uint32_t)((uint64_t)rx_ring >> 32));
    e1000_write(E1000_REG_RDLEN, sizeof(rx_ring));
    e1000_write(E1000_REG_RDH, 0);
    e1000_write(E1000_REG_RDT, E1000_RING_SIZE - 1);

    e1000_write(E1000_REG_TDBAL, (uint32_t)(uint64_t)tx_ring);
    e1000_write(E1000_REG_TDBAH, (uint32_t)((uint64_t)tx_ring >> 32));
    e1000_write(E1000_REG_TDLEN, sizeof(tx_ring));
    e1000_write(E1000_REG_TDH, 0);
    e1000_write(E1000_REG_TDT, 0);
    e1000_write(E1000_REG_TIPG, 0x0060200A);
    e1000_write(E1000_REG_TCTL, 0x0004010A | E1000_TCTL_EN | E1000_TCTL_PSP);
    e1000_write(E1000_REG_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC);
    e1000_write(E1000_REG_CTRL, E1000_CTRL_SLU);
    asm volatile("mfence" ::: "memory");

    lwip_init();
    ip4_addr_t address, netmask, gateway;
    ip4_addr_set_zero(&address);
    ip4_addr_set_zero(&netmask);
    ip4_addr_set_zero(&gateway);
    if (!netif_add(&e1000_netif, &address, &netmask, &gateway, NULL,
                   e1000_netif_init, netif_input)) {
        print_string("E1000 ERROR: lwIP netif setup failed.\n");
        return -1;
    }
    e1000_netif.linkoutput = e1000_linkoutput;
    netif_set_default(&e1000_netif);
    netif_set_status_callback(&e1000_netif, e1000_status_changed);
    netif_set_up(&e1000_netif);
    dhcp_start(&e1000_netif);

    initialized = 1;
    timer_set_net_ready();
    print_string("E1000: DMA rings active; DHCP started.\n");
    return 0;
}