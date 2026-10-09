# AOS: A Persistent, AI-Assisted Operating Environment

## One-Sentence Pitch

AOS is a prototype operating environment for persistent, policy-checked AI work: the OS validates model-proposed actions, runs bounded programs in its own Lisp runtime, and remembers useful tools on persistent storage instead of forcing the same work to be re-created by a host-side script every time.

## Talk Track

Most AI assistants operate outside the system they control. A model proposes commands, a host-side script executes them, and any useful state or workflow disappears after the interaction ends. AOS reframes that relationship: the model proposes an action, the OS checks it against policy, the kernel executes the approved operation, and the result becomes the next observation in a closed loop.

This keeps the control loop inside the environment that owns the tools. AOS does not ask the model to re-derive a routine every time it is needed. Instead, it validates a small program, saves it in the OS-owned filesystem, and reuses it by name. The result is closer to a persistent, programmable operating environment than to a shell-level helper script.

The current prototype is deliberately narrow. It demonstrates live model requests, kernel-side Lisp execution, persistent memory, and saved calculator workflows on FAT32-backed storage. It does not embed the model in the kernel, it does not provide a general-purpose code-execution environment for arbitrary generated C, and it does not permit unrestricted live kernel rewriting.

## Research Question

Can an OS-owned policy and execution loop, combined with persistent bounded programs, increase the reuse of validated workflows while keeping AI-directed actions inspectable, constrained, and explainable?

### Hypothesis

For recurring tasks, saving and invoking a validated OS-native tool will reduce model calls, prompt tokens, and completion latency compared with regenerating or re-planning the same procedure each time, without increasing invalid or rejected actions.

## System Contribution

- **OS-owned action loop:** AOS validates a model-proposed tool, executes it through its own shell/runtime, captures output, and supplies the observation for the next decision.
- **Bounded in-kernel language:** Lisp supports arithmetic, variables, conditionals, loops, display, keyboard input, and selected OS queries under source, parser, and execution-step limits.
- **Persistent agent tools:** `memory-get/set` store short records; `tool-save/read/run` persist validated Lisp programs under `/agent/lisp` and reuse them by name.
- **Constrained runtime extension experiment:** `kernel-stub` reaches the existing narrow JIT contract for a single allowlisted action. It is not general C compilation or unrestricted kernel mutation.

## Three-Minute Demo

Requirements: boot the current AOS ISO, connect the AOS Ethernet link to the host's local Ollama service, and use an AHCI-backed FAT32 volume if demonstrating reboot persistence.

1. Ask the agent to execute a simple program:

   ```text
   ask print hello world
   ```

   Show the model-proposed `lisp` action, the text printed by AOS, and the agent's observed completion.

2. Demonstrate persistent memory:

   ```text
   lisp (print (memory-set "demo" "saved in AOS"))
   lisp (print (memory-get "demo"))
   ```

   Expected output: `1`, then `saved in AOS`.

3. Save a factorial tool, then invoke it separately:

   ```text
   lisp (print (tool-save "fact5" "(begin (var n 5)(var r 1)(var i 1)(while (<= i n)(begin(set! r (* r i))(set! i (+ i 1))))(print r))"))
   apps
   runapp fact5
   ```

   Expected: save returns `1`, `apps` lists `FACT5.LSP`, and `runapp fact5` prints `120`. Use `cat /agent/lisp/FACT5.LSP` to inspect the saved source.

4. Explain the boundary: the LLM runs through the local Ollama endpoint; AOS owns validation, execution, and persistence. The Lisp interpreter is bounded but still runs in kernel context, so this is not a security-isolated sandbox.

## Evaluation Plan

Compare a stateless baseline that asks the model to recreate a procedure for every request with an AOS treatment that saves a validated Lisp tool once and invokes it on repeats. Use the same model, machine, task set, and initial OS state.

- Tasks: repeated arithmetic, status summaries, persistent memory lookup/update, and small stateful workflows.
- Independent variable: stateless re-planning versus saved-tool reuse.
- Primary metrics: model calls, prompt/completion tokens, end-to-end latency, task success rate, and invalid/rejected action rate.
- Reliability checks: malformed Lisp rejection, step-limit termination, storage readback, tool reuse, and persistence after reboot on physical FAT32.
- Report per-task results and failures; do not claim a security sandbox until process isolation exists.

## Current Boundaries

- Model inference is provided by a local Ollama-compatible host service; it is not embedded in the kernel.
- Lisp validation and step limits bound supported programs but do not isolate them from kernel memory or faults.
- FAT32-compatible saved tools use `.lsp` names and short source limits; reboot persistence depends on non-RAM storage and still needs a reboot test on the presentation machine.
- `kernel-stub` is experimental and supports only a narrow existing JIT action contract. Do not present it as general AI-driven kernel rewriting.
- Multi-step planning remains model-dependent; the agent can reject or stop on a malformed or unsupported action.

## Suggested Closing

AOS is a prototype for moving AI agency from the host script into the operating system: the OS becomes the durable place where capabilities are governed, programs are executed, and useful tools are remembered. The research contribution is not an LLM in ring zero; it is testing whether OS-owned policy plus persistent, bounded programs makes agentic workflows more efficient and reproducible.
