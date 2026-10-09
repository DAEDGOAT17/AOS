#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>

// AHCI Memory Registers
typedef volatile struct {
    uint32_t clb;       // Command list base address, 1K-byte aligned
    uint32_t clbu;      // Command list base address upper 32 bits
    uint32_t fb;        // FIS base address, 256-byte aligned
    uint32_t fbu;       // FIS base address upper 32 bits
    uint32_t is;        // Interrupt status
    uint32_t ie;        // Interrupt enable
    uint32_t cmd;       // Command and status
    uint32_t rsv0;      // Reserved
    uint32_t tfd;       // Task file data
    uint32_t sig;       // Signature
    uint32_t ssts;      // SATA status (SCR0:SStatus)
    uint32_t sctl;      // SATA control (SCR2:SControl)
    uint32_t serr;      // SATA error (SCR1:SError)
    uint32_t sact;      // SATA active (SCR3:SActive)
    uint32_t ci;        // Command issue
    uint32_t sntf;      // SATA notification (SCR4:SNotification)
    uint32_t fbs;       // FIS-based switch control
    uint32_t rsv1[11];  // Reserved
    uint32_t vendor[4]; // Vendor specific
} __attribute__((packed)) ahci_port_t;

typedef volatile struct {
    uint32_t cap;       // Host capability
    uint32_t ghc;       // Global host control
    uint32_t is;        // Interrupt status
    uint32_t pi;        // Ports implemented
    uint32_t vs;        // Version
    uint32_t ccc_ctl;   // Command completion coalescing control
    uint32_t ccc_pts;   // Command completion coalescing ports
    uint32_t em_loc;    // Enclosure management location
    uint32_t em_ctl;    // Enclosure management control
    uint32_t cap2;      // Host capabilities extended
    uint32_t bohc;      // BIOS/OS handoff control and status
    uint8_t  rsv[0x74]; // Reserved through offset 0x9F
    uint8_t  vendor[0x60]; // Vendor specific registers
    ahci_port_t ports[32]; // Port control registers
} __attribute__((packed)) ahci_hba_t;

// Initialize the AHCI controller given its PCI Base Address (ABAR)
void ahci_init(uint32_t abar);

// Initialize an AHCI disk discovered on PCI and expose bounded sector I/O.
int ahci_init_storage(uint32_t *sector_count);
int ahci_read_sectors(uint32_t lba, uint8_t count, void *buffer);
int ahci_write_sectors(uint32_t lba, uint8_t count, const void *buffer);
int ahci_flush_cache(void);

// Attempt to detect AHCI capabilities on the PCI bus
void pci_init_ahci();

#endif
