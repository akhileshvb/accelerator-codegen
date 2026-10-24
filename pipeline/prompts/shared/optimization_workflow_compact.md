# Optimization workflow (compact)

1. The baseline quick bench is measured once per iteration and reused across failed attempts.
2. The agent hypothesizes and patches. The orchestrator validates the attempt: build (with a fix loop), smoke ctest x2 (with a fix loop), quick benchmark.
3. Accept iff the quick-bench score improves by at least the accept threshold; otherwise revert and try the next attempt.
4. End of iteration: the full `post_optimization_gates` run once (correctness, cross_accel, fair bench sweep, ...).

One hypothesis per attempt; several edits only when all are needed to test it. The negative memory in the runtime section is advisory: prefer the current profile and scoreboard. The agent declares `OPTIMIZATION_SATURATED` when both axes are exhausted; there is no target speedup number.
