# AOS Research Pitch

Start with [PITCH_BRIEF.md](PITCH_BRIEF.html) for the talk track, tested live demo, research question, and evaluation plan.

## Runtime Model

AOS is an AI-assisted operating environment, not an OS-hosted LLM. The model runs through a local Ollama-compatible endpoint. AOS owns the task lifecycle, tool policy, execution, observation capture, FAT32-backed Lisp tools, and bounded interpretation.

The current native agent can accept `ask <goal>`, run validated Lisp, use OS inspection tools, and write only to `/agent/work`. Persistent Lisp operations are available as `memory-get/set` and `tool-save/read/run`; saved programs use the FAT32-compatible `/agent/lisp/<name>.lsp` convention. The existing interpreter bounds source length, parser nodes/depth, variables, and evaluation steps.

The `kernel-stub` operator is an experimental, narrowly allowlisted interface to the existing JIT. It is not general C compilation, process isolation, or unrestricted kernel self-modification; keep it out of the core demo until live-tested on the presentation image.

## Ten Capability Cards

- [01 Runtime App Creation](01-runtime-app-creation.html)
- [02 Persistent Commands](02-persistent-commands.html)
- [03 Live Redefinition](03-live-redefinition.html)
- [04 Argument-Aware Tools](04-argument-aware-tools.html)
- [05 Stateful Workflows](05-stateful-workflows.html)
- [06 Threshold Decisions](06-threshold-decisions.html)
- [07 Timed Agents](07-timed-agents.html)
- [08 Keyboard Interaction](08-keyboard-interaction.html)
- [09 Text Dashboards](09-text-dashboards.html)
- [10 Hardware-Aware Apps](10-hardware-aware-apps.html)

## Claims To Keep Precise

- Demonstrated: AOS accepted a natural-language task, executed model-proposed Lisp in the kernel, captured output, and completed the task.
- Demonstrated: Lisp memory and saved-tool operators wrote, read, and ran a named calculator on an AHCI-backed FAT32 volume.
- Not demonstrated yet: saved-tool survival across a reboot on the presentation machine.
- Not claimed: an LLM running inside the kernel, arbitrary AI-generated C execution, or unrestricted live kernel rewriting.
