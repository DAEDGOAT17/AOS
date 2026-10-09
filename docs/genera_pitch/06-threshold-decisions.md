# 06 Threshold Decisions

**Capability:** Generate agents that branch on numeric or boolean conditions.

**Example source:** `(if (>= 8 5) (print "threshold met") (print "below threshold"))`

**Try:** Ask the model for a threshold monitor with a user-supplied value and limit.

**Observe:** Host canonicalization maps common comparison aliases into the kernel’s supported expression tree before validation.

**Measure:** Decision correctness across boundary cases and malformed-input rejection.
