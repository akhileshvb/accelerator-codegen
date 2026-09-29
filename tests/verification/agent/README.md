# Post-generation verification pipeline

`scripts/verify_query.py` checks a generated accelerator against catalog SQL. The checked fixture in this tree is Q1.

## Command

```text
python3 scripts/verify_query.py
  --query q1
  --catalog-sql <path>          # optional; SHA-256 must match the Logos extract
  --accelerator <cpp-dir>       # default: src/generated/tpch/<q>/postgres/aux
  --model-spec-agent <model>
  --model-sql-agent <model>
  --model-accel-agent <model>
  --model-critique-agent <model>
  --critique-rounds 3
  --repair-rounds 5             # extra Rocq/Dafny rounds while that track's gates are red
  --deterministic-only          # gates and manifest only; no codex exec
```

`--deterministic-only` skips Logos when `tests/verification/formalsql/<q>/generated/Queries.v` exists and `catalog_sql.sha256` matches that SQL. A full run always re-lowers.

## Flow

1. `scripts/logos_lower_catalog.sh q1` extracts `tpch_q1` `base_sql` from `src/generated/catalog/postgres/query_catalog.cpp` and runs `logos-solver check --transform-only`. Success requires `generated/Queries.v`.
2. Canonical spec. If `tests/verification/specs/<q>/canonical_spec.json` already has the required fields, the spec session is not started. Otherwise one `codex_exec` session (`stage=canonical_spec`) writes that file from the catalog SQL, schema, and Logos query term only. It does not read Rocq proofs, Dafny, or C++.
3. Rocq and Dafny start together. Each writes a plan (`runs/<q>/sql_plan.md` or `accel_plan.md`) and closes one slice per round. If that track's gates are still red, the driver starts another round with the plan and the red gates, up to `--repair-rounds` (default 5). A red Rocq gate does not stop the Dafny track.
4. After a track's gates pass, a critique session reviews it. A `revise` verdict is passed back into that track, up to `--critique-rounds` (default 3).
5. If a proof session ran and its gates passed, fold its reuse record into `tests/verification/patterns/CATALOG.json`. Reusable lemmas must already be files under `formalsql/lib/`, `patterns/`, or `substrate/`. Query leaves stay out of the catalog.
6. `tests/verification/runs/<q>/verification_manifest.json` and `summary.md`.

Neither proof session starts until `CANONICAL_SPEC_OK` passes. The canonical spec and the C++ accelerator stay read-only. For a generated query, Rocq writes `proof_manifest.rocq.json` and Dafny writes `proof_manifest.dafny.json`. The driver merges those into `proof_manifest.json`.

## codex_exec

Each session calls `accel_pipeline.backends.codex_exec.CodexExecBackend.run_stage` with `StageRunRequest.agent_model` set from the matching `--model-*` flag. That backend runs:

```text
codex exec --skip-git-repo-check -C <repo>
  -m <model>
  -c model_provider="openrouter"
  --dangerously-bypass-approvals-and-sandbox
  <task.md>
```

Prompts: `session0_canonical_spec.md`, `session1_sql_spec.md`, and `session2_accel.md`. Logs: `session0.log`, `session1.log`, and `session2.log`, plus `session1_repair_N.log` and `session2_repair_N.log` for later rounds. The driver does not resume Codex. Each round is a new `codex exec`. The spec session does not run a checker loop. Tokens an agent prints are ignored unless the gate scripts print them.

## Gates

| Gate | Q1 |
|---|---|
| `LOGOS_OK` | `logos_lower_catalog.sh q1` |
| `CANONICAL_SPEC_OK` | `specs/q1/canonical_spec.json` |
| `SQL_SPEC_PROVED` | `compile_q1_proof.sh`, `compile_q1_normalize_sound.sh` |
| `NO_QUERY_SEMANTIC_RESIDUAL` | `check_q1_no_semantic_residual.sh` |
| `NO_ADMITTED` | `check_q1_rocq_no_admitted.sh` |
| `PRINT_ASSUMPTIONS` | `check_print_assumptions.sh q1` |
| `SPEC_EXPORT_CHECKED` | `check_spec_export.sh` |
| `ACCEL_MODEL_PROVED` | Dafny twin/aux plus `Q1ResultSpecBridge.dfy` |
| `MAINTENANCE_PROVED` | `Q1Maintenance.dfy` and no `assume {:axiom}` |
| `SNAPSHOT_HIT_PROVED` | `Q1RuntimeAuxCorrect.dfy`, `Q1RuntimeResultSpec.dfy` |
| `CPP_MODEL_CORRESPONDENCE` | `run_dafny_q1_extract_diff.sh` → `finite_test` |
| `PATTERN_LIBRARY_UPDATED` | `sql_reuse.json` folded into `patterns/CATALOG.json` when the Rocq session ran |

Both proof sessions are told to read `patterns/CATALOG.json` and to use any entry that applies. A general lemma discovered while proving is written into the shared library by the agent; the driver registers it only after that session's gates pass. A skipped session adds nothing.

`Print Assumptions` runs on `FormalSQLSpecCorrect_*` and `NormalizeSound_*` (and Q1 `NormalizeSound_generic`) and fails if any axiom or admitted constant is outside `formalsql/<q>/allowed_assumptions.txt`. Grep scanners stay in addition to that diff.

## Manifest

`preconditions[]`, `tcb[]`, and `semantic_residuals[]` are separate. Domain, uniqueness, overflow, and successful evaluation are preconditions. Q1 `semantic_residuals` is empty; `NumericEmbedFaithful` is `tcb` kind `numeric_representation`. `SpecExportCorrect` is `rocq_dafny_spec` (LogicalSpec DATA pin + interpreter correspondence manifest + source pins — not SpecApply/FinalizeKeys; dual presence is not semantic equivalence). C++ correspondence is `cpp_model`. Q1 Twin intentional SPI is `AggsFaithful` / free `RebuildOracleCorrect` only — **not** a catch-all `twin_aux_refinement` list (see `tests/verification/q1/CURRENT_THEOREM_TCB.md`). Model Hit/Exec proves ≈ generic Eval_Spec OutRows — not ModuleTwin/C++ alone.

## Still seeded

Q1 already has a seeded spec and proof, so those sessions are skipped while the gates stay green. `--deterministic-only` skips the agents and the critique.
