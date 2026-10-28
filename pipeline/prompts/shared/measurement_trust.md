# Trusting a benchmark number

A speedup only describes your accelerator when the reads it timed were served from your state. The
orchestrator checks this on every baseline, attempt and sweep, and refuses a result that fails: a refused
baseline stops the iteration, a refused attempt is neither accepted nor reverted, and a refused sweep
never becomes the best kernel. What it checks, per write-frequency point:

- `accel_hit_rate` at write frequency 0 is above 0.5, and `accel_fallback_reads` there is 0: the reads were
  answered from the state, not by running the fallback SQL. A rate of 0 means the bench timed the fallback.
- `state_builds` is above 0: a state was built.
- The timings are plausible: the accelerated read median is above 0, no speedup is inf or nan, the reference
  query time is in range, the accelerated read is not 10x slower than the fallback, repeated samples agree
  (CV below 0.5), and the bench binary is newer than your query's sources.

When a result is refused, the diagnosis names the cause. The usual ones, in the order to check:

1. The query is not registered with the backend (registration source not compiled in, wrong accelerator id).
2. The first read does not trigger the build, or the build fails and the read falls back silently.
3. The harness reads through another SQL entry than the one the registration declares.
4. A stale binary: rebuild the bench target and rerun.

Do not edit the bench harness, the counters or the thresholds to get a result accepted; fix the accelerator or
its registration. Never read a speedup next to a hit rate below 1.0 at write frequency 0 as a win.
