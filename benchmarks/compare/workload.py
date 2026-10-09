"""The workload speedup over DuckDB, one definition for every aux_compare plot (concurrent and multi runs alike).

The work is the reads the DuckDB run completed in its measured window (N_q per accelerator) plus the shared write
stream; each system is charged its own mean read latency for those reads and its own commit time (maintenance
included) over its measured window:

    speedup = (sum_q N_q * DuckDB mean read_q + DuckDB commit time) / (sum_q N_q * system mean read_q + system commit time)

A concurrent run (one accelerator) has one q; a multi run (every accelerator in one process, one writer maintaining
all of them) sums over its accelerators.
"""


def key(r):
    return r["accel"].replace("_vgen", "").split(":")[-1].replace("tpch_q", "")


def _reads(r):
    return {key(p): p for p in r["per_accel"]} if "per_accel" in r else {"": r}


def _commit_us(r):
    return r["commit_sum_us"] if "commit_sum_us" in r else r.get("commit_total_us", 0.0)


def workload_speedup(base, r):
    """base: the DuckDB run, r: the system's run of the same write stream and draws. None if r misses a query."""
    b, s = _reads(base), _reads(r)
    tb, ts = _commit_us(base), _commit_us(r)
    for q, p in b.items():
        x = s.get(q)
        if x is None or not x["reads"] or "read_mean_us" not in x:
            return None
        tb += p["reads"] * p["read_mean_us"]
        ts += p["reads"] * x["read_mean_us"]
    return tb / ts if ts > 0 else None


def workload_speedup_per_query(base, r):
    """{query: speedup} for a multi run, the same formula per accelerator q: its N_q reads at each system's mean read
    latency (store waits behind other accelerators' maintenance included), q's own maintenance (pre-commit and install
    per commit), and an equal 1/n share of what every commit costs regardless of accelerator (the SQL statement with
    mutation capture, the engine commit; for DuckDB its whole commit time).  The per-query numerators and denominators
    sum to workload_speedup's, so the aggregate is the read-time-weighted combination of these."""
    b, s = _reads(base), _reads(r)
    n_acc = len(b)
    w = r.get("writes", 0)
    if "commit_breakdown_mean_us" in r:
        shared = w * (r["commit_breakdown_mean_us"]["statement"] + r["commit_breakdown_mean_us"]["engine_commit"])
    else:  # no maintenance (DuckDB, stateless): the whole commit is shared
        shared = _commit_us(r)
    out = {}
    for q, p in b.items():
        x = s.get(q)
        if x is None or not x["reads"] or "read_mean_us" not in x:
            continue
        own = w * (x.get("pre_commit_mean_us", 0.0) + x.get("install_mean_us", 0.0))
        tb = p["reads"] * p["read_mean_us"] + _commit_us(base) / n_acc
        ts = p["reads"] * x["read_mean_us"] + shared / n_acc + own
        out[q] = tb / ts if ts > 0 else None
    return out
