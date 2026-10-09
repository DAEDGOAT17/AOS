# AOS Documentation

Welcome to the AOS learning and contributor documentation. Start with the [newcomer guide](START_HERE.html), browse the [HTML portal](index.html), or search all 195 public declarations and 379 AOS-owned C definitions in the [function glossary](FUNCTION_GLOSSARY.html).

## Learning path

1. [Start Here](START_HERE.html): what AOS is, build/run, repo map, and first shell exercises.
2. [Boot and architecture](architecture/bootloader.html): firmware-to-kernel entry and x86_64 setup.
3. [Memory](memory/pmm.html) and [virtual memory](memory/vmm.html): how physical frames and page tables are managed.
4. [Interrupts](interrupt_io/idt.html), [tasks](core_systems/task_management.html), and [scheduler](core_systems/scheduler.html): how events and execution are coordinated.
5. [Filesystem](filesystem/fat32.html), [storage drivers](drivers/ata_driver.html), and [networking](networking/lwip_stack.html): how AOS reaches hardware.
6. [Shell](core_systems/shell.html), [AI-assisted OS research brief](../genera_pitch/PITCH_BRIEF.html), and [complete function glossary](FUNCTION_GLOSSARY.html): explore the live agent/Lisp path and source API.

## Architecture in one paragraph

GRUB loads the Multiboot kernel. `src/core/kernel.c` initializes CPU tables, memory, timer, heap, tasks, PCI devices, storage/FAT32, and finally the shell task. The shell dispatches commands and the bounded Lisp interpreter. For `ask`, the local model supplies one tool proposal through an Ollama-compatible endpoint; AOS validates and executes supported tools, captures actual output, persists task state, and feeds observations back. FAT32-backed Lisp programs can be saved and reused. Model inference is host-provided, not embedded in the kernel.

## Documentation sections

- [Architecture and boot](architecture/)
- [Memory management](memory/)
- [Interrupts and I/O](interrupt_io/)
- [Device drivers](drivers/)
- [Filesystem](filesystem/)
- [Core systems](core_systems/)
- [Networking](networking/)
- [Research pitch and demo](../genera_pitch/PITCH_BRIEF.html)
- [Generated function glossary](FUNCTION_GLOSSARY.html)

## Generated references

Regenerate the glossary and search data after changing AOS public headers or functions:

```sh
python3 tools/generate_docs_index.py
```

The generator indexes AOS-owned `include/*.h` declarations and `src/**/*.c` definitions; vendored `src/net/lwip/` implementation is excluded. See [START_HERE](START_HERE.html) for contribution guidance and tested commands.
