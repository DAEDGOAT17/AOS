#ifndef PMM_H
#define PMM_H

#include <stdint.h>

#define PMM_BLOCK_SIZE 4096
#define PMM_BITMAP_SIZE 524288 // Tracks up to 16GB of RAM (in bytes: 512KB bitmap)

typedef struct {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} __attribute__((packed)) multiboot_mmap_entry_t;

typedef struct {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t framebuffer_bpp;
    uint8_t framebuffer_type;
    uint8_t color_info[6];
} __attribute__((packed)) multiboot_info_t;

typedef struct {
    uint32_t type;
    uint32_t size;
} __attribute__((packed)) multiboot2_tag_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
} __attribute__((packed)) multiboot2_mmap_tag_t;

typedef struct {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t zero;
} __attribute__((packed)) multiboot2_mmap_entry_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t bpp;
    uint8_t framebuffer_type;
    uint16_t reserved;
    uint8_t color_info[6];
} __attribute__((packed)) multiboot2_fb_tag_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
    char string[1]; // String starts here
} __attribute__((packed)) multiboot2_module_tag_t;

/* Multiboot1 module entry (used when booting via Legacy BIOS/Multiboot1) */
typedef struct {
    uint32_t mod_start;
    uint32_t mod_end;
    uint32_t string;  /* physical addr of null-terminated command string */
    uint32_t reserved;
} __attribute__((packed)) multiboot1_module_t;

extern uint64_t initrd_brain_start;
extern uint64_t initrd_brain_end;
extern uint8_t  initrd_brain_loaded;

/* Ramdisk (disk.img embedded in ISO as Multiboot2 module named "disk") */
extern uint64_t ramdisk_start;
extern uint64_t ramdisk_size;
extern uint8_t  ramdisk_loaded;

extern uint64_t whisper_model_start;
extern uint64_t whisper_model_end;
extern uint8_t  whisper_model_loaded;

void pmm_init(uint32_t magic, void* mbd);
void* pmm_alloc_block();
void pmm_free_block(void* addr);
uint32_t pmm_get_total_memory_kb();
uint32_t pmm_get_used_blocks();
uint32_t pmm_get_free_blocks();

#endif