# Postpipeliner periodic resource validation

## Problem

The postpipeliner's final resource check replays `NStages` copies of the
modulo schedule. Pipeline stage count describes schedule extraction; it does
not say how many loop periods are needed for the resource scoreboard to reach
steady state.

Issue #1012 exposes the `NStages == 1` case. At `II=23`, partial-word stores
occupy modulo cycles 1 and 22. The next period's first store therefore issues
at cycle 24, only two cycles after the last store, while the existing
`PART_WORD_STORE` itinerary occupies seven cycles. The final check replays only
the first period and misses that modeled conflict.

## Design

Keep the existing final check in
`PostPipeliner::scheduleOtherIterations()`, but replay enough periods to cover
the hazard recognizer's resource pipeline depth:

```text
periods = max(NStages, ceil(PipelineDepth / II) + 1)
```

The extra period exposes the wrap from the last modulo cycle to the first.
When a resource can span more than one complete initiation interval, the
ceiling term supplies the additional periods needed to observe every possible
overlap. Keeping `NStages` as the lower bound preserves existing validation
for multi-stage schedules.

This changes only the authoritative resource validation. Scheduling
heuristics, itinerary definitions, `ExitSU` edges, loop materialization, and
hardware-specific opcode predicates remain unchanged.

## Verification

The focused MIR regression starts the postpipeliner at `II=23`. It must reject
`II=23` through `II=27`, where the periodic `PART_WORD_STORE` reservations
overlap, and accept `II=28`, where the last store at cycle 22 and the next
period's first store at cycle 29 are seven cycles apart.

Run the focused regression first, then the AIE2-family scheduler tests on
`halo`. Repackage and run the original Phoenix harness when `/dev/accel/accel0`
is available. Do not publish or open a pull request.

## Rejected alternatives

- Keeping partial-word stores out of the `LE` bundle encodes an empirical
  hardware observation and still accepts the model-invalid `II=24` schedule.
- Adding `MaxResourceExtent` to `ExitSU` changes the already-safe ordinary
  schedule, while the postpipeliner ignores boundary nodes and replaces it.
- A post-schedule NOP fixup hides the invalid modulo schedule instead of making
  the existing resource model authoritative.
