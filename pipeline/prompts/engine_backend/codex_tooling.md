# Engine backend stages: tools and pass bars

The pass bar is the MCP gate (`run_engine_build`, `run_layer_gate`, `request_gate`); host cmake, ninja or `docker exec` runs are for debugging only.

## Layer implementation

Implementation is **sequential layers** (`layers.md`): `capture` → `epochs` →
`scan` → `runtime` → `host` (host skipped when not required).

1. Implement **only this layer’s** objects + `backends/<engine>/tests/test_<engine>_<layer>.cpp`.
2. Wire CMake so this layer’s target actually builds.
3. **Steer with MCP tools on server `accel_pipeline`** (these are **tools**, not
   resources — do **not** `list_mcp_resources`):
   - `run_engine_build()` after CMake / adapter TU changes
   - `run_layer_gate()` after the harness should run (maps `impl_<layer>` → `engine_<layer>`)
   - or `request_gate` with `engine_build` then this layer’s gate
     (`engine_capture`, `engine_epochs`, `engine_scan`, `engine_runtime`,
     `engine_host`)
4. On fail: read `failure_summary` / `log_tail`, fix, call the MCP tool again.
   **Do not** treat host `cmake`, `ninja`, or `docker exec` of the harness as
   the pass bar — MCP is the official in-container (or host_cmake) gate.
5. Do **not** author `test_<engine>_adversarial.cpp` during implementation.
6. Do not implement later layers' contracts (this layer's own wiring in their files is yours), and do not finish on cmake-only success or a question.
7. **Prior-layer patch (narrow):** if this layer is blocked by a real bug or
   design flaw in an earlier layer, make the smallest fix there, keep that
   prior harness green (add the missing assert if needed), and `request_gate`
   that prior gate before continuing. Do not rewrite prior layers for style.

## Validation stage

1. Read the adapter + existing **layer** harnesses.
2. Author unseen `backends/<engine>/tests/test_<engine>_adversarial.cpp` (≥4 new cases).
3. Loop `request_gate` for layer gates (regression) and **`engine_adversarial`** until pass.
4. Fix adapter bugs found by adversarial tests; never weaken asserts to greenwash.

## Engine backend layer critique

You are a **read-only critic**. Do **not** edit adapter, harness, or CMake sources
(native Codex tools included). Write only
`pipeline/artifacts/engines/<engine>/layer_critique_<layer>.json` and the
matching `.md`; write them with a shell heredoc.
A green layer gate is not enough — fail on untested contract paths.

**Done = files on disk.** Last action: heredoc-write the JSON then the `.md`,
then stop. Do not explore later-layer trees. If you have enough to judge,
write immediately (`cat > pipeline/artifacts/engines/<engine>/layer_critique_<layer>.json <<'EOF'`).
The orchestrator fails the stage if that JSON is missing.

## Engine backend architecture audit

Same write rule: last action is `architecture_audit.json` + `.md` under
`pipeline/artifacts/engines/<engine>/`. Do not end without those files.
