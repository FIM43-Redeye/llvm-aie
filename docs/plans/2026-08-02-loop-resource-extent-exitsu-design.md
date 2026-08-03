# Loop resource extent at `ExitSU`

## Problem

Loop-aware scheduling detects resource conflicts across a backedge, but it can
still converge with resources occupied beyond the loop body's final bundle.
That extent is currently used only to pad the loop exit, which does not protect
the next loop iteration.

## Design

Treat `MaxResourceExtent` from the previous fixpoint iteration as an offset on
the bottom region's final `ExitSU` constraints. Apply the offset after normal
instruction latency, delay-slot, implicit-latency, and ZOL constraints have
been combined. This shifts the region boundary without changing the meaning of
those constraints.

When convergence finds a nonzero resource extent, schedule the loop once more.
The next schedule uses that extent as its boundary offset. Convergence succeeds
only after the resulting tail bundles drain the scoreboard and the recomputed
extent becomes zero.

Limit this behavior to loop bottom regions while the existing interblock
scoreboard mode is enabled. Reuse the current `ExitSU`/region-conflict machinery
to materialize empty tail bundles; do not insert NOPs in a post-schedule pass.

## Verification

The issue #1012 MIR regression must change from RED to GREEN with four tail
NOPs. Existing loop-aware resource-convergence tests must remain green, and the
focused AIE scheduler test directory should pass on the remote build host.
