# 02 Persistent Commands

**Capability:** Store a generated app as `/agent/lisp/<name>.lsp` on FAT32 and invoke it by command name after installation.

**Try:** Install `greet` with `(print (concat "Hello " arg))`, then invoke `greet researcher`.

**Observe:** The command receives its tail text through `arg`; the stored source can be read with `cat /agent/lisp/greet.lsp`.

**Measure:** Compare the installed definition before and after reboot. Runtime persistence requires physical FAT32, not the embedded RAM disk.
