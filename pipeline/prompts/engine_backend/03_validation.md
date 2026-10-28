# Engine backend validation (unseen adversarial agent)

You are a **separate** validation agent for **{{display_name}}** (`{{engine_name}}`).
Implementation already shipped sequential layer harnesses. Your job is to
**attack** that backend's slots with **new** tests it has not seen, then fix any
real bugs until those tests pass.


{{engine_host_discovery}}

## Engine profile

Facts and hazards of the engine (not instructions). Aim the adversarial cases at them.

{{engine_profile}}
A profile field given as `{na: reason}` declares that mechanic absent on this engine: it is not a requirement here. Do not fail, implement or test it; only make sure nothing depends on it.

## Inputs

- Backend sources under target paths:
{{engine_target_paths}}
- Known layer harnesses: `backends/{{engine_name}}/tests/test_{{engine_name}}_*.cpp`
  (see `layers.md`)
- Spec: `pipeline/prompts/engine_backend/adversarial_tests.md`
- Analysis (context only): `{{artifacts_path}}/backend_analysis.json`
- Open audit findings, if present: `{{artifacts_path}}/architecture_audit.open_findings.md`. Each one
  is a required fix of this stage, alongside the adversarial tests.
- The pipeline's canary suite already covers scan text views, dictionary and NULL cells, DML without
  rebuilds, concurrent bulk inserts, multi-table registration, fallback snapshot and order, and commit/build
  concurrency (`canary_contract.md`): write adversarial cases it does not.

## Mandates

1. **Do not** weaken, delete, or dilute existing layer harnesses.
2. **Do** author `backends/{{engine_name}}/tests/test_{{engine_name}}_adversarial.cpp` with
   ≥4 **new** cases after reading the implementation.
3. Make `test_{{engine_name}}_adversarial` build (the `backend.cmake` glob registers it) with the **same**
   enabled native/SDK path as the layer suite.
4. Mid-loop until green:
   - `request_gate` → each layer gate already in `post_validation_gates` (regression)
   - `request_gate` → **`engine_adversarial`**
   - On fail: fix the **backend** (preferred) or correct a wrong test; never soft-assert away.
5. Confirm scan uses **native** storage APIs; capture uses real hooks from every
   connection; no runtime flow in the backend; no generated query-accelerator dependency.
6. **Wire `host.sh slot`.** Implement `slot_gates::Engine` for this engine
   (`eval/harness/slot_gates.hpp`; the scenarios are written once there) in
   `backends/{{engine_name}}/tests/test_{{engine_name}}_slot_gates.cpp`, and make
   `slot <scenario> <seconds>` build and run it through `run`. No accelerator exists during a backend
   run: the driver uses only the backend's own toy module from `backends/{{engine_name}}/tests/` (the
   one the `runtime` layer harness runs on a real `RuntimeHost`), never `accelerators/**`:
   - `ReadCases()`: one case per toy read: `accel_sql` calls the toy module's SQL entry (the engine's
     idiom: the test-only entry point the layer harnesses already use), `ref_sql` is the plain SQL
     aggregate over the toy table with the same answer; return after the module is registered and
     built (warm).
   - `Stats()`: the toy module's counters (hits, fallbacks, builds) from the runtime handle.
   - `NewWriteSource()`, `TruncateAndRestore()`, `DdlCases()`: statements on the toy table (its own
     keys; replace, rename and reshape it, then restore it). `ReaderModes()`: the engine's isolation
     levels. `RestartAccelerators()`: restart the toy module's worker where the profile's
     `process_model` has one, else `false`.
   Every scenario runs a driver; 77 only where the profile declares the mechanic n/a (say which field),
   never for a missing driver. `own_writes` and `ddl_events` are never n/a.
7. Write `{{artifacts_path}}/validation_report.md`.

## Gates the orchestrator will re-run

`engine_build`, every layer gate in `layers.md`, **`engine_adversarial`**, **`engine_hit_contract`**
(the layer `api` hit contract: `host_contract.md`), the pipeline-owned canary gates (`engine_canary`,
`engine_canary_asan`, `engine_canary_tsan`; `canary_contract.md`: a failure goes to the owning layer's
session, not to you), then the backend-generic slot gates (`slot_epoch_stress`, `slot_hits_under_writes`,
`slot_read_after_commit`, `slot_txn_edges`, `slot_restart`, `slot_own_writes`,
`slot_ddl_events`, `slot_tsan`;
`pipeline/gates/README_runtime_gates.md`). A slot gate failure is a backend bug:
fix the slot, not the gate. `slot_tsan` reruns the slot scenarios under ThreadSanitizer with
forced tiny slice grains; any report with a frame in substrate/, backends/ or accelerators/
fails it. Required whenever the backend or its modules have parallel code.

Missing adversarial source/target or TODO stubs → **fail**.
