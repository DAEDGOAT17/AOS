# AOS Live-App Research Pitch

AOS can accept an AI-generated Lisp app through the JIT installer, validate it against a bounded grammar, persist it on FAT32, and run or redefine it without rebuilding the kernel for each app. The current physical test target is the booted AOS at `192.168.77.10`.

## Demo Flow

1. Ask the local model for one AOS Lisp app: `python3 -B tools/agent_test.py --host 192.168.77.10 --generate-lisp-command <name> --lisp-prompt '<request>' --launch-after-install`.
2. The host validates/canonicalizes the response and sends one installer call over JIT.
3. AOS validates and writes the source to `/agent/lisp/<name>.lsp` on FAT32.
4. `runapp <name>` queues execution on the shell task, so interactive loops do not run in the network callback.
5. The app observes its result; the user can request a revised definition under the same name and install it again.

## Ten Capability Cards

- [01 Runtime App Creation](01-runtime-app-creation.md)
- [02 Persistent Commands](02-persistent-commands.md)
- [03 Live Redefinition](03-live-redefinition.md)
- [04 Argument-Aware Tools](04-argument-aware-tools.md)
- [05 Stateful Workflows](05-stateful-workflows.md)
- [06 Threshold Decisions](06-threshold-decisions.md)
- [07 Timed Agents](07-timed-agents.md)
- [08 Keyboard Interaction](08-keyboard-interaction.md)
- [09 Text Dashboards](09-text-dashboards.md)
- [10 Hardware-Aware Apps](10-hardware-aware-apps.md)

## Current Runtime Contract

Supported code is interpreted Lisp: `begin`, `var`, `let`, `set!`, `if`, `while`, arithmetic/comparison/boolean forms, `concat`, `print`, `key`, `wait`, `canvas`, `cell`, `draw`, `uptime`, and `diskinfo`. Source, syntax-tree size, nesting, variable count, operand count, and evaluation steps are bounded. The interpreter does not expose arbitrary native calls, direct filesystem writes, MMIO, DMA, or network operations.

A one-time kernel build and boot is needed when changing the interpreter itself. Individual Lisp apps are installed and redefined at runtime. Reboot persistence should be checked on the physical FAT32 target after each app install.
