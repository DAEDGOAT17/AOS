# TinyCC Integration Plan for AOS

This directory is the staging ground for the bare-metal TinyCC port that will eventually compile AI-generated C directly inside the AOS runtime.

Current status:
- AOS now has a symbol export layer and executable page staging in the kernel.
- A host-side validation bridge is available at tools/tcc_driver_bridge.py.
- The kernel-side runtime now includes a minimal freestanding TinyCC-style subset that accepts a simple `int driver_init(void) { return N; }` contract, emits x86_64 return-bytecode into RX pages, and executes it safely from the kernel address space.

Planned path:
1. Port TinyCC to a freestanding build that does not require libc.
2. Replace host `gcc` calls with a small in-kernel TCC state machine.
3. Bind exported kernel APIs using `__ksymtab` and `ksym_lookup()`.
4. Compile driver source into executable pages and validate `driver_init(void)`.
5. Trap faults and return stack dumps over serial for automatic re-synthesis.

The initial in-kernel pass is intentionally small and restricted: it validates the ABI contract, emits a tiny return stub to executable memory, and executes the generated entry before the full TinyCC frontend/backend is ported.