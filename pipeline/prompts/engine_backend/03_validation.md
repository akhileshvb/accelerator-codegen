# Engine backend validation (unseen adversarial agent)

You are a **separate** validation agent for **{{display_name}}** (`{{engine_name}}`).
Implementation already shipped sequential layer harnesses. Your job is to
**attack** that adapter with **new** tests it has not seen, then fix any real
bugs until those tests pass.

{{hard_guidelines}}

{{codex_tooling}}

{{engine_host_discovery}}

## Inputs

- Adapter sources under target paths:
{{engine_target_paths}}
- Known layer harnesses: `tests/engine_backend/test_{{engine_name}}_*.cpp`
  (see `layers.md`)
- Spec: `pipeline/prompts/engine_backend/adversarial_tests.md`
- Analysis (context only): `{{artifacts_path}}/backend_analysis.json`

## Mandates

1. **Do not** weaken, delete, or dilute existing layer harnesses.
2. **Do** author `tests/engine_backend/test_{{engine_name}}_adversarial.cpp` with
   ≥4 **new** cases after reading the implementation.
3. Register CMake so `test_{{engine_name}}_adversarial` builds with the **same**
   enabled native/SDK path as the layer suite.
4. Mid-loop until green:
   - `request_gate` → each layer gate already in `post_validation_gates` (regression)
   - `request_gate` → **`engine_adversarial`**
   - On fail: fix the **adapter** (preferred) or correct a wrong test; never soft-assert away.
5. Confirm scan uses **native** storage APIs; mutation uses real hooks; no generated
   query-accelerator dependency.
6. Write `{{artifacts_path}}/validation_report.md`.

## Gates the orchestrator will re-run

`engine_build`, every layer gate in `layers.md`, **`engine_adversarial`**.

Missing adversarial source/target or TODO stubs → **fail**.
