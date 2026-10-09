# 05 Stateful Workflows

**Capability:** Maintain local state across bounded loops for checklists, counters, and multi-step workflows.

**Example source:** `(begin (var step 0) (while (< step 3) (begin (print step) (set! step (+ step 1)))) (print "complete"))`

**Try:** Install it as `steps` and invoke `steps`.

**Observe:** A single interpreted app performs several state transitions and terminates.

**Measure:** Evaluation steps used and behavior when the interpreter fuel limit is reached.
