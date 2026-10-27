# Host bootstrap critique (read-only)

You are a **separate critic** for the **workload harnesses** of
**{{display_name}}** (`{{engine_name}}`). The bootstrap stage wrote
`backends/{{engine_name}}/tests/harness/**`, `backends/{{engine_name}}/bench/**` and
`backends/{{engine_name}}/tests/harness.cmake`, and the `host_bootstrap` gate may
already be green (it only builds the targets and smokes `--help` or one query).
**A green build is not enough.**

Do **not** rewrite harness, CMake or backend sources. Under Codex, native
edit/shell must not touch those — only `pipeline/artifacts/**`.
**The stage is not done until the JSON is on disk.** After the reads below,
write the two files (heredoc `cat > …`) and **stop**.

{{hard_guidelines}}


{{engine_host_discovery}}

## Paths (must match `engine_host` conventions)

- Differential: `{{differential_source}}` (target `{{differential_binary}}`)
- Multi-connection: `{{multi_conn_target}}`; cross-accelerator: `{{cross_accel_target}}`
- Bench: `{{harness_bench_target}}`; calibrate: `{{harness_calibrate_target}}`
- Sources: `backends/{{engine_name}}/tests/harness/`, `backends/{{engine_name}}/bench/`; targets in `backends/{{engine_name}}/tests/harness.cmake`
- Gate JSON: `{{bootstrap_artifacts_path}}/host_bootstrap.json`

## Read (in order)

1. `pipeline/prompts/shared/query_host_bootstrap.md`
2. `pipeline/prompts/host_bootstrap.md` (what each harness must do, its CLI)
3. `backends/{{engine_name}}/api/README.md` and the registration role's headers
4. Every harness source and `harness.cmake`
5. Gate JSON if present

## What to fail

Fail if later per-query accelerators cannot be validated or measured with these
harnesses even though everything built. Typical holes:

- The differential ignores the frozen suite (`ACCEL_SUITE_DIR`: its tuples and `txns/`) or the `ACCEL_ADV_*`
  limits; a harness fails when a write leaves the answer unchanged instead of taking invalidating writes; a
  harness opens a dataset or test fixture for writing instead of its own copy
- A harness is a stub: `--help` and an empty `main`, a comparison that cannot
  fail, no writes in the differential, readers and the writer on one connection
- The differential compares the accelerator with itself or with pinned results
  instead of the engine's own reference SQL on the same snapshot
- A harness names a query, switches on a query number, or includes a per-query
  header (`accelerators/<bench>/q<N>/`)
- Wrong CLI: not `<target> <query>` (differential, multi-conn, cross-accel),
  `<query> <write_pct>` (bench), `--query N <config>` (calibrate); bench does not
  print `read_only_query_speedup,<x>` / `write_mix_workload_speedup,<x>`; calibrate
  writes no interference fields
- It reaches the engine around `backends/{{engine_name}}/api/` and the `host.sh`
  environment (hard-coded path/port/container, the server's own tools by name)
- It reimplements runtime flow (commit ordering, catch-up, builds, thread pools)
  instead of using `substrate/aux/runtime/`, or sizes threads with
  `hardware_concurrency`
- A second adapter tree (duplicate store/aux/txn) instead of the backend's api
- Fixed parameters where the benchmark's generator and `ACCEL_BENCH_ARG_SEED`
  should draw them
- Target names differ from the table above, or they are not declared in
  `tests/harness.cmake`

`violations[].rule` must be one of `sql_entrypoints`, `differential`,
`cmake_target`, `host_reuse`, `independence`, `contract`.

## What not to fail

- Style, extra comments, unused includes
- Per-query accelerators or kernels not generated yet — harnesses are shared
- Demanding another engine's surfaces
- Anything outside the three bootstrap paths above (`host.sh`, `api/`, `backend.cmake`, `eval/`, `substrate/`):
  the bootstrap may not change it, so never ask it to. A defect there goes in `notes`; ask only for fixes inside
  the harness paths (a harness that needs a missing shared source carries its own copy under them)

## Output (mandatory — last action)

The orchestrator **fails this stage** if the JSON is missing. Chat text is not
enough.

1. Write **`{{bootstrap_artifacts_path}}/host_bootstrap_critique.json`**
   conforming to `pipeline/schemas/host_bootstrap_critique.schema.json`.
2. Write **`{{bootstrap_artifacts_path}}/host_bootstrap_critique.md`** (short).
3. Stop. Do not keep reading.

Codex: `cat > {{bootstrap_artifacts_path}}/host_bootstrap_critique.json <<'EOF'`
… `EOF`.

- `status`: `"pass"` or `"fail"`
- `engine_name`: `"{{engine_name}}"`
- On `"fail"`: `violations` + concrete `feedback_for_implementation`
- On `"pass"`: `violations` `[]`, empty `feedback_for_implementation`. `notes`
  may still list nits.
