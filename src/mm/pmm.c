#include "pmm.h"
#include "screen.h"
#include "acpi.h"
#include <stdbool.h>

// Global bitmap - resides in .bss section
uint8_t pmm_bitmap[PMM_BITMAP_SIZE];
static uint32_t total_memory_kb = 0;
static uint32_t free_blocks = 0;

uint64_t initrd_brain_start = 0;
uint64_t initrd_brain_end   = 0;
uint8_t  initrd_brain_loaded = 0;

/* Ramdisk: FAT32 disk image embedded in the ISO as a Multiboot2 module */
uint64_t ramdisk_start  = 0;
uint64_t ramdisk_size   = 0;
uint8_t  ramdisk_loaded = 0;

uint64_t whisper_model_start = 0;
uint64_t whisper_model_end = 0;
uint8_t whisper_model_loaded = 0;

// Symbols from link.ld
extern uint32_t _kernel_start;
extern uint32_t _kernel_end;

static void pmm_set_bit(uint32_t bit) {
    if (!(pmm_bitmap[bit / 8] & (1 << (bit % 8)))) {
        pmm_bitmap[bit / 8] |= (1 << (bit % 8));
        if (free_blocks > 0) free_blocks--;
    }
}

static void pmm_clear_bit(uint32_t bit) {
    if (pmm_bitmap[bit / 8] & (1 << (bit % 8))) {
        pmm_bitmap[bit / 8] &= ~(1 << (bit % 8));
        free_blocks++;
    }
}

static void pmm_mark_available_range(uint64_t address, uint64_t length) {
    if (length == 0 || address > UINT64_MAX - length ||
        address > UINT64_MAX - (PMM_BLOCK_SIZE - 1)) return;

    uint64_t page = (address + PMM_BLOCK_SIZE - 1) &
        ~((uint64_t)PMM_BLOCK_SIZE - 1);
    uint64_t end = (address + length) & ~((uint64_t)PMM_BLOCK_SIZE - 1);
    while (page < end) {
        uint64_t block = page / PMM_BLOCK_SIZE;
        if (block >= PMM_BITMAP_SIZE * 8ULL) break;
        pmm_clear_bit((uint32_t)block);
        page += PMM_BLOCK_SIZE;
    }
}

static int pmm_contains_text(const char* text, const char* token) {
    if (!text || !token || !token[0]) return 0;
    for (const char* start = text; *start; start++) {
        const char* left = start;
        const char* right = token;
        while (*left && *right && *left == *right) {
            left++;
            right++;
        }
        if (!*right) return 1;
    }
    return 0;
}

void pmm_init(uint32_t magic, void* mbd_ptr) {
    // 1. Mark everything as RESERVED
    for (int i = 0; i < PMM_BITMAP_SIZE; i++) pmm_bitmap[i] = 0xFF;
    free_blocks = 0;

    // 2. Parse Multiboot Map depending on boot method
    if (magic == 0x2BADB002) {
        // --- Multiboot 1 ---
        multiboot_info_t* mbd = (multiboot_info_t*)mbd_ptr;
        if (mbd->flags & (1 << 6)) {
            multiboot_mmap_entry_t* mmap = (multiboot_mmap_entry_t*)(uint64_t)mbd->mmap_addr;
            uint32_t mmap_end = mbd->mmap_addr + mbd->mmap_length;

            while((uint64_t)mmap < mmap_end) {
                if (mmap->type == 1) { // Available RAM
                    pmm_mark_available_range(mmap->addr, mmap->len);
                }
                mmap = (multiboot_mmap_entry_t*)((uint64_t)mmap + mmap->size + sizeof(mmap->size));
            }
        }
        
        // Framebuffer info (bit 12)
        if (mbd->flags & (1 << 12)) {
            screen_init_fb(mbd->framebuffer_addr, mbd->framebuffer_width, 
                           mbd->framebuffer_height, mbd->framebuffer_pitch, 
                           mbd->framebuffer_bpp);
        }

        // Modules (bit 3) — detect embedded ramdisk named "disk"
        if (mbd->flags & (1 << 3)) {
            multiboot1_module_t* mods = (multiboot1_module_t*)(uint64_t)mbd->mods_addr;
            for (uint32_t i = 0; i < mbd->mods_count; i++) {
                char* str = (char*)(uint64_t)mods[i].string;
                if (str) {
                    // Search for "disk" anywhere in the command string
                    bool found = false;
                    int len = 0;
                    while (str[len]) len++;
                    
                    for (int j = 0; j <= len - 4; j++) {
                        if (str[j]=='d' && str[j+1]=='i' && str[j+2]=='s' && str[j+3]=='k') {
                            found = true; break;
                        }
                    }
                    if (found) {
                        ramdisk_start  = mods[i].mod_start;
                        ramdisk_size   = mods[i].mod_end - mods[i].mod_start;
                        ramdisk_loaded = 1;
                    } else if (pmm_contains_text(str, "whisper_model")) {
                        whisper_model_start = mods[i].mod_start;
                        whisper_model_end = mods[i].mod_end;
                        whisper_model_loaded = whisper_model_end > whisper_model_start;
                    }
                }
            }
        }
    } else if (magic == 0x36D76289) {
        // --- Multiboot 2 ---
        // size of the entire mb2 structure is at the first 4 bytes
        uint32_t total_size = *(uint32_t*)mbd_ptr;
        uint32_t current_offset = 8; // skip size and reserved fields
        
        while (current_offset < total_size) {
            multiboot2_tag_t* tag = (multiboot2_tag_t*)((uint8_t*)mbd_ptr + current_offset);
            
            // Suppressed print statements to prevent accessing unmapped Framebuffer addresses > 4GB
            
            if (tag->type == 0) { // End of tags
                break;
            }
            if (tag->type == 6) { // Memory map tag
                multiboot2_mmap_tag_t* mmap_tag = (multiboot2_mmap_tag_t*)tag;
                uint32_t entry_size = mmap_tag->entry_size;
                if (entry_size >= sizeof(multiboot2_mmap_entry_t) &&
                    mmap_tag->size >= sizeof(multiboot2_mmap_tag_t)) {
                    uint32_t payload_size = mmap_tag->size - sizeof(multiboot2_mmap_tag_t);
                    uint32_t num_entries = payload_size / entry_size;
                    uint8_t* entry_ptr = (uint8_t*)mmap_tag + sizeof(multiboot2_mmap_tag_t);
                    for (uint32_t i = 0; i < num_entries; i++) {
                        multiboot2_mmap_entry_t* entry = (multiboot2_mmap_entry_t*)entry_ptr;
                        if (entry->type == 1) {
                            pmm_mark_available_range(entry->addr, entry->len);
                        }
                        entry_ptr += entry_size;
                    }
                }
            } else if (tag->type == 8) { // Framebuffer tag
                multiboot2_fb_tag_t* fb_tag = (multiboot2_fb_tag_t*)tag;
                screen_init_fb(fb_tag->addr, fb_tag->width, fb_tag->height, fb_tag->pitch, fb_tag->bpp);
            } else if (tag->type == 3) { // Module tag
                multiboot2_module_tag_t* mod_tag = (multiboot2_module_tag_t*)tag;
                
                /* Detect ramdisk: search for "disk" in module string */
                bool found = false;
                int len = 0;
                while (mod_tag->string[len]) len++;
                
                for (int j = 0; j <= len - 4; j++) {
                    if (mod_tag->string[j] == 'd' && mod_tag->string[j+1] == 'i' && 
                        mod_tag->string[j+2] == 's' && mod_tag->string[j+3] == 'k') {
                        found = true; break;
                    }
                }
                if (found) {
                    ramdisk_start  = mod_tag->mod_start;
                    ramdisk_size   = mod_tag->mod_end - mod_tag->mod_start;
                    ramdisk_loaded = 1;
                } else if (pmm_contains_text(mod_tag->string, "whisper_model")) {
                    whisper_model_start = mod_tag->mod_start;
                    whisper_model_end = mod_tag->mod_end;
                    whisper_model_loaded = whisper_model_end > whisper_model_start;
                } else if (mod_tag->string[0] == 'a' && mod_tag->string[1] == 'i') {
                    initrd_brain_start = mod_tag->mod_start;
                    initrd_brain_end   = mod_tag->mod_end;
                    initrd_brain_loaded = 1;
                }
            } else if (tag->type == 14 || tag->type == 15) { // ACPI RSDP Tag (old or new)
                void* rsdp_addr = (void*)((uint8_t*)tag + 8);
                acpi_init(rsdp_addr);
            }
            // Tags are 8-byte aligned
            current_offset += (tag->size + 7) & ~7;
        }
    }

    // Capture total detected RAM amount before marking kernel/reserved regions
    total_memory_kb = free_blocks * 4;

    // 3. Protect Kernel Memory
    uint32_t start_block = (uint64_t)&_kernel_start / PMM_BLOCK_SIZE;
    uint32_t end_block = (uint64_t)&_kernel_end / PMM_BLOCK_SIZE;
    for (uint32_t i = start_block; i <= end_block; i++) {
        pmm_set_bit(i);
    }

    // 4. Protect Page 0 (NULL pointer protection)
    pmm_set_bit(0);

    // 5. Protect InitRD Module Memory
    if (initrd_brain_loaded) {
        uint32_t mod_start_block = initrd_brain_start / PMM_BLOCK_SIZE;
        uint32_t mod_end_block = (initrd_brain_end + PMM_BLOCK_SIZE - 1) / PMM_BLOCK_SIZE;
        for (uint32_t i = mod_start_block; i <= mod_end_block; i++) {
            pmm_set_bit(i);
        }
    }

    // 6. Protect Ramdisk Memory (don't let allocator overwrite the FAT32 image)
    if (ramdisk_loaded) {
        uint32_t rd_start_block = (uint32_t)(ramdisk_start / PMM_BLOCK_SIZE);
        uint32_t rd_end_block   = (uint32_t)((ramdisk_start + ramdisk_size + PMM_BLOCK_SIZE - 1) / PMM_BLOCK_SIZE);
        for (uint32_t i = rd_start_block; i <= rd_end_block; i++) {
            pmm_set_bit(i);
        }
    }

    if (whisper_model_loaded) {
        uint32_t model_start_block = (uint32_t)(whisper_model_start / PMM_BLOCK_SIZE);
        uint32_t model_end_block = (uint32_t)((whisper_model_end + PMM_BLOCK_SIZE - 1) / PMM_BLOCK_SIZE);
        for (uint32_t i = model_start_block; i < model_end_block; i++) {
            pmm_set_bit(i);
        }
    }
}

static uint32_t last_free_bit = 256;

void* pmm_alloc_block() {
    // First pass: start searching from the last known free bit
    for (uint32_t i = (last_free_bit / 8); i < PMM_BITMAP_SIZE; i++) {
        if (pmm_bitmap[i] != 0xFF) {
            for (int j = 0; j < 8; j++) {
                uint32_t block = (i * 8) + j;
                if (block >= last_free_bit && !(pmm_bitmap[i] & (1 << j))) {
                    pmm_set_bit(block);
                    last_free_bit = block + 1;
                    return (void*)((uint64_t)block * PMM_BLOCK_SIZE);
                }
            }
        }
    }
    
    // Second pass: if we reached the end, loop back and search from 1MB line
    for (uint32_t i = (256 / 8); i <= (last_free_bit / 8); i++) {
        if (pmm_bitmap[i] != 0xFF) {
            for (int j = 0; j < 8; j++) {
                uint32_t block = (i * 8) + j;
                if (block >= 256 && !(pmm_bitmap[i] & (1 << j))) {
                    pmm_set_bit(block);
                    last_free_bit = block + 1;
                                        return (void*)((uint64_t)block * PMM_BLOCK_SIZE);

                }
            }
        }
    }
    return 0;
}

void pmm_free_block(void* addr) {
    uint32_t block = (uint64_t)addr / PMM_BLOCK_SIZE;
    pmm_clear_bit(block);
    if (block < last_free_bit) {
        last_free_bit = block; // Point roving cache to early newly freed block
    }
}

uint32_t pmm_get_total_memory_kb() {
    return total_memory_kb;
}

uint32_t pmm_get_used_blocks() {
    uint32_t total_blocks = total_memory_kb / 4;
    if (total_blocks < free_blocks) return 0;
    return total_blocks - free_blocks;
}

uint32_t pmm_get_free_blocks() {
    return free_blocks;
}