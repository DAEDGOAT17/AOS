#include "io.h"
#include "screen.h"
#include "pci.h"
#include <stdint.h>

// PCI config ports
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

uint32_t pci_read_config_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t address;
    uint32_t lbus = (uint32_t)bus;
    uint32_t ldevice = (uint32_t)device;
    uint32_t lfunc = (uint32_t)function;

    // Build the configuration address as per PCI spec
    address = (uint32_t)((1U << 31) | (lbus << 16) | (ldevice << 11) | (lfunc << 8) | (offset & 0xFC));
    outl(PCI_CONFIG_ADDRESS, address);
    uint32_t data = inl(PCI_CONFIG_DATA);
    return data;
}

void pci_write_config_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value) {
    uint32_t address;
    uint32_t lbus = (uint32_t)bus;
    uint32_t ldevice = (uint32_t)device;
    uint32_t lfunc = (uint32_t)function;

    address = (uint32_t)((1U << 31) | (lbus << 16) | (ldevice << 11) | (lfunc << 8) | (offset & 0xFC));
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

void pci_scan_multimedia() {
    print_string("PCI: Scanning for Multimedia Audio Controllers...\n");
    for (uint32_t bus = 0; bus < 256; ++bus) {
        for (uint32_t device = 0; device < 32; ++device) {
            // First check function 0 vendor ID
            uint32_t vendor_dev = pci_read_config_dword(bus, device, 0, 0x00);
            uint16_t vendor = vendor_dev & 0xFFFF;
            if (vendor == 0xFFFF) continue; // no device

            // Determine whether multifunction
            uint32_t header = pci_read_config_dword(bus, device, 0, 0x0C);
            uint8_t header_type = (header >> 16) & 0xFF;
            uint32_t max_func = (header_type & 0x80) ? 8 : 1;

            for (uint32_t function = 0; function < max_func; ++function) {
                uint32_t id = pci_read_config_dword(bus, device, function, 0x00);
                uint16_t vendor_id = id & 0xFFFF;
                if (vendor_id == 0xFFFF) continue;

                uint32_t cls = pci_read_config_dword(bus, device, function, 0x08);
                uint8_t class_code = (cls >> 24) & 0xFF;
                uint8_t subclass = (cls >> 16) & 0xFF;

                if (class_code == 0x04 && subclass == 0x03) {
                    print_string("Found Multimedia Audio Controller at ");
                    // Print bus:device.function
                    print_string("bus ");
                    kprint_dec(bus);
                    print_string(" dev ");
                    kprint_dec(device);
                    print_string(" func ");
                    kprint_dec(function);
                    print_string(" vendor= ");
                    kprint_hex(vendor_id);
                    print_string(" device= ");
                    uint16_t device_id = (id >> 16) & 0xFFFF;
                    kprint_hex(device_id);
                    print_string("\n");
                    if (vendor_id == 0x8086 && device_id == 0x2668) {
                        extern void hda_audio_init(uint32_t bus, uint32_t device, uint32_t function);
                        hda_audio_init(bus, device, function);
                    }
                }
            }
        }
    }
    print_string("PCI: Scan complete.\n");
}

void pci_scan_storage() {
    print_string("PCI: Scanning for Storage Controllers...\n");
    for (uint32_t bus = 0; bus < 256; ++bus) {
        for (uint32_t device = 0; device < 32; ++device) {
            uint32_t vendor_dev = pci_read_config_dword(bus, device, 0, 0x00);
            uint16_t vendor = vendor_dev & 0xFFFF;
            if (vendor == 0xFFFF) continue;

            uint32_t header = pci_read_config_dword(bus, device, 0, 0x0C);
            uint8_t header_type = (header >> 16) & 0xFF;
            uint32_t max_func = (header_type & 0x80) ? 8 : 1;

            for (uint32_t function = 0; function < max_func; ++function) {
                uint32_t id = pci_read_config_dword(bus, device, function, 0x00);
                uint16_t vendor_id = id & 0xFFFF;
                if (vendor_id == 0xFFFF) continue;

                uint32_t cls = pci_read_config_dword(bus, device, function, 0x08);
                uint8_t class_code = (cls >> 24) & 0xFF;
                uint8_t subclass = (cls >> 16) & 0xFF;
                uint8_t prog_if = (cls >> 8) & 0xFF;

                if (class_code == 0x01) {
                    print_string("PCI: Found SSD / Storage Controller at bus ");
                    kprint_dec(bus);
                    print_string(" dev ");
                    kprint_dec(device);
                    print_string(" func ");
                    kprint_dec(function);
                    print_string(" [vid=");
                    char* hex = "0123456789ABCDEF";
                    for (int i = 12; i >= 0; i -= 4) print_char(hex[(vendor_id >> i) & 0xF]);
                    print_string(" did=");
                    uint16_t device_id = (id >> 16) & 0xFFFF;
                    for (int i = 12; i >= 0; i -= 4) print_char(hex[(device_id >> i) & 0xF]);
                    print_string("] ->");

                    if (subclass == 0x01) print_string(" IDE");
                    else if (subclass == 0x06) {
                        print_string(" SATA");
                        if (prog_if == 0x01) print_string(" (AHCI)");
                    }
                    else if (subclass == 0x08) {
                        print_string(" NVMe SSD");
                    }
                    else {
                        print_string(" Other Storage");
                    }
                    print_string("\n");
                }
                else if (class_code == 0x0C && subclass == 0x03) {
                    print_string("PCI: Found USB Controller at bus ");
                    kprint_dec(bus);
                    print_string(" dev ");
                    kprint_dec(device);
                    print_string(" func ");
                    kprint_dec(function);
                    print_string(" [vid=");
                    char* hex = "0123456789ABCDEF";
                    for (int i = 12; i >= 0; i -= 4) print_char(hex[(vendor_id >> i) & 0xF]);
                    print_string(" did=");
                    uint16_t device_id = (id >> 16) & 0xFFFF;
                    for (int i = 12; i >= 0; i -= 4) print_char(hex[(device_id >> i) & 0xF]);
                    print_string("] ->");

                    if (prog_if == 0x00) print_string(" UHCI (USB 1.x)");
                    else if (prog_if == 0x10) print_string(" OHCI (USB 1.x)");
                    else if (prog_if == 0x20) print_string(" EHCI (USB 2.0)");
                    else if (prog_if == 0x30) print_string(" xHCI (USB 3.0)");
                    else print_string(" Unknown USB");
                    print_string("\n");
                }
            }
        }
    }
}

void pci_scan_network() {
    print_string("PCI: Scanning for Network Controllers...\n");
    for (uint32_t bus = 0; bus < 256; ++bus) {
        for (uint32_t device = 0; device < 32; ++device) {
            uint32_t vendor_dev = pci_read_config_dword(bus, device, 0, 0x00);
            uint16_t vendor = vendor_dev & 0xFFFF;
            if (vendor == 0xFFFF) continue;

            uint32_t header = pci_read_config_dword(bus, device, 0, 0x0C);
            uint8_t header_type = (header >> 16) & 0xFF;
            uint32_t max_func = (header_type & 0x80) ? 8 : 1;

            for (uint32_t function = 0; function < max_func; ++function) {
                uint32_t id = pci_read_config_dword(bus, device, function, 0x00);
                uint16_t vendor_id = id & 0xFFFF;
                if (vendor_id == 0xFFFF) continue;

                uint32_t cls = pci_read_config_dword(bus, device, function, 0x08);
                uint8_t class_code = (cls >> 24) & 0xFF;
                uint8_t subclass = (cls >> 16) & 0xFF;

                if (class_code == 0x02) {
                    print_string("PCI: Found Network Controller at bus ");
                    kprint_dec(bus);
                    print_string(" dev ");
                    kprint_dec(device);
                    print_string(" func ");
                    kprint_dec(function);
                    print_string(" [vid=");
                    char* hex = "0123456789ABCDEF";
                    for (int i = 12; i >= 0; i -= 4) print_char(hex[(vendor_id >> i) & 0xF]);
                    print_string(" did=");
                    uint16_t device_id = (id >> 16) & 0xFFFF;
                    for (int i = 12; i >= 0; i -= 4) print_char(hex[(device_id >> i) & 0xF]);
                    print_string("] ->");
                    
                    if (subclass == 0x00) {
                        print_string(" Ethernet");
                        if (vendor_id == 0x10EC && (device_id == 0x8168 || device_id == 0x8169)) {
                            print_string("\nPCI: Realtek 8168/8169 Ethernet matched. Bootstrapping...\n");
                            extern void rtl8169_init(uint32_t bus, uint32_t device, uint32_t function);
                            rtl8169_init(bus, device, function);
                        } else if (vendor_id == 0x8086 && device_id == 0x100E) {
                            print_string("\nPCI: Intel E1000 matched. Initializing DMA network driver...\n");
                            extern void e1000_init(uint32_t bus, uint32_t device, uint32_t function);
                            e1000_init(bus, device, function);
                        }
                    }
                    else if (subclass == 0x80) print_string(" Wi-Fi / Other");
                    else print_string(" Unknown Network");
                    print_string("\n");
                }
            }
        }
    }
}
