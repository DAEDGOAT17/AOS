# 08 Keyboard Interaction

**Capability:** Let locally running apps read one queued ASCII key at a time and react to controls.

**Example source:** `(begin (var keycode (key 0)) (if (= keycode 113) (print "quit requested") (print keycode)))`

**Try:** Run the command from the physical console and press a key; `q` is ASCII 113.

**Observe:** The remote JIT connection returns after queuing the app; the shell task runs it and polls the local keyboard.

**Measure:** Input latency, dropped keys, and exit behavior under rapid input.
