# Stage 3: Aux-state optimization (iteration {{optimization_iter}})

Tune **read-path performance aggressively** for **Q{{query_number}}**. **One hypothesis per attempt** (may require a few coordinated edits).

## Context

{{retrieval_brief}}

Full analysis (candidate switches): `{{full_analysis_path}}`

**Optimization score** = `read_only_query_speedup` (0% writes: aux hit vs base kernel query latency). Accept/reject uses this metric only. Post-stage gates still run the full write-frequency sweep (`composite_score`).

## Workflow

Orchestrator measures baseline once, then per attempt:
1. **Unified optimization agent** (`03_optimization_unified.md`): one session — read full implementation once, keep design in working memory, hypothesize, implement coordinated edits (header + module + finalize + tests when needed), `HYPOTHESIS_PATCH_COMPLETE`
2. **Orchestrator:** build → smoke correctness → read-only quick benchmark → confirmation → accept/revert

Full structural redesign is allowed within a single attempt when evidence supports it.

## Allowed change kinds (per hypothesis)

**A. Read-path structure** — eliminate residual scans, dense vs map layouts, vectorized decode, operator fusion, pointer-chasing removal. **Preferred.**

**B. Cost-model / maintenance tuning** — only when it directly speeds the read hot path without hurting scalable maintenance.

**C. Candidate switch** — viable alternate from retrieval brief; update `correctness_facts.state_design`.

**D. Structural re-analysis** — `request_structural_reanalysis` as last resort.

Stops after `consecutive_fail_stop` failed attempts (config) or outer iteration gate stall.

{{shared_docs_reference}}

{{constraints}}
