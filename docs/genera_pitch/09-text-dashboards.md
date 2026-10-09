# 09 Text Dashboards

**Capability:** Draw generated text interfaces with a bounded character canvas.

**Example source:** `(begin (canvas 12 3) (cell 0 0 "A") (cell 1 0 "O") (cell 2 0 "S") (draw))`

**Try:** Install as `banner`, then run it locally with `runapp banner`.

**Observe:** The app updates the actual AOS text display through the generic screen API; no app-specific rendering routine is compiled into the kernel.

**Measure:** Frame update time and maximum useful canvas size on the target display.
