# 07 Timed Agents

**Capability:** Pace an app with timer ticks for polling, animation, and periodic status checks.

**Example source:** `(begin (var n 0) (while (< n 3) (begin (print n) (wait 5) (set! n (+ n 1)))))`

**Try:** Install as `pulse`; observe three updates approximately 50 ms apart per wait interval at 100 Hz.

**Measure:** Timing jitter and maximum runtime before the global evaluation budget stops the app.
