#include "fat32.h"
#include "ata.h"
#include "kmalloc.h"
#include "string.h"
#include "screen.h"

static fat32_bpb_t bpb;
static uint32_t partition_lba = 0;
static uint32_t fat_lba = 0;
static uint32_t data_lba = 0;
static int mounted = 0;

static fat32_file_t open_files[FAT32_MAX_OPEN_FILES];

uint32_t fat32_cwd_cluster = 0;
char fat32_cwd_path[FAT32_MAX_PATH] = "/";

// --- Internal Helpers ---

static uint32_t cluster_to_lba(uint32_t cluster) {
    if (cluster < 2) cluster = bpb.root_cluster;
    return data_lba + (cluster - 2) * bpb.sectors_per_cluster;
}

static uint32_t get_next_cluster(uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fat_lba + (fat_offset / 512);
    uint32_t ent_offset = fat_offset % 512;

    uint8_t sector_buf[512];
    if (ata_read_sectors(fat_sector, 1, sector_buf) != 0) return FAT32_BAD;

    uint32_t next_cluster = (*(uint32_t*)&sector_buf[ent_offset]) & 0x0FFFFFFF;
    return (next_cluster >= 0x0FFFFFF8) ? FAT32_EOC : next_cluster;
}

static void set_next_cluster(uint32_t cluster, uint32_t next) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fat_lba + (fat_offset / 512);
    uint32_t ent_offset = fat_offset % 512;

    uint8_t sector_buf[512];
    ata_read_sectors(fat_sector, 1, sector_buf);
    *(uint32_t*)&sector_buf[ent_offset] = next;
    ata_write_sectors(fat_sector, 1, sector_buf);
}

static uint32_t find_free_cluster() {
    uint8_t sector_buf[512];
    for (uint32_t s = 0; s < bpb.fat_size_32; s++) {
        ata_read_sectors(fat_lba + s, 1, sector_buf);
        uint32_t* fat = (uint32_t*)sector_buf;
        for (int i = 0; i < 128; i++) {
            if ((fat[i] & 0x0FFFFFFF) == 0) {
                uint32_t cluster = s * 128 + i;
                if (cluster >= 2) return cluster;
            }
        }
    }
    return 0;
}

// Convert 8.3 name to readable string
static void format_name(const uint8_t* fat_name, const uint8_t* fat_ext, char* dest) {
    int p = 0;
    for (int i = 0; i < 8; i++) {
        if (fat_name[i] != ' ') dest[p++] = fat_name[i];
    }
    if (fat_ext[0] != ' ') {
        dest[p++] = '.';
        for (int i = 0; i < 3; i++) {
            if (fat_ext[i] != ' ') dest[p++] = fat_ext[i];
        }
    }
    dest[p] = '\0';
}

// Standardize a name into 8.3 format for searching
static void standardize_name(const char* src, uint8_t* dest_name, uint8_t* dest_ext) {
    memset(dest_name, ' ', 8);
    memset(dest_ext, ' ', 3);
    
    int i = 0;
    while (src[i] && src[i] != '.' && i < 8) {
        dest_name[i] = src[i];
        if (dest_name[i] >= 'a' && dest_name[i] <= 'z') dest_name[i] -= 32;
        i++;
    }
    
    const char* dot = strstr(src, ".");
    if (dot) {
        dot++;
        int j = 0;
        while (dot[j] && j < 3) {
            dest_ext[j] = dot[j];
            if (dest_ext[j] >= 'a' && dest_ext[j] <= 'z') dest_ext[j] -= 32;
            j++;
        }
    }
}

// Create a directory entry in the specified parent cluster
static int create_entry(uint32_t parent_cluster, const char* name, uint8_t attr, uint32_t first_cluster, uint32_t size) {
    uint8_t name_8[8], ext_3[3];
    standardize_name(name, name_8, ext_3);

    uint32_t current_cluster = parent_cluster;
    uint8_t sector_buf[512];

    while (current_cluster != FAT32_EOC) {
        uint32_t lba = cluster_to_lba(current_cluster);
        for (int s = 0; s < bpb.sectors_per_cluster; s++) {
            ata_read_sectors(lba + s, 1, sector_buf);
            fat32_dir_entry_t* entries = (fat32_dir_entry_t*)sector_buf;
            for (size_t i = 0; i < 512 / sizeof(fat32_dir_entry_t); i++) {
                if (entries[i].name[0] == 0x00 || entries[i].name[0] == 0xE5) {
                    // Found a slot
                    memcpy(entries[i].name, name_8, 8);
                    memcpy(entries[i].ext, ext_3, 3);
                    entries[i].attributes = attr;
                    entries[i].first_cluster_high = (first_cluster >> 16) & 0xFFFF;
                    entries[i].first_cluster_low = first_cluster & 0xFFFF;
                    entries[i].file_size = size;
                    ata_write_sectors(lba + s, 1, sector_buf);
                    return 0;
                }
            }
        }
        uint32_t next = get_next_cluster(current_cluster);
        if (next == FAT32_EOC) {
            uint32_t extra = find_free_cluster();
            if (!extra) return -1;
            set_next_cluster(current_cluster, extra);
            set_next_cluster(extra, FAT32_EOC);
            memset(sector_buf, 0, 512);
            for(int s=0; s<bpb.sectors_per_cluster; s++) ata_write_sectors(cluster_to_lba(extra)+s, 1, sector_buf);
            next = extra;
        }
        current_cluster = next;
    }
    return -1;
}

int fat32_mkdir(const char* path) {
    if (!mounted) return -1;

    // Separate parent path and new directory name
    char parent_path[FAT32_MAX_PATH];
    char dir_name[FAT32_MAX_FILENAME];
    strcpy(parent_path, path);
    
    char* last_slash = 0;
    for(int i=0; parent_path[i]; i++) if(parent_path[i] == '/') last_slash = &parent_path[i];
    
    uint32_t parent_cluster;
    if (!last_slash) {
        parent_cluster = fat32_cwd_cluster;
        strcpy(dir_name, path);
    } else {
        if (last_slash == parent_path) {
            parent_cluster = bpb.root_cluster;
        } else {
            *last_slash = '\0';
            parent_cluster = fat32_resolve_path(parent_path);
        }
        strcpy(dir_name, last_slash + 1);
    }

    if (parent_cluster == 0 && last_slash != parent_path) return -1;
    if (parent_cluster == 0) parent_cluster = bpb.root_cluster;

    // Check if folder already exists
    if (fat32_resolve_path(path) != 0) return -1;

    uint32_t new_cluster = find_free_cluster();
    if (!new_cluster) return -1;
    set_next_cluster(new_cluster, FAT32_EOC);

    // Initialize directory cluster with . and ..
    uint8_t sector_buf[512];
    memset(sector_buf, 0, 512);
    fat32_dir_entry_t* entries = (fat32_dir_entry_t*)sector_buf;
    
    // "." entry
    memset(entries[0].name, ' ', 11);
    entries[0].name[0] = '.';
    entries[0].attributes = FAT_ATTR_DIRECTORY;
    entries[0].first_cluster_high = (new_cluster >> 16) & 0xFFFF;
    entries[0].first_cluster_low = new_cluster & 0xFFFF;
    
    // ".." entry
    memset(entries[1].name, ' ', 11);
    entries[1].name[0] = '.';
    entries[1].name[1] = '.';
    entries[1].attributes = FAT_ATTR_DIRECTORY;
    uint32_t dot_dot_cluster = (parent_cluster == bpb.root_cluster) ? 0 : parent_cluster;
    entries[1].first_cluster_high = (dot_dot_cluster >> 16) & 0xFFFF;
    entries[1].first_cluster_low = dot_dot_cluster & 0xFFFF;

    uint32_t lba = cluster_to_lba(new_cluster);
    ata_write_sectors(lba, 1, sector_buf);
    // Zero out the rest of the cluster
    memset(sector_buf, 0, 512);
    for(int s=1; s < bpb.sectors_per_cluster; s++) ata_write_sectors(lba + s, 1, sector_buf);

    // Create entry in parent
    if (create_entry(parent_cluster, dir_name, FAT_ATTR_DIRECTORY, new_cluster, 0) != 0) return -1;

    return 0;
}

// Helper for recursive removal
static int recursive_rm(uint32_t cluster) {
    uint32_t current_cluster = cluster;
    uint8_t sector_buf[512];

    while (current_cluster != FAT32_EOC && current_cluster != FAT32_BAD && current_cluster != 0) {
        uint32_t lba = cluster_to_lba(current_cluster);
        for (int s = 0; s < bpb.sectors_per_cluster; s++) {
            ata_read_sectors(lba + s, 1, sector_buf);
            fat32_dir_entry_t* entries = (fat32_dir_entry_t*)sector_buf;
            for (size_t i = 0; i < 512 / sizeof(fat32_dir_entry_t); i++) {
                if (entries[i].name[0] == 0x00) break;
                if (entries[i].name[0] == 0xE5) continue;
                if (entries[i].name[0] == '.') continue;
                if (entries[i].attributes == FAT_ATTR_LFN) continue;

                uint32_t child_cluster = ((uint32_t)entries[i].first_cluster_high << 16) | entries[i].first_cluster_low;
                if (entries[i].attributes & FAT_ATTR_DIRECTORY) {
                    recursive_rm(child_cluster);
                } else {
                    // Free file cluster chain
                    uint32_t f_cluster = child_cluster;
                    while (f_cluster != 0 && f_cluster < 0x0FFFFFF8) {
                        uint32_t next = get_next_cluster(f_cluster);
                        set_next_cluster(f_cluster, FAT32_FREE);
                        f_cluster = next;
                    }
                }
            }
        }
        uint32_t next = get_next_cluster(current_cluster);
        set_next_cluster(current_cluster, FAT32_FREE);
        current_cluster = next;
    }
    return 0;
}

int fat32_rmdir(const char* path) {
    if (!mounted) return -1;
    // Don't allow removing root
    if (strcmp(path, "/") == 0) return -1;

    uint32_t cluster = fat32_resolve_path(path);
    if (cluster == 0) return -1;

    // Find parent and entry to mark as deleted
    char parent_path[FAT32_MAX_PATH];
    char dir_name[FAT32_MAX_FILENAME];
    strcpy(parent_path, path);
    char* last_slash = 0;
    for(int i=0; parent_path[i]; i++) if(parent_path[i] == '/') last_slash = &parent_path[i];

    uint32_t parent_cluster;
    if (!last_slash) {
        parent_cluster = fat32_cwd_cluster;
        strcpy(dir_name, path);
    } else {
        if (last_slash == parent_path) parent_cluster = bpb.root_cluster;
        else {
            *last_slash = '\0';
            parent_cluster = fat32_resolve_path(parent_path);
        }
        strcpy(dir_name, last_slash + 1);
    }

    // Recurse and free chain
    recursive_rm(cluster);

    // Mark as deleted in parent
    uint8_t name_8[8], ext_3[3];
    standardize_name(dir_name, name_8, ext_3);
    
    uint32_t search_cluster = parent_cluster;
    int found = 0;
    while (search_cluster != FAT32_EOC && !found) {
        uint32_t lba = cluster_to_lba(search_cluster);
        uint8_t sector_buf[512];
        for (int s = 0; s < bpb.sectors_per_cluster && !found; s++) {
            ata_read_sectors(lba + s, 1, sector_buf);
            fat32_dir_entry_t* edentries = (fat32_dir_entry_t*)sector_buf;
            for (size_t i = 0; i < 512 / sizeof(fat32_dir_entry_t); i++) {
                if (edentries[i].name[0] == 0x00) break;
                if (edentries[i].name[0] == 0xE5) continue;
                if (memcmp(edentries[i].name, name_8, 8) == 0 && memcmp(edentries[i].ext, ext_3, 3) == 0) {
                    edentries[i].name[0] = 0xE5;
                    ata_write_sectors(lba + s, 1, sector_buf);
                    found = 1;
                    break;
                }
            }
        }
        if (!found) search_cluster = get_next_cluster(search_cluster);
    }

    return 0;
}

// --- Public API ---

int fat32_mount(uint32_t lba_start) {
    partition_lba = lba_start;
    uint8_t sector0[512];
    
    if (ata_read_sectors(partition_lba, 1, sector0) != 0) return -1;
    
    memcpy(&bpb, sector0, sizeof(fat32_bpb_t));
    
    print_string("FAT32: Attempting mount at LBA ");
    kprint_dec(partition_lba);
    print_string("\nFAT32: OEM: ");
    for(int i=0; i<8; i++) print_char(bpb.oem[i]);
    print_string(" | Sig: 0x");
    kprint_hex(bpb.boot_signature);
    print_char('\n');

    if (bpb.boot_signature != 0x29 && bpb.boot_signature != 0x28) {
        print_string("FAT32: [WARN] Non-standard boot signature found. Proceeding anyway.\n");
    }

    if (bpb.bytes_per_sector != 512) {
        print_string("FAT32: [ERROR] Only 512 byte sectors supported. Found: ");
        kprint_dec(bpb.bytes_per_sector);
        print_char('\n');
        return -1;
    }

    fat_lba = partition_lba + bpb.reserved_sectors;
    data_lba = fat_lba + (bpb.num_fats * bpb.fat_size_32);
    
    fat32_cwd_cluster = bpb.root_cluster;
    strcpy(fat32_cwd_path, "/");
    mounted = 1;

    for (int i = 0; i < FAT32_MAX_OPEN_FILES; i++) open_files[i].valid = 0;

    print_string("FAT32: Mounted successfully. Root Cluster: ");
    kprint_dec(bpb.root_cluster);
    print_char('\n');
    
    return 0;
}

int fat32_is_mounted(void) { return mounted; }

int fat32_list_dir(uint32_t cluster, fat32_dir_callback_t cb) {
    if (!mounted) return -1;
    if (cluster == 0) cluster = fat32_cwd_cluster;
    if (cluster == 0) cluster = bpb.root_cluster; // absolute fallback

    uint32_t current_cluster = cluster;
    uint8_t sector_buf[512];

    while (current_cluster != FAT32_EOC && current_cluster != FAT32_BAD) {
        uint32_t lba = cluster_to_lba(current_cluster);
        
        for (int s = 0; s < bpb.sectors_per_cluster; s++) {
            if (ata_read_sectors(lba + s, 1, sector_buf) != 0) return -1;
            
            fat32_dir_entry_t* entries = (fat32_dir_entry_t*)sector_buf;
            for (size_t i = 0; i < 512 / sizeof(fat32_dir_entry_t); i++) {
                if (entries[i].name[0] == 0x00) return 0;
                if (entries[i].name[0] == 0xE5) continue;
                if (entries[i].attributes == FAT_ATTR_LFN) continue;

                char name[13];
                format_name(entries[i].name, entries[i].ext, name);
                
                uint32_t ent_cluster = ((uint32_t)entries[i].first_cluster_high << 16) | entries[i].first_cluster_low;
                cb(name, entries[i].attributes, entries[i].file_size, ent_cluster);
            }
        }
        current_cluster = get_next_cluster(current_cluster);
    }
    return 0;
}

uint32_t fat32_resolve_path(const char* path) {
    if (!mounted) return 0;
    if (path[0] == '\0') return bpb.root_cluster;

    uint32_t current_cluster = bpb.root_cluster;
    if (path[0] != '/') current_cluster = fat32_cwd_cluster;

    char temp_path[FAT32_MAX_PATH];
    strcpy(temp_path, path);
    char* part = temp_path;
    if (*part == '/') part++;

    while (*part) {
        char* next_slash = strstr(part, "/");
        if (next_slash) *next_slash = '\0';

        uint32_t search_cluster = current_cluster;
        uint32_t next_cluster = 0;
        uint8_t name_8[8], ext_3[3];
        standardize_name(part, name_8, ext_3);

        int found = 0;
        while (search_cluster != FAT32_EOC && !found) {
            uint32_t lba = cluster_to_lba(search_cluster);
            uint8_t sector_buf[512];
            for (int s = 0; s < bpb.sectors_per_cluster && !found; s++) {
                ata_read_sectors(lba + s, 1, sector_buf);
                fat32_dir_entry_t* entries = (fat32_dir_entry_t*)sector_buf;
                for (size_t i = 0; i < 512 / sizeof(fat32_dir_entry_t); i++) {
                    if (entries[i].name[0] == 0x00) break;
                    if (entries[i].name[0] == 0xE5) continue;
                    if (entries[i].attributes == FAT_ATTR_LFN) continue;
                    if (memcmp(entries[i].name, name_8, 8) == 0 && memcmp(entries[i].ext, ext_3, 3) == 0) {
                        next_cluster = ((uint32_t)entries[i].first_cluster_high << 16) | entries[i].first_cluster_low;
                        found = 1;
                        break;
                    }
                }
            }
            if (!found) search_cluster = get_next_cluster(search_cluster);
        }
        if (!found) return 0;
        current_cluster = next_cluster;
        if (!next_slash) break;
        part = next_slash + 1;
    }
    return current_cluster;
}
int fat32_open(const char* path, char mode) {
    uint32_t target_cluster = fat32_resolve_path(path);
    uint32_t parent_cluster = 0;
    char parent_path[FAT32_MAX_PATH];
    char filename[FAT32_MAX_FILENAME];
    const char* target_filename = path;

    // Resolve the parent directory for nested paths first.
    strcpy(parent_path, path);
    char* last_slash = NULL;
    for (int i = 0; parent_path[i]; i++) {
        if (parent_path[i] == '/') last_slash = &parent_path[i];
    }
    if (last_slash) {
        if (last_slash == parent_path) {
            parent_cluster = bpb.root_cluster;
            strcpy(filename, last_slash + 1);
            target_filename = filename;
        } else {
            *last_slash = '\0';
            parent_cluster = fat32_resolve_path(parent_path);
            strcpy(filename, last_slash + 1);
            target_filename = filename;
        }
    } else {
        parent_cluster = fat32_cwd_cluster;
        if (path[0] == '/') parent_cluster = bpb.root_cluster;
        strcpy(filename, path);
        target_filename = filename;
    }
    if (parent_cluster == 0) parent_cluster = bpb.root_cluster;

    // Special handling for write mode if file doesn't exist.
    if (target_cluster == 0 && mode == 'w') {
        target_cluster = find_free_cluster();
        if (!target_cluster) return -1;
        set_next_cluster(target_cluster, FAT32_EOC);

        uint8_t zero_buf[512];
        memset(zero_buf, 0, 512);
        uint32_t start_lba = cluster_to_lba(target_cluster);
        for (int s = 0; s < bpb.sectors_per_cluster; s++) {
            ata_write_sectors(start_lba + s, 1, zero_buf);
        }

        if (create_entry(parent_cluster, filename, FAT_ATTR_ARCHIVE, target_cluster, 0) != 0) return -1;
        target_cluster = fat32_resolve_path(path);
    }

    if (target_cluster == 0) return -1;

    int fd = -1;
    for (int i = 0; i < FAT32_MAX_OPEN_FILES; i++) {
        if (!open_files[i].valid) { fd = i; break; }
    }
    if (fd == -1) return -1;

    open_files[fd].valid = 1;
    open_files[fd].first_cluster = target_cluster;
    open_files[fd].current_cluster = target_cluster;
    open_files[fd].position = 0;
    open_files[fd].cluster_offset = 0;
    open_files[fd].file_size = 0;

    strncpy(open_files[fd].name, target_filename, FAT32_MAX_FILENAME - 1);
    open_files[fd].name[FAT32_MAX_FILENAME - 1] = '\0';

    uint32_t search_cluster = parent_cluster;
    uint8_t name_8[8], ext_3[3];
    standardize_name(target_filename, name_8, ext_3);

    int found = 0;
    while (search_cluster != FAT32_EOC && !found) {
        uint32_t lba = cluster_to_lba(search_cluster);
        uint8_t sector_buf[512];
        for (int s = 0; s < bpb.sectors_per_cluster && !found; s++) {
            ata_read_sectors(lba + s, 1, sector_buf);
            fat32_dir_entry_t* entries = (fat32_dir_entry_t*)sector_buf;
            for (size_t i = 0; i < 512 / sizeof(fat32_dir_entry_t); i++) {
                if (entries[i].name[0] == 0x00) break;
                if (entries[i].name[0] == 0xE5) continue;
                if (memcmp(entries[i].name, name_8, 8) == 0 && memcmp(entries[i].ext, ext_3, 3) == 0) {
                    if (mode == 'w') {
                        entries[i].file_size = 0;
                        open_files[fd].file_size = 0;
                        ata_write_sectors(lba + s, 1, sector_buf);
                    } else {
                        open_files[fd].file_size = entries[i].file_size;
                    }
                    open_files[fd].dir_cluster = search_cluster;
                    open_files[fd].dir_entry_index = (s * (512 / sizeof(fat32_dir_entry_t))) + i;
                    found = 1;
                    break;
                }
            }
        }
        if (!found) search_cluster = get_next_cluster(search_cluster);
    }

    if (!found) {
        open_files[fd].valid = 0;
        return -1;
    }

    return fd;
}

uint32_t fat32_get_size(int fd) {
    if (fd < 0 || fd >= FAT32_MAX_OPEN_FILES || !open_files[fd].valid) return 0;
    return open_files[fd].file_size;
}

int fat32_seek(int fd, uint32_t pos) {
    if (fd < 0 || fd >= FAT32_MAX_OPEN_FILES || !open_files[fd].valid) return -1;
    fat32_file_t* file = &open_files[fd];
    if (pos > file->file_size) pos = file->file_size;

    uint32_t cluster_size = bpb.sectors_per_cluster * 512;
    uint32_t target_cluster_idx = pos / cluster_size;

    // Walk chain to seek
    file->current_cluster = file->first_cluster;
    for (uint32_t i = 0; i < target_cluster_idx; i++) {
        if (file->current_cluster == FAT32_EOC || file->current_cluster == FAT32_BAD) break;
        file->current_cluster = get_next_cluster(file->current_cluster);
    }
    file->cluster_offset = pos % cluster_size;
    file->position = pos;
    return 0;
}

// Read 'len' bytes from 'offset' in file, fully reentrant — safe inside page fault handler.
// Uses ONLY local stack variables. Does not touch open_files[] position/cluster state.
int fat32_read_at(int fd, uint32_t offset, void* buf, uint32_t len) {
    if (fd < 0 || fd >= FAT32_MAX_OPEN_FILES || !open_files[fd].valid) return -1;
    fat32_file_t* file = &open_files[fd];
    if (offset >= file->file_size) return 0;
    if (offset + len > file->file_size) len = file->file_size - offset;

    uint32_t cluster_size = bpb.sectors_per_cluster * 512;
    uint32_t target_cluster_idx = offset / cluster_size;
    uint32_t cluster_off = offset % cluster_size;

    // Walk chain locally -- no modification of file state
    uint32_t cur_cluster = file->first_cluster;
    for (uint32_t i = 0; i < target_cluster_idx; i++) {
        if (cur_cluster == FAT32_EOC || cur_cluster == FAT32_BAD) return 0;
        cur_cluster = get_next_cluster(cur_cluster);
    }

    uint32_t bytes_read = 0;
    uint8_t sector_buf[512];

    while (bytes_read < len) {
        if (cur_cluster == FAT32_EOC || cur_cluster == FAT32_BAD) break;
        uint32_t sector_in_cluster = cluster_off / 512;
        uint32_t off_in_sector     = cluster_off % 512;
        uint32_t can_read = 512 - off_in_sector;
        if (can_read > (len - bytes_read)) can_read = len - bytes_read;

        uint32_t lba = cluster_to_lba(cur_cluster) + sector_in_cluster;
        if (ata_read_sectors(lba, 1, sector_buf) != 0) break;

        uint8_t* dst = (uint8_t*)buf + bytes_read;
        for (uint32_t i = 0; i < can_read; i++) dst[i] = sector_buf[off_in_sector + i];

        bytes_read  += can_read;
        cluster_off += can_read;
        if (cluster_off >= cluster_size) {
            cur_cluster = get_next_cluster(cur_cluster);
            cluster_off = 0;
        }
    }
    return (int)bytes_read;
}

void fat32_close(int fd) {
    if (fd >= 0 && fd < FAT32_MAX_OPEN_FILES) open_files[fd].valid = 0;
}

int fat32_read(int fd, void* buf, uint32_t len) {
    if (fd < 0 || fd >= FAT32_MAX_OPEN_FILES || !open_files[fd].valid) return -1;
    fat32_file_t* file = &open_files[fd];
    
    // Don't read past EOF
    if (file->position >= file->file_size) return 0;
    if (file->position + len > file->file_size) len = file->file_size - file->position;

    uint32_t bytes_read = 0;
    uint8_t sector_buf[512];
    uint32_t cluster_size = bpb.sectors_per_cluster * 512;
    while (bytes_read < len) {
        if (file->current_cluster == FAT32_EOC) break;
        uint32_t sector_in_cluster = file->cluster_offset / 512;
        uint32_t offset_in_sector = file->cluster_offset % 512;
        uint32_t can_read = 512 - offset_in_sector;
        if (can_read > (len - bytes_read)) can_read = (len - bytes_read);
        
        uint32_t lba = cluster_to_lba(file->current_cluster) + sector_in_cluster;
        if (ata_read_sectors(lba, 1, sector_buf) != 0) break;
        memcpy((uint8_t*)buf + bytes_read, sector_buf + offset_in_sector, can_read);
        bytes_read += can_read;
        file->position += can_read;
        file->cluster_offset += can_read;
        if (file->cluster_offset >= cluster_size) {
            file->current_cluster = get_next_cluster(file->current_cluster);
            file->cluster_offset = 0;
        }
    }
    return bytes_read;
}

int fat32_write(int fd, const void* buf, uint32_t len) {
    if (fd < 0 || fd >= FAT32_MAX_OPEN_FILES || !open_files[fd].valid) return -1;
    fat32_file_t* file = &open_files[fd];
    uint32_t bytes_written = 0;
    uint8_t sector_buf[512];
    uint32_t cluster_size = bpb.sectors_per_cluster * 512;
    while (bytes_written < len) {
        uint32_t sector_in_cluster = file->cluster_offset / 512;
        uint32_t offset_in_sector = file->cluster_offset % 512;
        uint32_t can_write = 512 - offset_in_sector;
        if (can_write > (len - bytes_written)) can_write = (len - bytes_written);
        uint32_t lba = cluster_to_lba(file->current_cluster) + sector_in_cluster;
        if (can_write < 512) ata_read_sectors(lba, 1, sector_buf);
        memcpy(sector_buf + offset_in_sector, (uint8_t*)buf + bytes_written, can_write);
        ata_write_sectors(lba, 1, sector_buf);
        bytes_written += can_write;
        file->position += can_write;
        file->cluster_offset += can_write;
        if (file->cluster_offset >= cluster_size) {
            uint32_t next = get_next_cluster(file->current_cluster);
            if (next == FAT32_EOC) {
                next = find_free_cluster();
                if (!next) break;
                set_next_cluster(file->current_cluster, next);
                set_next_cluster(next, FAT32_EOC);
                
                uint8_t zero_buf[512];
                memset(zero_buf, 0, 512);
                uint32_t nlba = cluster_to_lba(next);
                for(int s = 0; s < bpb.sectors_per_cluster; s++) {
                    ata_write_sectors(nlba + s, 1, zero_buf);
                }
            }
            file->current_cluster = next;
            file->cluster_offset = 0;
        }
    }
    
    // Update directory entry with new size
    if (bytes_written > 0 && file->position > file->file_size) {
        file->file_size = file->position;
        uint32_t search_cluster = file->dir_cluster;
        uint32_t entry_idx = file->dir_entry_index;
        
        uint32_t entries_per_sector = 512 / sizeof(fat32_dir_entry_t);
        // Wait, index is relative to start of cluster or global?
        // My index logic in OPEN was: (s * (512/sizeof)) + i; where s is sector in cluster.
        // So entry_idx is indeed relative to cluster start.
        
        uint32_t sector_offset = (entry_idx / entries_per_sector);
        uint32_t idx_in_sector = (entry_idx % entries_per_sector);
        
        uint32_t lba = cluster_to_lba(search_cluster) + sector_offset;
        uint8_t sector_buf[512];
        ata_read_sectors(lba, 1, sector_buf);
        fat32_dir_entry_t* entries = (fat32_dir_entry_t*)sector_buf;
        entries[idx_in_sector].file_size = file->file_size;
        ata_write_sectors(lba, 1, sector_buf);
    }

    return bytes_written;
}

int fat32_unlink(const char* path) {
    if (!mounted) return -1;
    
    uint8_t name_8[8], ext_3[3];
    standardize_name(path, name_8, ext_3);
    
    uint32_t parent_cluster = fat32_cwd_cluster;
    if (path[0] == '/') parent_cluster = bpb.root_cluster;
    
    uint32_t current_cluster = parent_cluster;
    uint8_t sector_buf[512];
    int found = 0;
    
    while (current_cluster != FAT32_EOC && !found) {
        uint32_t lba = cluster_to_lba(current_cluster);
        for (int s = 0; s < bpb.sectors_per_cluster && !found; s++) {
            ata_read_sectors(lba + s, 1, sector_buf);
            fat32_dir_entry_t* entries = (fat32_dir_entry_t*)sector_buf;
            for (size_t i = 0; i < 512 / sizeof(fat32_dir_entry_t); i++) {
                if (entries[i].name[0] == 0x00) break;
                if (entries[i].name[0] == 0xE5) continue;
                
                if (memcmp(entries[i].name, name_8, 8) == 0 && memcmp(entries[i].ext, ext_3, 3) == 0) {
                    if (entries[i].attributes & FAT_ATTR_DIRECTORY) return -1; // Use rmdir
                    
                    uint32_t cluster_to_free = ((uint32_t)entries[i].first_cluster_high << 16) | entries[i].first_cluster_low;
                    
                    // Mark as deleted
                    entries[i].name[0] = 0xE5;
                    ata_write_sectors(lba + s, 1, sector_buf);
                    
                    // Free cluster chain
                    while (cluster_to_free != 0 && cluster_to_free < 0x0FFFFFF8) {
                        uint32_t next = get_next_cluster(cluster_to_free);
                        set_next_cluster(cluster_to_free, FAT32_FREE);
                        cluster_to_free = next;
                    }
                    
                    found = 1;
                    break;
                }
            }
        }
        if (!found) current_cluster = get_next_cluster(current_cluster);
    }
    
    return found ? 0 : -1;
}

int fat32_chdir(const char* path) {
    if (!path || path[0] == '\0') return -1;
    if (strcmp(path, ".") == 0) return 0;
    if (strcmp(path, "/") == 0) {
        strcpy(fat32_cwd_path, "/");
        fat32_cwd_cluster = bpb.root_cluster;
        return 0;
    }

    char full_path[FAT32_MAX_PATH];
    if (path[0] == '/') {
        strcpy(full_path, path);
    } else {
        strcpy(full_path, fat32_cwd_path);
        if (strcmp(full_path, "/") != 0) strcat(full_path, "/");
        strcat(full_path, path);
    }

    // Resolve relative parent navigation before checking filesystem entries.
    char normalized[FAT32_MAX_PATH];
    normalized[0] = '\0';
    char temp[FAT32_MAX_PATH];
    strcpy(temp, full_path);
    char* part = temp;
    if (*part == '/') part++;

    while (*part) {
        char* slash = strchr(part, '/');
        if (slash) *slash = '\0';

        if (part[0] == '\0' || strcmp(part, ".") == 0) {
            if (slash) part = slash + 1;
            else break;
            continue;
        }

        if (strcmp(part, "..") == 0) {
            if (strcmp(normalized, "/") == 0) {
                if (slash) part = slash + 1;
                else break;
                continue;
            }
            char* last = NULL;
            for (int i = 0; normalized[i]; i++) {
                if (normalized[i] == '/') last = &normalized[i];
            }
            if (!last || last == normalized) {
                strcpy(normalized, "/");
            } else {
                *last = '\0';
            }
            if (slash) part = slash + 1;
            else break;
            continue;
        }

        if (strcmp(normalized, "/") == 0) {
            strcpy(normalized, "/");
            strcat(normalized, part);
        } else {
            strcat(normalized, "/");
            strcat(normalized, part);
        }

        if (slash) part = slash + 1;
        else break;
    }

    if (normalized[0] == '\0') strcpy(normalized, "/");

    uint32_t cluster = fat32_resolve_path(normalized);
    if (cluster == 0) return -1;

    // Only directories may be entered.
    uint32_t parent_cluster = bpb.root_cluster;
    char parent_path[FAT32_MAX_PATH];
    char final_name[FAT32_MAX_FILENAME];
    strcpy(parent_path, normalized);
    char* last_slash = NULL;
    for (int i = 0; parent_path[i]; i++) {
        if (parent_path[i] == '/') last_slash = &parent_path[i];
    }
    if (last_slash && last_slash != parent_path) {
        *last_slash = '\0';
        parent_cluster = fat32_resolve_path(parent_path);
        strcpy(final_name, last_slash + 1);
    } else if (strcmp(normalized, "/") == 0) {
        parent_cluster = bpb.root_cluster;
        strcpy(final_name, "/");
    } else {
        parent_cluster = bpb.root_cluster;
        strcpy(final_name, normalized + 1);
    }

    if (strcmp(normalized, "/") == 0) {
        strcpy(fat32_cwd_path, "/");
        fat32_cwd_cluster = bpb.root_cluster;
        return 0;
    }

    uint32_t search_cluster = parent_cluster;
    uint8_t name_8[8], ext_3[3];
    standardize_name(final_name, name_8, ext_3);
    int found = 0;
    while (search_cluster != FAT32_EOC && !found) {
        uint32_t lba = cluster_to_lba(search_cluster);
        uint8_t sector_buf[512];
        for (int s = 0; s < bpb.sectors_per_cluster && !found; s++) {
            ata_read_sectors(lba + s, 1, sector_buf);
            fat32_dir_entry_t* entries = (fat32_dir_entry_t*)sector_buf;
            for (size_t i = 0; i < 512 / sizeof(fat32_dir_entry_t); i++) {
                if (entries[i].name[0] == 0x00) break;
                if (entries[i].name[0] == 0xE5) continue;
                if (entries[i].attributes == FAT_ATTR_LFN) continue;
                if (memcmp(entries[i].name, name_8, 8) == 0 && memcmp(entries[i].ext, ext_3, 3) == 0) {
                    if (!(entries[i].attributes & FAT_ATTR_DIRECTORY)) return -1;
                    found = 1;
                    break;
                }
            }
        }
        if (!found) search_cluster = get_next_cluster(search_cluster);
    }
    if (!found && strcmp(normalized, "/") != 0) return -1;

    strcpy(fat32_cwd_path, normalized);
    fat32_cwd_cluster = cluster;
    return 0;
}

void fat32_print_cwd(void) {
    print_string(fat32_cwd_path);
}

static void ls_callback(const char* name, uint8_t attr, uint32_t size, uint32_t cluster) {
    (void)cluster;
    if (attr & FAT_ATTR_DIRECTORY) print_string("<DIR> ");
    else print_string("      ");
    print_string(name);
    for (int i = strlen(name); i < 15; i++) print_char(' ');
    if (!(attr & FAT_ATTR_DIRECTORY)) {
        kprint_dec(size);
        print_string(" bytes");
    }
    print_char('\n');
}

void fat32_ls(uint32_t cluster) {
    fat32_list_dir(cluster, ls_callback);
}
