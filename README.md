# AOS - Modern 64-bit Educational Kernel
> A powerful x86_64 monolithic kernel built from scratch for learning OS internals and AI integration.

## 📸 QEMU - simulation

![Jarvis OS Demo](https://raw.githubusercontent.com/DAEDGOAT17/AOS/pankaj/images/jarvis-os-image.jpeg)

**Status**: Active Development | **License**: Open Source | **Target**: Modern Laptops (x86_64) | **Language**: C (99%)

---

## 🌐 Live Web Documentation
The full architecture documentation, development guides, and subsystem deep-dives are rendered live at:
👉 **[https://daedgoat17.github.io/AOS/](https://daedgoat17.github.io/AOS/)**

Research overview and reproducible demo: [AOS AI-Assisted Operating Environment](docs/genera_pitch/PITCH_BRIEF.md).

---

## 🎯 Quick Start (Build & Run)

```bash
./run.sh --run        # Runs in BIOS (Legacy) mode
./run.sh --run --uefi # Runs in UEFI (Modern) mode
```

### Offline Voice Typing

Download the ignored, local Whisper tiny-English model before building:

```bash
mkdir -p models
curl -L --fail https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en.bin -o models/ggml-tiny.en.bin
./run.sh --run
```

At the AOS prompt, run `voice type`, speak, then pause. The locally transcribed text is inserted into the shell prompt for review; it is not executed until you press Enter. The model is loaded as a boot module and inference runs in the kernel without Python or network access.

## 📋 Overview
Jarvis OS is a comprehensive educational operating system kernel designed to teach modern x86_64 architecture principles, memory management, process scheduling, and AI integration. Built entirely in C with minimal assembly, it provides hands-on experience with real OS concepts including boot sequences, interrupt handling, virtual memory, and filesystem implementation.

### Key Highlights
- **From Scratch**: No external kernel frameworks—everything built for educational clarity
- **Modern Boot**: Supports both legacy BIOS and UEFI with Multiboot 2 specification
- **AI-Assisted Runtime**: AOS owns task policy, bounded tool execution, interpreted Lisp apps, and persistent agent tools; a local Ollama-compatible service supplies model inference
- **Practical Testing**: Runs on modern x86_64 systems, VirtualBox, and QEMU

---

## 🏗️ Architecture

### Core Design
- **Mode**: 64-bit Long Mode (x86_64) with proper privilege levels
- **Boot**: Hybrid ISO supporting both BIOS and UEFI (Multiboot 2 compliant)
- **Memory Layout**: 
  - Kernel mapped to higher half (8GB boundary) for security isolation
  - 4GB identity mapping fallback for compatibility
  - Proper page table structure with 4-level paging

### Interrupt & Execution Model
- **Advanced APIC / IO-APIC logic** for modern CPU interrupt routing
- **PIC disabled** in favor of modern APIC architecture
- **Global Descriptor Table (GDT)**: 64-bit segments for kernel and user space
- **Interrupt Descriptor Table (IDT)**: Full exception and hardware interrupt handling

### Memory Management
- **Physical Memory Manager**: Bitmap-based allocation for efficient memory tracking
- **Virtual Memory Manager**: Full 4-level paging implementation with protection
- **Kernel Heap Allocator**: Dynamic memory allocation for kernel structures
- **Page Protection**: Proper access control and fault handling

### Process & Scheduling
- **Scheduler**: Round-robin scheduling for fair process execution
- **Process Context**: Full process state management and switching
- **Interrupt Safety**: Proper context preservation across interrupts

### Storage & Filesystem
- **ATA PIO Driver**: ATA Programmed I/O for disk communication
- **FAT32 Filesystem**: Full FAT32 support for disk operations
- **Boot Integration**: Filesystem accessible from bootloader and kernel

### Device Support
- **Framebuffer**: Hybrid BIOS/UEFI framebuffer support for graphics
- **Keyboard Driver**: Full keyboard input handling and event processing
- **Video Driver**: Virtio-VGA graphics support for modern virtualization
- **Display**: Terminal-based output with graphics capability

### AI Integration
- **Model Boundary**: The language model runs through a local Ollama-compatible endpoint; model inference is not embedded in the kernel
- **OS-Owned Agent Loop**: AOS validates proposed actions, executes native shell/Lisp tools, captures observations, and sends results back for the next bounded decision
- **Persistent Lisp Tools**: Validated named programs and short memory records are stored through FAT32 APIs and can be read or reused by the OS
- **Constrained JIT Experiment**: A narrow, allowlisted native-stub path explores runtime extension; it is not general C compilation or unrestricted kernel mutation

---

## ✅ Roadmap & Implementation Status

### Completed Features
- [x] 64-bit Long Mode Transition with proper setup
- [x] UEFI Boot Support (Multiboot 2 specification)
- [x] Global Descriptor Table (64-bit GDT)
- [x] IDT & APIC / IO-APIC Routing for interrupt handling
- [x] Physical Memory Manager (Bitmap-based)
- [x] Virtual Memory Manager (Full 4-level paging)
- [x] Hybrid BIOS/UEFI Framebuffer Support
- [x] Keyboard Driver & VGA (Virtio-VGA) Support
- [x] Advanced Shell with NLP Intent Mapping
- [x] Kernel Heap Allocator with fragmentation handling
- [x] Round-Robin Scheduler with context switching
- [x] ATA PIO Driver & FAT32 Filesystem Support
- [x] AI Brain (LLM Engine) Integration

### In Progress / Planned
- [ ] User Mode Support (Ring 3 privilege level)
- [ ] System Call Interface (syscall/sysret)
- [ ] SMP (Symmetric Multi-Processing) for multi-core support
- [ ] Virtual Filesystem Abstraction Layer
- [ ] Advanced Networking Stack
- [ ] Device Driver Framework
- [ ] Performance Optimization & Benchmarking

---

## 🛠️ Development Setup

### Prerequisites
- x86_64 Linux system or WSL2
- GCC toolchain (x86_64-elf-gcc recommended)
- QEMU or VirtualBox for testing
- GNU Make for building

### Building
```bash
make clean
make
./run.sh --run        # Build and run in BIOS mode
./run.sh --run --uefi # Build and run in UEFI mode
```

---

## 📚 Learning Focus

This project covers:
- **Boot Sequences**: From firmware to kernel initialization
- **Memory Management**: Physical allocation, virtual addressing, paging
- **Interrupt Handling**: Exception vectors, APIC/IO-APIC configuration
- **Scheduling**: Process management and context switching
- **Filesystem**: Block I/O, FAT32 implementation, directory structures
- **Device Drivers**: Keyboard, framebuffer, and storage integration
- **AI Integration**: Embedding ML concepts in system software

---

## 🎓 Educational Value

Jarvis OS is ideal for:
- Computer Science students learning OS internals
- Systems programmers understanding hardware-software interaction
- Developers interested in kernel-level programming
- Researchers exploring AI integration in system software
- Anyone building their own OS from scratch

---

## 📊 Project Statistics

- **Primary Language**: C (99%)
- **Build System**: Makefile-based
- **Target Architecture**: x86_64
- **Boot Standards**: BIOS (Legacy) & UEFI (Modern)
- **Repository Size**: Compact, modular codebase

---

## 🚀 Getting Started

1. **Clone the repository**
   ```bash
   git clone https://github.com/DAEDGOAT17/AOS.git
   cd AOS
   ```

2. **Build the kernel**
   ```bash
   make
   ```

3. **Run in emulation**
   ```bash
   ./run.sh --run        # BIOS mode
   ./run.sh --run --uefi # UEFI mode
   ```

4. **Test in VirtualBox or QEMU**
   - Use the generated ISO file
   - Allocate 512MB+ RAM and 2+ CPU cores
   - Enable serial output for debugging

---

## 📖 Documentation

- Full architecture documentation in source code comments
- Boot sequence details in bootloader and kernel startup
- Memory management strategy in paging subsystem
- Scheduler implementation with round-robin algorithm
- AI integration through LLM engine wrapper

---

## 💡 Future Enhancements

- **User-space Programs**: Support for running applications in Ring 3
- **Networking**: TCP/IP stack implementation
- **Advanced Scheduling**: Priority queues and preemption
- **Security**: Page access protection and privilege isolation
- **Performance**: Optimization for multi-core systems

---

## 📝 License

This project is open source. See LICENSE file for details.

---

## 🔗 Related Resources

- x86_64 Architecture References
- Multiboot 2 Specification
- FAT32 Filesystem Documentation
- QEMU/VirtualBox Emulation Guides

---

**Last Updated**: 2026-05-09
