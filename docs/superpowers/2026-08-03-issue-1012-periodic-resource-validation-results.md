# Issue #1012 periodic resource validation results

## Scope

This was a local investigation and implementation experiment. Nothing was
pushed, no pull request was opened, and no GitHub comment was posted.

## Finding

The bug is in the postpipeliner's final resource validation, not in loop-end
materialization or the AIE2 itinerary model.

`scheduleOtherIterations()` used `NStages` as the number of modulo periods to
replay into a pristine resource scoreboard. For a single-stage schedule it
therefore checked only one period and could not see a resource conflict across
the loop wrap.

The issue kernel has four partial-word stores. `PART_WORD_STORE` is occupied
for seven cycles. With the regression constrained to start at `II=23`, stores
at modulo cycles 22 and 1 are only two cycles apart across the wrap. `II=23`
through `II=27` are illegal; `II=28` is the first legal schedule available to
that test.

The implemented check replays enough periods to cover the hazard recognizer's
pipeline depth:

```text
periods = max(NStages, ceil(PipelineDepth / II) + 1)
```

This leaves scheduler heuristics, itineraries, `ExitSU` edges, and loop
materialization unchanged.

## Test-first result

Before the replay change, the new MIR regression failed because the validator
accepted a model-invalid single-stage schedule. The diagnostic build used for
the RED run selected `II=24`; the test requires `II=28`.

After the change:

- Focused regression and affected neighboring test: 2 passed.
- AIE2/AIE2P/AIE2PS scheduler tests: 354 passed, 3 unsupported.
- Full AIE CodeGen suite: 2,249 passed, 5 expected failures, 3 unsupported.

One existing AIE2P exact-schedule test legitimately changed from `II=11` to
`II=12`. In its old schedule, `st.s8` issued at cycle 5 and used `R_RS_PORT` at
cycle 11. The next iteration's `MOVS` also used `R_RS_PORT` at cycle 11. The
new periodic check exposes that modeled conflict, so the generated expectation
was updated.

## Original kernel artifact

The original issue kernel was rebuilt with the fixed compiler on `halo`.
The resulting loop uses `II=29`, with stores at cycles 1, 9, 16, and 23 and an
empty loop-end bundle at cycle 28. The last store and the next iteration's first
store are seven cycles apart. The problematic `st.s8` is no longer in `LE`.

Local artifacts:

- `/tmp/issue1012-periodic-fix-2026-08-03/issue1012-fixed-fuzz_kernel.cc.o`
- `/tmp/issue1012-periodic-fix-2026-08-03/issue1012-fixed-fuzz_kernel.disasm`
- `/tmp/issue1012-periodic-fix-2026-08-03/issue1012-fixed-fuzz_kernel.remarks`
- `/tmp/issue1012-periodic-fix-2026-08-03/issue1012-fixed-fuzz_kernel.opt.yaml`

The Phoenix run is still pending because `/dev/accel/accel0` is absent and
`xrt-smi examine` currently reports zero devices. The fixed object is ready to
package and run when the NPU returns.

## Local commits

- `203c770a9cff` documents the approved design.
- `9dc87f8d5d06` implements and tests the periodic validation.

The result supports the periodic scoreboard replay as the sound fix. It does
not encode the empirical observation that a partial-word store fails in the
loop-end bundle.
