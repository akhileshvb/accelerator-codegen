# Optimization workflow (compact)

1. Baseline quick bench measured once per iteration (reused across failed attempts).
2. Agent hypothesizes and patches; orchestrator runs **attempt validation only**:
   - build (with fix loop on failure)
   - smoke ctest ×2 (with fix loop on failure)
   - quick benchmark (0% writes, few queries)
3. Accept iff quick-bench improvement ≥ accept threshold; else revert and next attempt.
4. **End of iteration**: full `post_optimization_gates` (correctness, cross_accel, fair bench sweep, etc.).

Per-attempt: smoke + quick bench only. Heavy gates run once per iteration, not per idea.

One hypothesis per attempt — multiple edits allowed only if all are required to test that hypothesis.
