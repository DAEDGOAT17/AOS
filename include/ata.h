#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include <stddef.h>

// ========================
// ATA PIO Driver Header
// Supports Primary Master drive via polling (no IRQ required).
// ========================

// ATA I/O port bases
#define ATA_PRIMARY_BASE    0x1F0
#define ATA_PRIMARY_CTRL    0x3F6
#define ATA_SECONDARY_BASE  0x170
#define ATA_SECONDARY_CTRL  0x376

// ATA Register offsets from base
#define ATA_REG_DATA        0x00
#define ATA_REG_ERROR       0x01
#define ATA_REG_FEATURES    0x01
#define ATA_REG_SECCOUNT    0x02
#define ATA_REG_LBA0        0x03
#define ATA_REG_LBA1        0x04
#define ATA_REG_LBA2        0x05
#define ATA_REG_HDDEVSEL    0x06
#define ATA_REG_COMMAND     0x07
#define ATA_REG_STATUS      0x07

// ATA Commands
#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_IDENTIFY    0xEC

// ATA Status bits
#define ATA_SR_BSY  0x80   // Busy
#define ATA_SR_DRDY 0x40   // Drive Ready
#define ATA_SR_DRQ  0x08   // Data Request Ready
#define ATA_SR_ERR  0x01   // Error

// Sector size
#define ATA_SECTOR_SIZE 512

// Initialize ATA driver - returns 1 if drive found, 0 otherwise
// Initialize storage. Returns 0 on success, -1 on failure.
int ata_init(void);

// Get the LBA where a valid FAT32 filesystem was detected (0 for ramdisk or raw disk)
uint32_t ata_get_partition_offset(void);

// Returns 1 if we are using the internal ramdisk, 0 if using a real drive
int ata_is_ramdisk(void);

// Returns 1 if storage I/O is using the AHCI driver, 0 otherwise
int ata_is_ahci(void);

// Read 'count' sectors starting at LBA 'lba' into buffer
// Returns 0 on success, -1 on error
int ata_read_sectors(uint32_t lba, uint8_t count, void* buffer);

// Write 'count' sectors starting at LBA 'lba' from buffer
// Returns 0 on success, -1 on error
int ata_write_sectors(uint32_t lba, uint8_t count, const void* buffer);

// Flush volatile device write caches before reporting a persistent install.
int ata_flush_cache(void);

// Get total sector count of the drive (from IDENTIFY data)
uint32_t ata_get_sector_count(void);

#endif // ATA_H
