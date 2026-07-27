# Pipeline orchestrator — remaining work (target state)

Tracked inline in source as `# TODO:` as well. Phase 2+ from the agent pipeline plan.

## Providers

- [x] Cursor SDK backend (`backends/cursor_sdk.py`, `provider: cursor`)
- [x] OpenAI Agents backend (`backends/openai_agents.py`, `provider: openai`)
- [ ] OpenRouter via `LitellmModel` (`models.py`, `pyproject.toml` `[openrouter]` extra)
- [ ] `provider: openrouter` in `config/models.yaml`

## Scaffold & build integration

- [x] CMake aux-state scaffolding (`cmake/AuxState.cmake`, `accel_add_aux_state_query`)
- [ ] Idempotent patch of `src/register.cpp` and `CMakeLists.txt` from scaffold templates
- [ ] `manifest.hpp.tpl` + seed directory scan for `tests/test_support/q{N}_manifest.hpp`

## Gates & artifacts

- [ ] Parse benchmark output into `baseline_summary.json` / structured `profile_summary.json`
- [ ] Optimization retention policy (retain/revert) from gate JSON, not agent claims
- [ ] Skip SF10 gate when change does not affect state sizing

## Memory & context

- [ ] Handbook split into `prompts/03_handbook/*.md` with section selection by bottleneck
- [ ] Playbook structured merge/dedupe (not append-only)
- [ ] Benchmark packs for non-TPC-H workloads (`config/benchmark_packs/`)

## Orchestrator CLI

- [ ] `--queries` batch mode
- [ ] `--branch auto` git branch per run
- [ ] `SQLiteSession` multi-turn per stage with `stage_max_turns` cap

## Hardening

- [x] Aux-state prompt library (`pipeline/prompts/aux_state/shared/`)
- [x] Aux analysis validation (`validation.py`, fitness + mutation_coverage gates)
- [x] Base accelerator prerequisite chaining (`prerequisites.py`)
- [x] Aux speedup verification gate (`run_aux_speedup_verification.sh`)
- [ ] Tool Guardrails (path allowlist enforcement before write)
- [ ] Optional git worktree sandbox for concurrent runs
