#ifndef RTL8169_H
#define RTL8169_H

#include <stdint.h>

// RTL8169 / RTL8168 Registers (MMIO offsets)
#define RTL_MAC0             0x00
#define RTL_MAR0             0x08
#define RTL_TX_DESC_START    0x20 // TNPDS (Transmit Normal Priority Descriptor Start)
#define RTL_COMMAND          0x37
#define RTL_TPPOLL           0x38
#define RTL_IMR              0x3C // Interrupt Mask Register
#define RTL_ISR              0x3E // Interrupt Status Register
#define RTL_TCR              0x40 // Transmit Configuration Register
#define RTL_RCR              0x44 // Receive Configuration Register
#define RTL_9346CR           0x50 // EEPROM / Config Register
#define RTL_CONFIG1          0x52
#define RTL_RX_DESC_START    0xE4 // RDSAR (Receive Descriptor Start Address)

// Commands and Bits
#define RTL_CMD_RESET        0x10
#define RTL_CMD_RE           0x08  // Receiver Enable
#define RTL_CMD_TE           0x04  // Transmitter Enable

#define RTL_9346CR_UNLOCK    0xC0
#define RTL_9346CR_LOCK      0x00

#define RTL_DESC_OWN         0x80000000

// Hardware Descriptor Structure (16 bytes per descriptor)
typedef struct {
    uint32_t command;
    uint32_t vlan;
    uint32_t buf_low;
    uint32_t buf_high;
} __attribute__((packed)) rtl_desc_t;

// Standard Init Function
int rtl8169_init(uint32_t bus, uint32_t device, uint32_t function);
void rtl8169_poll(void);
void rtl8169_print_packet_parsed(uint8_t* data, int len);

#endif
