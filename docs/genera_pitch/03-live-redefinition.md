# 03 Live Redefinition

**Capability:** Replace an app definition under the same command name while AOS continues running.

**Try:** Install `status` as `(print "first version")`, invoke it, then install the same name as `(print "revised version")` and invoke again.

**Observe:** The second behavior takes effect immediately; no kernel rebuild is needed for the app change.

**Measure:** Redefinition latency and whether the persisted file matches the active behavior.
