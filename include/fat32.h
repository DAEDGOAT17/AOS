#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include <stddef.h>

// ========================
// FAT32 File System Header
// ========================

// --- On-disk structures (packed, match FAT32 spec exactly) ---

// BIOS Parameter Block + FAT32 extended BPB
typedef struct __attribute__((packed)) {
    uint8_t  jump[3];           // 0x00 Boot jump
    uint8_t  oem[8];            // 0x03 OEM name
    uint16_t bytes_per_sector;  // 0x0B
    uint8_t  sectors_per_cluster; // 0x0D
    uint16_t reserved_sectors;  // 0x0E
    uint8_t  num_fats;          // 0x10
    uint16_t root_entry_count;  // 0x11 (0 for FAT32)
    uint16_t total_sectors_16;  // 0x13 (0 for FAT32)
    uint8_t  media_type;        // 0x15
    uint16_t fat_size_16;       // 0x16 (0 for FAT32)
    uint16_t sectors_per_track; // 0x18
    uint16_t num_heads;         // 0x1A
    uint32_t hidden_sectors;    // 0x1C
    uint32_t total_sectors_32;  // 0x20

    // FAT32 extended BPB (starts at 0x24)
    uint32_t fat_size_32;       // 0x24 sectors per FAT
    uint16_t ext_flags;         // 0x28
    uint16_t fs_version;        // 0x2A
    uint32_t root_cluster;      // 0x2C cluster of root dir
    uint16_t fs_info;           // 0x30 FSInfo sector number
    uint16_t backup_boot_sector;// 0x32
    uint8_t  reserved[12];      // 0x34
    uint8_t  drive_number;      // 0x40
    uint8_t  reserved1;         // 0x41
    uint8_t  boot_signature;    // 0x42
    uint32_t volume_id;         // 0x43
    uint8_t  volume_label[11];  // 0x47
    uint8_t  fs_type[8];        // 0x52 "FAT32   "
} fat32_bpb_t;

// Directory entry (32 bytes)
typedef struct __attribute__((packed)) {
    uint8_t  name[8];           // 8.3 filename, space-padded
    uint8_t  ext[3];            // Extension, space-padded
    uint8_t  attributes;        // File attributes
    uint8_t  reserved;
    uint8_t  create_time_tenth; // Tenths of second at creation
    uint16_t create_time;
    uint16_t create_date;
    uint16_t last_access_date;
    uint16_t first_cluster_high;// High 16 bits of first cluster
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_low; // Low 16 bits of first cluster
    uint32_t file_size;         // File size in bytes
} fat32_dir_entry_t;

// File attributes
#define FAT_ATTR_READONLY   0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20
#define FAT_ATTR_LFN        0x0F   // Long filename entry

// Special cluster values
#define FAT32_EOC           0x0FFFFFF8  // End Of Chain
#define FAT32_FREE          0x00000000
#define FAT32_BAD           0x0FFFFFF7

// Max path/name lengths
#define FAT32_MAX_FILENAME  12    // 8 + '.' + 3 + null
#define FAT32_MAX_PATH      256

// File handle
typedef struct {
    int      valid;                       // 1 = open
    uint32_t first_cluster;              // Starting cluster
    uint32_t current_cluster;            // Current cluster during read/write
    uint32_t cluster_offset;             // Byte offset within current cluster
    uint32_t file_size;                  // Total file size
    uint32_t position;                   // Current read/write position
    uint8_t  is_dir;                     // 1 = directory
    uint32_t dir_cluster;                // Parent directory cluster
    uint32_t dir_entry_index;            // Index of this entry in parent dir
    char     name[FAT32_MAX_FILENAME];
} fat32_file_t;

#define FAT32_MAX_OPEN_FILES 8

// --- Public API ---

// Mount the FAT32 filesystem on the primary ATA drive
// lba_start: first LBA of the FAT32 partition (0 for raw disk)
// Returns 0 on success
int fat32_mount(uint32_t lba_start);

// Returns 1 if filesystem is mounted
int fat32_is_mounted(void);

// Get volume label (null-terminated, max 12 chars)
void fat32_get_label(char* buf);

// List directory (cluster=0 means current dir)
// Calls callback for each entry
typedef void (*fat32_dir_callback_t)(const char* name, uint8_t attr, uint32_t size, uint32_t cluster);
int fat32_list_dir(uint32_t cluster, fat32_dir_callback_t cb);

// Open a file/dir by path from the root
// mode: 'r' = read, 'w' = write/create, 'a' = append
// Returns file handle index (>=0) or -1 on error
int fat32_open(const char* path, char mode);

// Close file handle
void fat32_close(int fd);

// Read up to 'len' bytes from file handle 'fd' into buf
// Returns bytes read, 0 at EOF, -1 on error
int fat32_read(int fd, void* buf, uint32_t len);

// Write 'len' bytes from buf into file handle 'fd'
// Returns bytes written, -1 on error
int fat32_write(int fd, const void* buf, uint32_t len);

// Seek to absolute position
int fat32_seek(int fd, uint32_t pos);

// Read 'len' bytes from 'offset' without modifying the file's seek position.
// Safe to call from interrupt context (fully reentrant, local vars only).
int fat32_read_at(int fd, uint32_t offset, void* buf, uint32_t len);

// Get file size
uint32_t fat32_get_size(int fd);

// Create directory (absolute path from root)
int fat32_mkdir(const char* path);

// Delete file (absolute path from root)
int fat32_unlink(const char* path);

// Delete directory (must be empty)
int fat32_rmdir(const char* path);

// Resolve a path, return starting cluster (0 = not found)
uint32_t fat32_resolve_path(const char* path);

// Shell helper: list files in cluster, printing to screen
void fat32_ls(uint32_t cluster);

// Current working directory cluster (0 = root)
extern uint32_t fat32_cwd_cluster;

// Change directory, returns 0 on success
int fat32_chdir(const char* path);

// Print working directory path
void fat32_print_cwd(void);

#endif // FAT32_H
