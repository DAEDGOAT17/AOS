# Bootloader & Multiboot - Kernel Loading

## Overview

The bootloader is the first code executed when the computer powers on. In AOS, we use the **Multiboot 2 standard**, which allows a compatible bootloader (like GRUB) to load our kernel without needing to write bootloader code from scratch.

This document explains how AOS interfaces with bootloaders and initializes in 32-bit protected mode before transitioning to 64-bit long mode.

---

## What is Multiboot?

Multiboot is a specification that defines how a bootloader and kernel communicate. Instead of requiring kernel developers to write bootloaders, they can rely on bootloaders like GRUB that already support the standard.

### Multiboot Versions

- **Multiboot 1** - Legacy, 32-bit only
- **Multiboot 2** - Modern, supports 32 and 64-bit

AOS supports **Multiboot 2** for both BIOS and UEFI boot methods.

---

## Multiboot Header

The Multiboot header tells the bootloader information about the kernel. It's placed in the first 32KB of the kernel file and contains specific magic numbers and configuration flags.

### Multiboot 2 Header Structure

```c
// Multiboot 2 header (architecture-independent)
struct mb2_header {
    uint32_t magic;           // MULTIBOOT2_MAGIC = 0xE85250D6
    uint32_t architecture;    // 0 = x86 (32-bit)
    uint32_t length;          // Header length in bytes
    uint32_t checksum;        // -(magic + architecture + length)

    // Followed by tags...
    struct {
        uint16_t type;        // Tag type
        uint16_t flags;       // Flags (optional, end tags, etc.)
        uint32_t size;        // Tag size in bytes
        // Tag-specific data...
    } tags[];

    // Terminating tag
    struct {
        uint16_t type;        // 0 = end tag
        uint16_t flags;       // 0
        uint32_t size;        // 8 bytes
    } end_tag;
};
```

### Header Magic Values

```c
#define MULTIBOOT2_MAGIC        0xE85250D6
#define MULTIBOOT_SEARCH        32768  // Search first 32KB of file
```

---

## AOS Boot Configuration

AOS boots as a 32-bit ELF executable with the following Multiboot 2 setup:

### Architecture Declaration

```
magic = 0xE85250D6 (Multiboot 2)
architecture = 0 (x86, 32-bit)
```

### Essential Tags

| Tag Type            | Purpose                                  |
| ------------------- | ---------------------------------------- |
| Information Request | Request framebuffer info from bootloader |
| Load Base Address   | Specify kernel load address              |
| Entry Point         | Where to jump after loading              |
| Framebuffer         | Request graphics mode                    |
| Module              | Load ramdisks or additional modules      |

---

## Kernel Entry Point

When the bootloader loads AOS, it:

1. **Loads kernel segments** into memory
2. **Sets up 32-bit protected mode**
3. **Jumps to entry point** with:
   - EAX = Multiboot magic number (0x36D76289)
   - EBX = Address of Multiboot info structure

The entry point is typically `_start` in the bootloader code (GRUB's `multiboot2.S` or kernel's loader):

```nasm
.global _start
_start:
    ; EAX = multiboot magic (0x36D76289)
    ; EBX = multiboot info structure
    cmp eax, MULTIBOOT2_MAGIC
    jne .error_magic

    ; Store multiboot magic and info for kernel usage
    mov [multiboot_magic], eax
    mov [multiboot_info], ebx

    ; Continue to kernel initialization...
```

---

## Multiboot Information Structure

The bootloader passes an information structure containing:

```c
// Multiboot 2 info structure (simplified)
struct multiboot_info {
    uint32_t size;              // Size of this structure
    uint32_t reserved;          // Reserved (must be 0)

    // Followed by tags...
    struct multiboot_tag {
        uint32_t type;
        uint32_t size;
        uint8_t  data[];        // Tag-specific data
    } tags[];
};
```

### Important Tag Types

| Type | Name              | Contents                      |
| ---- | ----------------- | ----------------------------- |
| 0    | End               | Marks end of tags             |
| 1    | Boot Command Line | Kernel boot parameters        |
| 2    | Boot Loader Name  | Name of bootloader            |
| 3    | Module            | Loaded modules (ramdisks)     |
| 4    | Basic Memory Info | Memory layout                 |
| 6    | Memory Map        | Available and reserved memory |
| 8    | Framebuffer Info  | Graphics mode details         |
| 9    | ELF Symbols       | Kernel symbol table           |

---

## Memory Information from Bootloader

The memory map tag provides crucial information about available RAM:

```c
struct multiboot_mmap_entry {
    uint64_t addr;           // Start address
    uint64_t len;            // Length in bytes
    uint32_t type;           // 1=RAM, 2=Reserved, 3=ACPIReclaimable, etc.
    uint32_t zero;           // Reserved (padding)
} __attribute__((packed));
```

AOS uses this to initialize the Physical Memory Manager (PMM):

```c
void pmm_init(uint32_t magic, multiboot_info_t* mbd) {
    // Iterate through memory map tags
    struct multiboot_tag* tag = (struct multiboot_tag*)(mbd + 8);

    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        if (tag->type == MULTIBOOT_TAG_TYPE_MMAP) {
            // Process memory map entries
            struct multiboot_mmap_entry* entry = ...;
            // Mark available RAM in bitmap
        }

        // Move to next tag
        tag = (struct multiboot_tag*)((uint8_t*)tag +
              ((tag->size + 7) & ~7));
    }
}
```

---

## Framebuffer Information

Modern systems use graphical framebuffers instead of VGA text mode. The bootloader provides framebuffer details:

```c
struct multiboot_tag_framebuffer {
    uint64_t common_addr;      // Framebuffer physical address
    uint32_t common_pitch;     // Bytes per scanline
    uint32_t common_width;     // Width in pixels
    uint32_t common_height;    // Height in pixels
    uint8_t  common_bpp;       // Bits per pixel
    uint8_t  common_type;      // Type (0=indexed, 1=RGB)
    uint8_t  reserved;

    // For RGB:
    uint8_t  red_field_pos;
    uint8_t  red_mask_size;
    uint8_t  green_field_pos;
    uint8_t  green_mask_size;
    uint8_t  blue_field_pos;
    uint8_t  blue_mask_size;
};
```

AOS maps this framebuffer into virtual memory and uses it for graphics output.

---

## Boot Process Timeline

```
1. Power On
   ↓
2. BIOS/UEFI Firmware
   ↓
3. Bootloader (GRUB) starts
   ↓
4. Bootloader searches for Multiboot 2 header
   ↓
5. Bootloader loads kernel ELF segments
   ↓
6. Bootloader copies Multiboot info to memory
   ↓
7. Bootloader switches to 32-bit protected mode
   ↓
8. Bootloader jumps to kernel entry point
   ↓
9. Kernel _start executes (in protected mode)
   ↓
10. Kernel transitions to 64-bit long mode
   ↓
11. Kernel initializes subsystems
   ↓
12. Kernel initializes drivers and shell
   ↓
13. System ready ✓
```

---

## ISO Building and Boot

AOS uses GRUB's Multiboot 2 support for ISO generation:

### GRUB Configuration

```
# iso/boot/grub/grub.cfg
menuentry 'AOS' {
    multiboot2 /boot/kernel.elf
    boot
}

menuentry 'AOS (UEFI)' {
    multiboot2 /boot/kernel.elf
    boot
}
```

### ISO Creation

```bash
# Create ISO with GRUB bootloader
grub-mkrescue -o aos.iso iso/

# Result:
# - grub.cfg in the ISO
# - kernel.elf loaded by GRUB
# - Boot sector points to GRUB
# - BIOS and UEFI boot support
```

---

## Kernel Transition: 32-bit to 64-bit

After bootloader loads kernel in 32-bit protected mode:

1. **Bootloader passes control** to `kernel_entry()` (32-bit code)
2. **Kernel checks magic** to ensure Multiboot compliant bootloader
3. **Kernel sets up paging** for 64-bit addressing
4. **Kernel enables long mode** via MSR registers
5. **Kernel enters 64-bit mode** with `ljmp` instruction
6. **64-bit code continues** initialization

This transition is handled in:

- `src/arch/x86_64/asm/loader.s` - Entry point assembly

---

## Common Boot Issues

| Problem                    | Cause                               | Solution                 |
| -------------------------- | ----------------------------------- | ------------------------ |
| "No multiboot magic"       | Bootloader not multiboot-compatible | Use GRUB 2+              |
| "Invalid memory info"      | Corrupted Multiboot structure       | Check bootloader         |
| Memory size wrong          | Wrong tag parsed                    | Verify tag iteration     |
| Framebuffer not found      | Graphics mode unsupported           | Fall back to text mode   |
| Kernel loads but no output | VGA setup issue                     | Check screen driver init |

---

## Multiboot Tag Walking Code

Typical code to iterate through Multiboot 2 tags:

```c
void process_multiboot_info(multiboot_info_t* info) {
    // Start at first tag (after 8-byte header)
    struct multiboot_tag* tag = (struct multiboot_tag*)(info + 8);

    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        switch (tag->type) {
        case MULTIBOOT_TAG_TYPE_MMAP:
            handle_memory_map(tag);
            break;

        case MULTIBOOT_TAG_TYPE_FRAMEBUFFER:
            handle_framebuffer(tag);
            break;

        case MULTIBOOT_TAG_TYPE_ELF_SECTIONS:
            handle_elf_symbols(tag);
            break;
        }

        // Align tag size to 8-byte boundary and advance
        tag = (struct multiboot_tag*)((uint8_t*)tag +
              ((tag->size + 7) & ~7));
    }
}
```

---

## Key Takeaways

✓ Multiboot 2 standard simplifies bootloader integration  
✓ GRUB provides bootloader functionality for free  
✓ Bootloader loads kernel in 32-bit protected mode  
✓ Multiboot info contains memory, framebuffer, and module data  
✓ Kernel must validate magic before using Multiboot info  
✓ Memory map from bootloader initializes PMM  
✓ Framebuffer allows graphics output without BIOS calls

---

## Related Components

- [Long Mode Transition](long_mode.html)
- [Physical Memory Manager](../memory/pmm.html)
- [Screen/VGA Driver](../drivers/screen_driver.html)
- [Kernel Initialization](../core_systems/kernel_init.html)

---

**Source Files:**

- `src/arch/x86_64/asm/loader.s` - Bootloader interface
- `src/core/kernel.c` - Multiboot processing
- `include/` - Multiboot declarations
- `iso/boot/grub/grub.cfg` - Boot configuration
