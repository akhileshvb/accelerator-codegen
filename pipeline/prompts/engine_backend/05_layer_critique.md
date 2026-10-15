# Engine backend layer critique (read-only)

You are a **separate critic** for layer **`{{layer_id}}`** (`{{layer_title}}`) of
**{{display_name}}** (`{{engine_name}}`). Implementation just shipped this layer
and its gate may already be green. **A green harness is not enough.**

Do **not** rewrite adapter sources. Do **not** start later layers.
Under Codex, native edit/shell must not touch adapter, harness, or CMake —
only `pipeline/artifacts/**`. **The stage is not done until the JSON is on
disk.** After the reads below, write the two files (heredoc `cat > …`) and
**stop**. Do not explore later-layer sources.

{{hard_guidelines}}

{{codex_tooling}}

## This layer

- **id:** `{{layer_id}}`
- **gate:** `{{layer_gate}}`
- **harness:** `{{layer_harness}}`
- **CMake target:** `{{layer_cmake_target}}`
- Checklist rules owned here: `{{layer_rules}}`

## Read (in order)

1. `pipeline/prompts/engine_backend/layer_contracts.md` — section `{{layer_id}}`
2. `pipeline/prompts/engine_backend/acceptance_checklist.md` — this layer’s rules
3. `pipeline/prompts/engine_backend/layers.md` — this row only
4. Adapter + harness files for **this layer only** (paths in analysis
   `layers[id={{layer_id}}].files` and `{{layer_harness}}`)
5. Substrate headers those files include (`aux_types.hpp`,
   `versioned_aux_store.hpp`, `aux_registry.hpp` as relevant)
6. Gate JSON if present: `{{artifacts_path}}/gates/{{layer_gate}}.json`

Prior layers (reuse; do not re-litigate style). If **this** layer cannot meet
its contract because a prior layer has a real bug or design flaw, **fail** and
tell implementation to make the smallest prior-layer patch (and keep that
prior harness green). A later layer that already made such a minimal patch is
not a fail.

{{prior_layers}}

{{layer_contracts}}

## What to fail

Fail if **later layers or this layer’s contract** will break even though the
gate passed. Typical holes:

- Harness never exercises a contract path (example: `Invalidate(PartitionSet)`
  while `TryPublish` remaps partition ids, so only `InvalidateAll` was tested)
- Wrong substrate API (a store that re-implements `AuxStateStore` instead of
  deriving from `VersionedAuxStore`, or overrides that skip it; missing
  `AttachServices`; harness `TryPublish` instead of coordinator commit)
- Side-channel APIs the next layer will not call (key registration before
  `CheckValidity`)
- Epoch provider not in the order of `CurrentVisibility().mutation_epoch`, or
  a harness that never asserts `NotVisible` (store layer)
- Server-only symbols in a host shared library (host / txn)

`violations[].rule` must be this layer’s checklist id, or one of
`contract`, `harness_gap`, `substrate_api`.

## What not to fail

- Style, extra comments, unused includes
- Golden values that differ from analysis but still assert real payload fields
- Leftover debug files (`*_mini.cpp`) that are **not** linked — put in `notes`
- Demands that belong to a **later** layer
- Architecture-audit extras beyond this layer’s contracts
- A **minimal** prior-layer bugfix this impl already made so this layer can
  meet its contract (style rewrites of prior layers still fail as `contract`)

## Output (mandatory — last action)

The orchestrator **fails this stage** if the JSON is missing. Chat text is not
enough.

1. Write **`{{artifacts_path}}/layer_critique_{{layer_id}}.json`** conforming to
   `pipeline/schemas/layer_critique.schema.json`.
2. Write **`{{artifacts_path}}/layer_critique_{{layer_id}}.md`** (short).
3. Stop. Do not keep reading.

Codex: `cat > {{artifacts_path}}/layer_critique_{{layer_id}}.json <<'EOF'` … `EOF`.

- `status`: `"pass"` or `"fail"`
- `layer`: `"{{layer_id}}"`
- On `"fail"`: `violations` + concrete `feedback_for_implementation` the
  implementation agent can apply (paths, what to change, which harness asserts
  to add). Do not weaken the harness to greenwash.
- On `"pass"`: `violations` `[]`, empty `feedback_for_implementation`. `notes`
  may still list nits.

If sources for this layer are missing, **fail** (`harness_gap` / `contract`)
and still write the JSON.