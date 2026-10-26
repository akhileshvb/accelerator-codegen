# Prompts

One directory per pipeline. In each, the top-level files are stage prompts, numbered in stage order;
`docs/` holds the material those prompts inline (`{{constraints}}`, `{{hard_guidelines}}`, ...) or name
for the agent to read when it needs it.

| Directory | Pipeline | Selected by |
|---|---|---|
| `base/` | stateless query kernels | query workloads without `workload_kind` |
| `aux_state/` | stateful query accelerators | `workload_kind: aux_state` |
| `engine_backend/` | an engine's backend, layer by layer | `workload_kind: engine_backend` |
| `verus/` | verified accelerators (`--mode verus`) | its own renderer, `accel_pipeline/verus/agent.py` |
| `shared/` | stages and docs the C++ pipelines share | fallback for the three above |

A C++ stage renders `prompts/<prompt_subdir>/<name>`, else `prompts/shared/<name>`
(`accel_pipeline.prompts.resolve_prompt_path`). The shared stages are crash debugging
(`04_crash_debug.md`) and the query-host bootstrap (`host_bootstrap.md`, `host_bootstrap_critique.md`);
`shared/compact_prompt.md` is Codex's context-compaction prompt (`config/models.yaml`).

Verus keeps its rules in `verus/contracts/`, copied into each agent's workspace as `contracts/`, and the
fragments its prompts include in `verus/shared/`.

Benchmark-specific facts do not live here: they are in `config/benchmark_packs/<benchmark>.yaml`
(`prompt_notes`, `aux_prompt_notes`).
