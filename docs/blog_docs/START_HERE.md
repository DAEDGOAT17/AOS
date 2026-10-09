# Start Here: Learn and Contribute to AOS

AOS is a from-scratch x86_64 operating-system prototype. It boots through GRUB in BIOS or UEFI mode, initializes memory and hardware in C, mounts FAT32 storage, and starts an interactive shell. Its AI-assisted agent receives model proposals from a local Ollama-compatible service; the kernel owns command validation, execution, observation capture, and persistent Lisp tools.

This guide is for a reader who has never opened the source tree. Follow it in order, then use the [documentation index](index.html) or [function glossary](FUNCTION_GLOSSARY.html) to go deeper.

## 1. What AOS Is (and Is Not)

- AOS is an educational, monolithic kernel targeting x86_64.
- The model currently runs on the connected host through a local Ollama-compatible endpoint; the LLM is not compiled into the kernel.
- AOS owns a bounded agent loop and executes supported Lisp expressions in its kernel runtime.
- Saved Lisp tools and agent memory use FAT32. On physical AHCI storage, writes are flushed; on a RAM-backed filesystem, changes disappear at reboot.
- The Lisp interpreter has grammar and step limits, but it is not a process-isolated security sandbox: interpreted code still runs in kernel context.
- The experimental `kernel-stub` operator reaches a very narrow JIT contract. It is not a general C compiler or unrestricted kernel hot-patching system.

These boundaries are part of the design. When describing behavior, distinguish what the running OS has demonstrated from what remains planned.

## 2. Repository Map

| Path | What lives here |
| --- | --- |
| `src/core/kernel.c` | Kernel entry point and boot initialization sequence |
| `src/core/shell.c` | Interactive command parser, saved Lisp apps, and shell task |
| `src/core/lisp.c` | Lisp parser, validator, evaluator, persistent memory/tool operators |
| `src/agent/` | Persistent agent state, task lifecycle, and local intent routing |
| `src/drivers/` | Screen, keyboard, storage, audio, PCI, and network drivers |
| `src/fs/` | FAT32 and GPT implementation |
| `src/mm/` | Physical, virtual, and heap memory management |
| `src/net/lwip/` | Vendored lwIP networking implementation |
| `include/` | Public AOS headers and subsystem interfaces |
| `tools/` | Host-side setup, model transport, and test utilities |
| `tests/` | Python tests for host tools and agent integration contracts |
| `docs/blog_docs/` | Architecture guides, this learning path, and generated API references |

## 3. Build and Boot

From the repository root on Linux, build a bootable hybrid BIOS/UEFI image:

```sh
./run.sh
```

The build writes `kernel.elf`, `jarvis.iso`, `jarvis_uefi.iso`, `disk.img`, and generated files under `build/` and `iso/`. **It removes and recreates `build/` and `iso/` and overwrites the generated images.** Keep any data you need from those paths elsewhere before rebuilding.

To launch the image in QEMU:

```sh
./run.sh --run --no-audio
```

UEFI emulation, when OVMF is installed:

```sh
./run.sh --run --uefi --no-audio
```

The serial/VGA shell displays `AOS [/] $`. Type `help` to see commands available in the image you booted. Kernel C changes require rebuilding and rebooting the image; saved Lisp apps can be edited and run at runtime.

## 4. Learn the Boot Path

Read these files in sequence:

1. `src/core/kernel.c`: firmware handoff, GDT/IDT, timer, VMM, heap, tasks, PCI, storage, filesystem, and shell startup.
2. `src/arch/x86_64/`: architecture setup and low-level assembly.
3. `src/mm/pmm.c` and `src/mm/vmm.c`: physical and virtual memory foundations.
4. `src/core/task.c` and `src/core/timer.c`: task scheduling and timer ticks.
5. `src/fs/fat32.c`: mounted filesystem and file operations.
6. `src/core/shell.c`: command input and dispatch.
7. `src/agent/agent.c`, `src/drivers/net/net_stack.c`, and `src/core/lisp.c`: agent state, model/tool protocol, and bounded in-kernel programs.

Use [the generated function glossary](FUNCTION_GLOSSARY.html) to search all 195 public header declarations and 379 AOS-owned C function definitions. It excludes vendored `src/net/lwip/` implementation internals; those belong to the upstream lwIP project.

## 5. Try the Shell and Saved Apps

List persisted Lisp apps:

```text
apps
```

Run an app by its name, optionally passing a text argument:

```text
runapp fact5
```

Inspect a saved app's source through FAT32:

```text
cat /agent/lisp/FACT5.LSP
```

The FAT32 implementation uses 8.3 filenames, so stored Lisp apps use `.lsp`. App command names are 1-8 lowercase letters/digits and cannot conflict with shell commands.

Evaluate a Lisp expression directly:

```text
lisp (print (concat "AOS " "ready"))
```

The exact supported expressions are listed in `src/core/lisp.c`; not every shell command is available as a Lisp operator. A small arithmetic example is:

```text
lisp (begin (var n 5) (var result 1) (var i 1) (while (<= i n) (begin (set! result (* result i)) (set! i (+ i 1)))) (print result))
```

## 6. Save Persistent Memory and Lisp Tools

Store and retrieve a short named note:

```text
lisp (print (memory-set "project" "AOS agent demo"))
lisp (print (memory-get "project"))
```

Save a reusable Lisp program, list the shelf, and execute it without asking the model to regenerate its source:

```text
lisp (print (tool-save "fact5" "(begin (var n 5)(var r 1)(var i 1)(while (<= i n)(begin(set! r (* r i))(set! i (+ i 1))))(print r))"))
apps
runapp fact5
```

`tool-save` validates the source before writing. Tool names are 1-8 safe characters; memory values and tool source are bounded to fit the runtime. FAT32 writes are flushed on physical storage. If the active FAT32 volume is RAM-backed, those writes are only persistent for the current boot session.

## 7. Use the AI Agent

For a natural-language request from the shell:

```text
ask print hello world
```

The model returns one proposed AOS tool action. AOS validates and executes it, captures actual output, and sends the observation back for the next decision. The agent has a small action budget; it may stop or request review when a tool is malformed, unsupported, or unsuccessful. For repetitive tasks, prefer a saved Lisp tool and `runapp` over asking the model to reproduce the same program on every request.

To use the physical direct-Ethernet setup, start local Ollama on the host, boot AOS and note its IPv4 address, then run:

```sh
./setup.sh <host-ethernet-interface> <aos-ip-address>
```

Select the wired interface directly connected to AOS, not the host Wi-Fi device. The setup service asks for a goal and forwards it to AOS; AOS handles task selection and execution. The host still supplies the model endpoint. See `setup.sh` and `tools/ollama_lan_relay.py` for the exact network boundary.

## 8. Test and Contribute

Run the host/agent regression suite in an isolated temporary directory:

```sh
./test_run.sh
```

Before contributing a kernel change:

1. Identify the owning module and public header in `include/`.
2. Make the smallest change at that boundary.
3. Compile touched C files with the flags in `run.sh` or run the full image build.
4. Run relevant tests and `git diff --check`.
5. Boot the new image for behavior that cannot be established by compile-time checks.
6. Record hardware, boot mode, storage backend, and exact commands for manual tests.

Use `docs/blog_docs/FUNCTION_GLOSSARY.html` to locate declarations and definitions. Keep public interfaces documented in their header and update a subsystem guide when behavior changes. Do not describe RAM-backed writes as reboot-persistent or the bounded Lisp evaluator as a security sandbox.

## 9. Suggested Reading Order

1. [Architecture and boot](architecture/bootloader.html)
2. [GDT](architecture/gdt.html) and [IDT](interrupt_io/idt.html)
3. [Physical memory](memory/pmm.html) and [virtual memory](memory/vmm.html)
4. [Tasks and scheduler](core_systems/task_management.html) and [shell](core_systems/shell.html)
5. [FAT32](filesystem/fat32.html), [ATA](drivers/ata_driver.html), and [AHCI](drivers/ahci_driver.html)
6. [lwIP network stack](networking/lwip_stack.html)
7. [AI-assisted OS research brief](../genera_pitch/PITCH_BRIEF.html)
8. [Function glossary](FUNCTION_GLOSSARY.html)
