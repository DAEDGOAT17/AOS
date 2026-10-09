# 01 Runtime App Creation

**Capability:** Turn a natural-language request into a new named AOS command while the OS is running.

**Research claim:** The AI creates application logic, not a hardcoded kernel feature. AOS parses the result, rejects unsupported forms, persists accepted source, and runs it through the interpreter.

**Try:** `python3 -B tools/agent_test.py --host 192.168.77.10 --generate-lisp-command ready --lisp-prompt 'Make a concise readiness report using uptime and diskinfo' --launch-after-install`

**Observe:** The source is printed by the host, then JIT installation and shell-task launch results are reported.

**Measure:** Prompt-to-valid-app rate, rejection reason, and time to first execution.
