# 04 Argument-Aware Tools

**Capability:** Generate reusable shell tools whose behavior depends on user-provided arguments.

**Example source:** `(print (concat "Inspecting: " arg))`

**Try:** Install a command named `inspect`, then run `inspect /agent/db/task.txt`.

**Observe:** The same stored program handles different input without regeneration.

**Measure:** Number of requests served by one definition and the input sizes accepted by the shell.
