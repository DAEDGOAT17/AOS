# 10 Hardware-Aware Apps

**Capability:** Generate system-health apps that query existing AOS hardware/status primitives.

**Example source:** `(begin (uptime) (diskinfo))`

**Try:** Install as `health` and run `health` on physical AOS.

**Observe:** The app can report uptime, backend type, sector count, and FAT32 label; the AI host can use that output to select a future improvement proposal.

**Boundary:** This reports capabilities; it does not generate or install a hardware driver yet. A separate verified driver lifecycle is required for that next research step.
