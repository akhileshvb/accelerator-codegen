# Engine backend layer critique (read-only)

You are a **separate critic** for layer **`{{layer_id}}`** (`{{layer_title}}`) of
**{{display_name}}** (`{{engine_name}}`). Implementation just shipped this layer
and its gate may already be green. **A green harness is not enough.**

Do **not** rewrite backend sources. Do **not** start later layers.
Under Codex, native edit/shell must not touch backend sources, harness, or CMake —
only `pipeline/artifacts/**`. **The stage is not done until the JSON is on
disk.** After the reads below, write the two files (heredoc `cat > …`) and
**stop**. Do not explore later-layer sources.

{{codex_tooling}}

## This layer

- **id:** `{{layer_id}}`
- **gate:** `{{layer_gate}}`
- **harness:** `{{layer_harness}}`
- **CMake target:** `{{layer_cmake_target}}`
- Checklist rules owned here: `{{layer_rules}}`

## Engine profile (this layer's sections)

{{engine_profile}}
A profile field given as `{na: reason}` declares that mechanic absent on this engine: it is not a requirement here. Do not fail, implement or test it; only make sure nothing depends on it.

## Read

The first message carries the backend digest and this layer's diff: review from them. Open a file
only to check a concrete suspicion, and read this layer's contract section (`layer_contracts.md`,
`{{layer_id}}`) and its checklist rules (`acceptance_checklist.md`) for what to judge against. A re-check
round lists the findings you reported; verify only those and what their fixes touched.

The pipeline's own files (`pipeline/**`: gates, prompts, contracts) are fixed for this run and the
backend cannot change them: judge the backend under `backends/{{engine_name}}/`, never ask for a
change to a gate or a contract.

Prior layers (reuse; do not re-litigate style). If **this** layer cannot meet its contract because a
prior layer has a real bug or design flaw, report it with `file:line` and the smallest patch.

{{prior_layers}}

{{layer_contracts}}

## What to report

The deterministic suites are the correctness floor: this layer's gate runs its harness, and the
pipeline's canary suite (scan text views and NULLs, DML without rebuilds, concurrent capture,
registration by table name, fallback snapshot and order, commit/build concurrency) drives the finished
backend. **Do not ask for more test coverage of those.** Review for what tests cannot see:

- a design flaw the next layer will break on (runtime flow re-implemented in the backend instead of a
  slot, a bypass of `RuntimeHost` / `AcceleratorRuntime`, a side channel the next layer will not call)
- an engine hazard from the profile the code ignores (a lock held across an engine call an engine callback
  can take, an unbounded wait, a process-wide static keyed by engine ids, a missing abort or crash path)
- an epoch not in visibility order, a reader epoch that can be newer than its snapshot, a floor above an
  open reader (epochs); a snapshot that blocks writers or whose epoch `b` does not bound its contents (scan)
- server-only symbols in a host shared library (host / runtime)

Every finding is one of two kinds: `code_fix` (a concrete change in the backend) or `needs_canary` (a
check that is general and missing from the pipeline's suite: say which behaviour it must assert; it
goes to the pipeline's maintainers and never blocks this layer).

## What not to report

- Style, comments, unused includes, golden values that still assert real fields
- Leftover debug files that are not linked (put them in `notes`)
- Demands that belong to a later layer, or to the architecture audit
- A minimal prior-layer bugfix this implementation made so this layer can meet its contract

## Output (mandatory, last action)

The orchestrator records this stage as unavailable if the JSON is missing. Chat text is not enough.

1. Write **`{{artifacts_path}}/layer_critique_{{layer_id}}.json`** conforming to
   `pipeline/schemas/layer_critique.schema.json`, then **`{{artifacts_path}}/layer_critique_{{layer_id}}.md`** (short).
   Codex: `cat > {{artifacts_path}}/layer_critique_{{layer_id}}.json <<'EOF'` ... `EOF`.
2. Stop. Do not keep reading.

- `status`: `"pass"` or `"fail"`; `layer`: `"{{layer_id}}"`.
- On `"fail"`, each `violations[]` item has `rule` (this layer's checklist id, or `contract`,
  `substrate_api`), `severity` (`blocking` or `advisory`), `location` (`path:line`), `contract_item`
  (the clause of `layer_contracts.md` or the checklist it violates), `expected` (the concrete behaviour
  that must hold), `kind` (`code_fix` or `needs_canary`) and `evidence`; plus one
  `feedback_for_implementation` that batches every blocking fix. All blocking findings go back to the
  implementer in one round: do not hold any back.
- On `"pass"`: `violations` `[]` (advisories may stay, `severity: advisory`), empty feedback.

If sources for this layer are missing, **fail** (`contract`) and still write the JSON.
