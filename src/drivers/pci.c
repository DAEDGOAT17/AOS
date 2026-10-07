#include "io.h"
#include "screen.h"
#include "pci.h"
#include "e1000.h"
#include "rtl8169.h"
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

static int pci_parse_request_component(const char **cursor, char separator, uint32_t *value) {
    uint32_t parsed = 0;
    int digits = 0;

    if (!cursor || !*cursor || !value) {
        return 0;
    }

    while (**cursor >= '0' && **cursor <= '9') {
        uint32_t digit = (uint32_t)(**cursor - '0');
        if (parsed > (0xFFFFFFFFU - digit) / 10U) {
            return 0;
        }
        parsed = parsed * 10U + digit;
        (*cursor)++;
        digits++;
    }

    if (!digits || **cursor != separator) {
        return 0;
    }

    (*cursor)++;
    *value = parsed;
    return 1;
}

int aos_apply_network_setup(const char *request) {
    uint32_t bus;
    uint32_t device;
    uint32_t function;
    uint32_t identity;
    uint16_t vendor_id;
    uint16_t device_id;
    const char *guidance;

    if (!request ||
        !pci_parse_request_component(&request, ',', &bus) ||
        !pci_parse_request_component(&request, ',', &device) ||
        !pci_parse_request_component(&request, '|', &function) ||
        bus > 255 || device >= 32 || function >= 8) {
        print_string("AOS SETUP: invalid device patch request.\n");
        return -1;
    }

    guidance = request;
    identity = pci_read_config_dword((uint8_t)bus, (uint8_t)device, (uint8_t)function, 0x00);
    vendor_id = (uint16_t)(identity & 0xFFFF);
    device_id = (uint16_t)(identity >> 16);

    if (vendor_id == 0xFFFF) {
        print_string("AOS SETUP: PCI device is no longer present.\n");
        return -2;
    }

    if (vendor_id == 0x8086 && device_id == 0x100E) {
        print_string("AOS SETUP: applying built-in E1000 template.\n");
        if (e1000_init(bus, device, function) != 0) {
            print_string("AOS SETUP: E1000 initialization failed.\n");
            return -3;
        }
    } else if (vendor_id == 0x10EC && (device_id == 0x8168 || device_id == 0x8169)) {
        print_string("AOS SETUP: applying built-in RTL8169 template.\n");
        if (rtl8169_init(bus, device, function) != 0) {
            print_string("AOS SETUP: RTL8169 initialization failed.\n");
            return -4;
        }
    } else {
        print_string("AOS SETUP: no safe built-in template for PCI device.\n");
        return -5;
    }

    print_string("AOS SETUP: ");
    print_string(guidance);
    print_string("\n");
    return 0;
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
                            rtl8169_init(bus, device, function);
                        } else if (vendor_id == 0x8086 && device_id == 0x100E) {
                            print_string("\nPCI: Intel E1000 matched. Initializing DMA network driver...\n");
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
