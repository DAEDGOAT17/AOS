// pci.h
#ifndef PCI_H
#define PCI_H

#include <stdint.h>

// Read a 32-bit config dword from PCI configuration space
uint32_t pci_read_config_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);

// Write a 32-bit config dword to PCI configuration space
void pci_write_config_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value);

// Scan the PCI bus and print any Multimedia Audio Controllers (class 0x04, subclass 0x03)
void pci_scan_multimedia();

// Scan the PCI bus and print SSDs / Storage Controllers (class 0x01)
void pci_scan_storage();

// Scan the PCI bus and print Network Controllers (class 0x02)
void pci_scan_network();

int aos_apply_network_setup(const char *request);

#endif

