# ATA/IDE Storage Driver - Hard Disk Access

## Overview

The ATA (Advanced Technology Attachment) driver enables AOS to communicate with hard disk drives via the IDE (Integrated Drive Electronics) interface. ATA/IDE allows reading and writing data to storage drives via Programmed I/O (PIO) operations.

---

## What is ATA/IDE?

ATA is a protocol for communicating with storage devices:

- **ATA-1**: Original parallel ATA (PATA)
- **SATA**: Serial ATA (modern drives)
- **IDE**: Integrated Electronics (hardware interface)
- **AHCI**: Advanced Host Controller Interface (modern SATA controllers)

AOS supports **ATA PIO mode**, the slowest but most compatible method.

---

## ATA Port I/O Addressing

### Primary Channel (Master/Slave)

```
I/O Port Mapping:

0x1F0  - Data register (Read/Write)
0x1F1  - Error register (Read)
0x1F1  - Feature register (Write pre-command)
0x1F2  - Sector count
0x1F3  - Sector number (LBA low byte)
0x1F4  - Cylinder low (LBA mid byte)
0x1F5  - Cylinder high (LBA high byte)
0x1F6  - Drive/Head (Master/Slave selector)
0x1F7  - Status (Read)
0x1F7  - Command (Write)
0x3F6  - Control register (IRQ/reset)
0x3F7  - Alternate status
```

### Secondary Channel

Same ports shifted by 0x170 (used for additional drives):

```
0x170  - Data register
0x171  - Error register
... (same pattern)
```

---

## ATA Commands

| Command           | Code | Purpose                       |
| ----------------- | ---- | ----------------------------- |
| **Read Sectors**  | 0x20 | Read disk sectors into buffer |
| **Write Sectors** | 0x30 | Write buffer to disk sectors  |
| **Identify**      | 0xEC | Get drive information         |
| **Set Features**  | 0xEF | Enable/disable features       |

---

## Implementation in AOS

### ATA Device Structure

```c
// From include/ata.h
struct ata_device {
    uint16_t io_base;           // I/O base address (0x1F0 or 0x170)
    uint16_t control_base;      // Control register address
    int      is_atapi;          // ATAPI device?
    int      exists;            // Device exists?

    char     model[41];         // Drive model name
    uint32_t sectors;           // Total sectors

    // Current operation state
    uint32_t current_sector;
    uint16_t current_byte;
};

extern struct ata_device ata_devices[4];  // Master/Slave × Primary/Secondary
```

### Identifying ATA Devices

```c
void ata_identify(int id) {
    struct ata_device* dev = &ata_devices[id];

    // Select device
    outb(dev->io_base + ATA_REG_HDDEVSEL, 0xA0 | (id << 4));

    // Send IDENTIFY command
    outb(dev->io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    // Wait for response
    uint8_t status = inb(dev->io_base + ATA_REG_STATUS);
    if (status == 0) {
        dev->exists = 0;
        return;
    }

    // Wait for busy bit to clear
    while (inb(dev->io_base + ATA_REG_STATUS) & ATA_SR_BSY);

    // Read identification data
    uint16_t* buf = kmalloc(512);
    for (int i = 0; i < 256; i++) {
        buf[i] = inw(dev->io_base + ATA_REG_DATA);
    }

    // Extract drive information
    dev->sectors = *(uint32_t*)(buf + 60);  // LBA28

    // Extract model name
    for (int i = 0; i < 40; i += 2) {
        dev->model[i]   = buf[27 + (i/2)] >> 8;
        dev->model[i+1] = buf[27 + (i/2)] & 0xFF;
    }
    dev->model[40] = '\0';

    dev->exists = 1;
    kfree(buf);
}
```

---

## Reading Sectors

### Basic Read Operation (28-bit LBA)

```c
int ata_read_sectors(int id, uint32_t lba, uint16_t count, uint8_t* buf) {
    struct ata_device* dev = &ata_devices[id];

    if (!dev->exists || count == 0 || count > 256) {
        return -1;
    }

    // Select device
    uint8_t cmd_select = 0xE0 | (id << 4);  // Master or slave
    outb(dev->io_base + ATA_REG_HDDEVSEL, cmd_select | ((lba >> 24) & 0x0F));

    // Set block count
    outb(dev->io_base + ATA_REG_SECCOUNT0, count);

    // Set LBA
    outb(dev->io_base + ATA_REG_LBA0, lba & 0xFF);
    outb(dev->io_base + ATA_REG_LBA1, (lba >> 8) & 0xFF);
    outb(dev->io_base + ATA_REG_LBA2, (lba >> 16) & 0xFF);

    // Send READ SECTORS command
    outb(dev->io_base + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    // Wait for each sector
    for (int i = 0; i < count; i++) {
        // Wait for data ready
        while (inb(dev->io_base + ATA_REG_STATUS) & ATA_SR_BSY);
        uint8_t status = inb(dev->io_base + ATA_REG_STATUS);

        if (status & ATA_SR_DF) {
            return -1;  // Device fault
        }
        if (!(status & ATA_SR_DRQ)) {
            return -1;  // No data
        }

        // Read 512 bytes (256 words)
        for (int j = 0; j < 256; j++) {
            uint16_t data = inw(dev->io_base + ATA_REG_DATA);
            buf[i*512 + j*2]     = data & 0xFF;
            buf[i*512 + j*2 + 1] = (data >> 8) & 0xFF;
        }
    }

    return count;
}
```

---

## Writing Sectors

```c
int ata_write_sectors(int id, uint32_t lba, uint16_t count, uint8_t* buf) {
    struct ata_device* dev = &ata_devices[id];

    if (!dev->exists || count == 0 || count > 256) {
        return -1;
    }

    // Select device
    uint8_t cmd_select = 0xE0 | (id << 4);
    outb(dev->io_base + ATA_REG_HDDEVSEL, cmd_select | ((lba >> 24) & 0x0F));

    // Set block count
    outb(dev->io_base + ATA_REG_SECCOUNT0, count);

    // Set LBA
    outb(dev->io_base + ATA_REG_LBA0, lba & 0xFF);
    outb(dev->io_base + ATA_REG_LBA1, (lba >> 8) & 0xFF);
    outb(dev->io_base + ATA_REG_LBA2, (lba >> 16) & 0xFF);

    // Send WRITE SECTORS command
    outb(dev->io_base + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    // Write each sector
    for (int i = 0; i < count; i++) {
        // Wait for ready
        while (inb(dev->io_base + ATA_REG_STATUS) & ATA_SR_BSY);

        uint8_t status = inb(dev->io_base + ATA_REG_STATUS);
        if (!(status & ATA_SR_DRQ)) {
            return -1;  // Not ready for write
        }

        // Write 512 bytes
        for (int j = 0; j < 256; j++) {
            uint16_t data = buf[i*512 + j*2] |
                           (buf[i*512 + j*2 + 1] << 8);
            outw(dev->io_base + ATA_REG_DATA, data);
        }
    }

    return count;
}
```

---

## Status Register Bits

```c
#define ATA_SR_BSY   0x80  // Controller is executing a command
#define ATA_SR_DRDY  0x40  // Drive is ready
#define ATA_SR_DF    0x20  // Drive fault
#define ATA_SR_DSC   0x10  // Seek complete
#define ATA_SR_DRQ   0x08  // Data request
#define ATA_SR_CORR  0x04  // Correctable data
#define ATA_SR_IDX   0x02  // Index mark
#define ATA_SR_ERR   0x01  // Error occurred
```

---

## Error Handling

```c
int ata_check_error(int id) {
    struct ata_device* dev = &ata_devices[id];
    uint8_t status = inb(dev->io_base + ATA_REG_STATUS);

    if (status & ATA_SR_ERR) {
        uint8_t error = inb(dev->io_base + ATA_REG_ERROR);

        if (error & 0x10) return -1;  // ID not found
        if (error & 0x02) return -2;  // Write protection
        if (error & 0x04) return -3;  // Media changed
        if (error & 0x40) return -4;  // Uncorrectable error

        return -99;  // Unknown error
    }

    return 0;
}
```

---

## Partition Detection

AOS detects the partition offset to find FAT32:

```c
uint32_t ata_get_partition_offset(void) {
    uint8_t mbr_buf[512];

    // Read Master Boot Record (sector 0)
    if (ata_read_sectors(0, 0, 1, mbr_buf) != 1) {
        return 0;
    }

    // Check MBR signature
    if (mbr_buf[510] != 0x55 || mbr_buf[511] != 0xAA) {
        return 0;
    }

    // Read partition table entry 1 (offset 0x1CE)
    uint32_t* partition_entry = (uint32_t*)&mbr_buf[0x1CE];
    uint32_t start_sector = partition_entry[2];  // LBA start

    return start_sector * 512;  // Return byte offset
}
```

---

## Timing Considerations

ATA operations are slow (~50μs per sector in PIO mode):

```c
// Never busy-wait forever; use timeouts
uint32_t timeout = 10000;  // Iterations
while ((inb(dev->io_base + ATA_REG_STATUS) & ATA_SR_BSY) && timeout--) {
    // Busy wait
}

if (timeout == 0) {
    // Device timeout - may be hung
    return -1;
}
```

---

## Modern Alternative: AHCI

Modern drives use AHCI (Advanced Host Controller Interface), which:

- Uses memory-mapped registers instead of I/O ports
- Supports Native Command Queueing (NCQ)
- More complex but much faster

AOS includes `ahci.c` for AHCI support on modern systems.

---

## Integration with Filesystem

The ATA driver provides low-level sector I/O, which FAT32 uses:

```
FAT32 Filesystem
   ↓ (needs to read file data)
   ↓
ATA Driver
   ↓ (reads sectors)
   ↓
IDE Controller
   ↓
Hard Disk
```

---

## Common Issues

| Issue             | Cause                | Solution              |
| ----------------- | -------------------- | --------------------- |
| Device not found  | No drives on channel | Check IDE cable       |
| Read/write errors | Cable contact loose  | Reseat cable          |
| Corrupted data    | Buffer overflow      | Check DMA/PIO setting |
| Timeout           | Device hung          | Software reset needed |

---

## Performance Notes

- **PIO mode**: ~10-50 MB/s (CPU-bound)
- **DMA mode**: ~100+ MB/s (offloaded to controller)
- **AHCI mode**: ~300+ MB/s (with NCQ)

AOS uses PIO for simplicity; upgrading to DMA or AHCI would significantly improve throughput.

---

## Key Takeaways

✓ ATA/IDE uses I/O port addressing for disk control  
✓ LBA (Logical Block Addressing) for sector access  
✓ PIO mode reads/writes data directly via ports  
✓ Status register polling indicates operation progress  
✓ Error checking prevents silent data corruption  
✓ Partition offset found in Master Boot Record

---

## Related Components

- [FAT32 Filesystem](../filesystem/fat32.html)
- [AHCI Driver](ahci_driver.html)
- [PCI Driver](pci_driver.html)
- [Kernel I/O Operations](../interrupt_io/io_ports.html)

---

**Source Files:**

- `include/ata.h` - ATA definitions
- `src/drivers/ata.c` - ATA implementation
