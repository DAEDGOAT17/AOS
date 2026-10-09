#include "ahci.h"
#include "screen.h"
#include "pci.h"
#include "string.h"

#define AHCI_PCI_CLASS 0x01
#define AHCI_PCI_SUBCLASS 0x06
#define AHCI_PCI_PROGIF 0x01
#define AHCI_SIG_ATA 0x00000101U
#define AHCI_PORT_DET_ACTIVE 3U
#define AHCI_PORT_IPM_ACTIVE 1U
#define AHCI_PORT_CMD_ST 0x0001U
#define AHCI_PORT_CMD_FRE 0x0010U
#define AHCI_PORT_CMD_FR 0x4000U
#define AHCI_PORT_CMD_CR 0x8000U
#define AHCI_PORT_TFD_BSY 0x0080U
#define AHCI_PORT_TFD_DRQ 0x0008U
#define AHCI_PORT_TFD_ERR 0x0001U
#define AHCI_PORT_IS_TFES 0x40000000U
#define AHCI_CMD_IDENTIFY 0xECU
#define AHCI_CMD_READ_DMA_EXT 0x25U
#define AHCI_CMD_WRITE_DMA_EXT 0x35U
#define AHCI_MAX_PORTS 32
#define AHCI_SLOTS 32
#define AHCI_POLL_LIMIT 10000000U

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t prdt_length;
    uint32_t transferred;
    uint32_t table_base;
    uint32_t table_base_upper;
    uint32_t reserved[4];
} ahci_command_header_t;

typedef struct __attribute__((packed)) {
    uint32_t data_base;
    uint32_t data_base_upper;
    uint32_t reserved;
    uint32_t byte_count;
} ahci_prdt_entry_t;

typedef struct __attribute__((packed, aligned(128))) {
    uint8_t command_fis[64];
    uint8_t atapi_command[16];
    uint8_t reserved[48];
    ahci_prdt_entry_t prdt;
} ahci_command_table_t;

static volatile ahci_hba_t *ahci_hba;
static int ahci_active_port = -1;
static uint32_t ahci_total_sectors;
static ahci_command_header_t ahci_command_lists[AHCI_MAX_PORTS][AHCI_SLOTS]
    __attribute__((aligned(1024)));
static uint8_t ahci_received_fis[AHCI_MAX_PORTS][256]
    __attribute__((aligned(256)));
static uint8_t ahci_command_tables[AHCI_MAX_PORTS][256]
    __attribute__((aligned(128)));
static uint16_t ahci_identify_data[256] __attribute__((aligned(512)));

static int ahci_wait_port_clear(volatile ahci_port_t *port, uint32_t mask, int task_file) {
    uint32_t timeout = AHCI_POLL_LIMIT;
    while (((task_file ? port->tfd : port->cmd) & mask) != 0U && timeout > 0U) timeout--;
    return timeout > 0U;
}

static int ahci_start_port(volatile ahci_port_t *port, int index) {
    uintptr_t command_list = (uintptr_t)&ahci_command_lists[index][0];
    uintptr_t fis_buffer = (uintptr_t)&ahci_received_fis[index][0];

    port->cmd &= ~AHCI_PORT_CMD_ST;
    if (!ahci_wait_port_clear(port, AHCI_PORT_CMD_CR, 0)) return 0;
    port->cmd &= ~AHCI_PORT_CMD_FRE;
    if (!ahci_wait_port_clear(port, AHCI_PORT_CMD_FR, 0)) return 0;

    memset(&ahci_command_lists[index][0], 0, sizeof(ahci_command_lists[index]));
    memset(&ahci_received_fis[index][0], 0, sizeof(ahci_received_fis[index]));
    port->clb = (uint32_t)command_list;
    port->clbu = (uint32_t)(command_list >> 32);
    port->fb = (uint32_t)fis_buffer;
    port->fbu = (uint32_t)(fis_buffer >> 32);
    port->is = 0xFFFFFFFFU;
    port->serr = 0xFFFFFFFFU;

    port->cmd |= AHCI_PORT_CMD_FRE;
    port->cmd |= AHCI_PORT_CMD_ST;
    return 1;
}

static int ahci_issue(uint8_t command, uint32_t lba, uint8_t count,
                      void *buffer, int write) {
    volatile ahci_port_t *port;
    ahci_command_header_t *header;
    ahci_command_table_t *table;
    uint8_t *fis;
    uintptr_t table_address;
    uintptr_t buffer_address;
    uint32_t byte_count;
    uint32_t timeout = AHCI_POLL_LIMIT;

    if (!ahci_hba || ahci_active_port < 0 || !buffer || count == 0U) return -1;
    if (command != AHCI_CMD_IDENTIFY &&
        (uint64_t)lba + count > ahci_total_sectors) return -1;

    port = &ahci_hba->ports[ahci_active_port];
    if (!ahci_wait_port_clear(port, AHCI_PORT_TFD_BSY | AHCI_PORT_TFD_DRQ, 1)) return -1;

    header = &ahci_command_lists[ahci_active_port][0];
    table_address = (uintptr_t)&ahci_command_tables[ahci_active_port][0];
    buffer_address = (uintptr_t)buffer;
    byte_count = (uint32_t)count * 512U;
    memset(&ahci_command_tables[ahci_active_port][0], 0, 256);

    header->flags = 5U | (write ? (1U << 6) : 0U);
    header->prdt_length = 1U;
    header->transferred = 0U;
    header->table_base = (uint32_t)table_address;
    header->table_base_upper = (uint32_t)(table_address >> 32);

    table = (ahci_command_table_t *)&ahci_command_tables[ahci_active_port][0];
    fis = table->command_fis;
    fis[0] = 0x27U;
    fis[1] = 0x80U;
    fis[2] = command;
    if (command != AHCI_CMD_IDENTIFY) {
        fis[4] = (uint8_t)lba;
        fis[5] = (uint8_t)(lba >> 8);
        fis[6] = (uint8_t)(lba >> 16);
        fis[7] = 0x40U;
        fis[8] = (uint8_t)(lba >> 24);
        fis[12] = count;
    }

    table->prdt.data_base = (uint32_t)buffer_address;
    table->prdt.data_base_upper = (uint32_t)(buffer_address >> 32);
    table->prdt.byte_count = byte_count - 1U;

    port->is = 0xFFFFFFFFU;
    port->ci = 1U;
    while ((port->ci & 1U) != 0U && timeout > 0U) timeout--;
    if (timeout == 0U || (port->is & AHCI_PORT_IS_TFES) != 0U ||
        (port->tfd & AHCI_PORT_TFD_ERR) != 0U) {
        port->is = 0xFFFFFFFFU;
        return -1;
    }

    port->is = 0xFFFFFFFFU;
    ahci_hba->is = 1U << ahci_active_port;
    return 0;
}

static int ahci_initialize_controller(uint32_t abar) {
    volatile ahci_hba_t *hba;
    uint32_t implemented_ports;

    if (abar == 0U) return -1;
    hba = (volatile ahci_hba_t *)(uintptr_t)abar;
    hba->ghc |= 1U << 31;
    implemented_ports = hba->pi;

    for (int index = 0; index < AHCI_MAX_PORTS; index++) {
        volatile ahci_port_t *port;
        uint32_t status;
        if ((implemented_ports & (1U << index)) == 0U) continue;
        port = &hba->ports[index];
        status = port->ssts;
        if ((status & 0x0FU) != AHCI_PORT_DET_ACTIVE ||
            ((status >> 8) & 0x0FU) != AHCI_PORT_IPM_ACTIVE ||
            port->sig != AHCI_SIG_ATA) continue;
        if (!ahci_start_port(port, index)) continue;

        ahci_hba = hba;
        ahci_active_port = index;
        ahci_total_sectors = 0U;
        if (ahci_issue(AHCI_CMD_IDENTIFY, 0U, 1U, ahci_identify_data, 0) != 0) {
            ahci_hba = NULL;
            ahci_active_port = -1;
            continue;
        }

        ahci_total_sectors = (uint32_t)ahci_identify_data[100] |
                             ((uint32_t)ahci_identify_data[101] << 16);
        if (ahci_total_sectors == 0U) {
            ahci_total_sectors = (uint32_t)ahci_identify_data[60] |
                                 ((uint32_t)ahci_identify_data[61] << 16);
        }
        if (ahci_total_sectors > 0U) {
            print_string("AHCI: SATA disk I/O ready on port ");
            kprint_dec((uint32_t)index);
            print_string("; sectors=");
            kprint_dec(ahci_total_sectors);
            print_char('\n');
            return 0;
        }
        ahci_hba = NULL;
        ahci_active_port = -1;
    }
    return -1;
}

int ahci_init_storage(uint32_t *sector_count) {
    if (ahci_hba && ahci_active_port >= 0) {
        if (sector_count) *sector_count = ahci_total_sectors;
        return 0;
    }

    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t device = 0; device < 32; device++) {
            uint32_t header = pci_read_config_dword((uint8_t)bus, (uint8_t)device, 0U, 0x0CU);
            uint32_t max_function = ((header >> 16) & 0x80U) ? 8U : 1U;
            for (uint32_t function = 0; function < max_function; function++) {
                uint32_t identity = pci_read_config_dword((uint8_t)bus, (uint8_t)device,
                                                           (uint8_t)function, 0x00U);
                uint32_t class_info;
                uint32_t command;
                uint32_t abar;
                if ((identity & 0xFFFFU) == 0xFFFFU) continue;
                class_info = pci_read_config_dword((uint8_t)bus, (uint8_t)device,
                                                    (uint8_t)function, 0x08U);
                if (((class_info >> 24) & 0xFFU) != AHCI_PCI_CLASS ||
                    ((class_info >> 16) & 0xFFU) != AHCI_PCI_SUBCLASS ||
                    ((class_info >> 8) & 0xFFU) != AHCI_PCI_PROGIF) continue;

                abar = pci_read_config_dword((uint8_t)bus, (uint8_t)device,
                                             (uint8_t)function, 0x24U) & 0xFFFFFFF0U;
                command = pci_read_config_dword((uint8_t)bus, (uint8_t)device,
                                                (uint8_t)function, 0x04U);
                pci_write_config_dword((uint8_t)bus, (uint8_t)device,
                                       (uint8_t)function, 0x04U,
                                       (command & 0xFFFFU) | 0x0006U);
                if (ahci_initialize_controller(abar) == 0) {
                    if (sector_count) *sector_count = ahci_total_sectors;
                    return 0;
                }
            }
        }
    }
    return -1;
}

int ahci_read_sectors(uint32_t lba, uint8_t count, void *buffer) {
    return ahci_issue(AHCI_CMD_READ_DMA_EXT, lba, count, buffer, 0);
}

int ahci_flush_cache(void) {
    volatile ahci_port_t *port;
    ahci_command_header_t *header;
    ahci_command_table_t *table;
    uint32_t timeout = AHCI_POLL_LIMIT;

    if (!ahci_hba || ahci_active_port < 0) return -1;
    port = &ahci_hba->ports[ahci_active_port];
    if (!ahci_wait_port_clear(port, AHCI_PORT_TFD_BSY | AHCI_PORT_TFD_DRQ, 1)) return -1;

    header = &ahci_command_lists[ahci_active_port][0];
    table = (ahci_command_table_t *)&ahci_command_tables[ahci_active_port][0];
    memset(table, 0, 256);
    header->flags = 5U;
    header->prdt_length = 0U;
    header->transferred = 0U;
    table->command_fis[0] = 0x27U;
    table->command_fis[1] = 0x80U;
    table->command_fis[2] = 0xEAU;

    port->is = 0xFFFFFFFFU;
    port->ci = 1U;
    while ((port->ci & 1U) != 0U && timeout > 0U) timeout--;
    if (timeout == 0U || (port->is & AHCI_PORT_IS_TFES) != 0U ||
        (port->tfd & AHCI_PORT_TFD_ERR) != 0U) {
        port->is = 0xFFFFFFFFU;
        return -1;
    }
    port->is = 0xFFFFFFFFU;
    ahci_hba->is = 1U << ahci_active_port;
    return 0;
}

int ahci_write_sectors(uint32_t lba, uint8_t count, const void *buffer) {
    return ahci_issue(AHCI_CMD_WRITE_DMA_EXT, lba, count, (void *)buffer, 1);
}

void ahci_init(uint32_t abar) {
    print_string("AHCI: Initializing HBA at 0x");
    kprint_hex(abar);
    print_char('\n');

    ahci_hba_t *hba = (ahci_hba_t *)(uint64_t)abar;
    
    // Check ports implemented
    uint32_t pi = hba->pi;
    for (int i = 0; i < 32; i++) {
        if (pi & (1 << i)) {
            // Port i is implemented
            uint32_t ssts = hba->ports[i].ssts;
            uint8_t det = ssts & 0x0F;
            uint8_t ipm = (ssts >> 8) & 0x0F;
            
            if (det == 3 && ipm == 1) { // Device present and active
                print_string("AHCI: Active drive detected on Port ");
                kprint_dec(i);
                print_string(" [Signature: 0x");
                kprint_hex(hba->ports[i].sig);
                print_string("]\n");
            }
        }
    }
}

void pci_init_ahci() {
    print_string("PCI: Scanning for AHCI Controllers...\n");
    for (uint32_t bus = 0; bus < 256; ++bus) {
        for (uint32_t device = 0; device < 32; ++device) {
            uint32_t vendor_dev = pci_read_config_dword(bus, device, 0, 0x00);
            if ((vendor_dev & 0xFFFF) == 0xFFFF) continue;

            uint32_t header = pci_read_config_dword(bus, device, 0, 0x0C);
            uint8_t header_type = (header >> 16) & 0xFF;
            uint32_t max_func = (header_type & 0x80) ? 8 : 1;

            for (uint32_t function = 0; function < max_func; ++function) {
                uint32_t id = pci_read_config_dword(bus, device, function, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) continue;

                uint32_t cls = pci_read_config_dword(bus, device, function, 0x08);
                uint8_t class_code = (cls >> 24) & 0xFF;
                uint8_t subclass = (cls >> 16) & 0xFF;
                uint8_t prog_if = (cls >> 8) & 0xFF;

                // Class 0x01 (Storage), Subclass 0x06 (SATA), ProgIF 0x01 (AHCI)
                if (class_code == 0x01 && subclass == 0x06 && prog_if == 0x01) {
                    print_string("PCI: AHCI Controller Found at ");
                    kprint_dec(bus); print_char(':');
                    kprint_dec(device); print_char('.');
                    kprint_dec(function); print_char('\n');
                    
                    // ABAR is usually at BAR5 for AHCI (Offset 0x24)
                    uint32_t abar = pci_read_config_dword(bus, device, function, 0x24);
                    // Clear lower bits to get physical address
                    abar &= 0xFFFFFFF0;
                    
                    if (abar) {
                        ahci_init(abar);
                    }
                    return; // Initialize the first one we find
                }
            }
        }
    }
    print_string("PCI: No AHCI Controllers found.\n");
}
