#!/usr/bin/env python3
"""Which maintenance action did each accelerator's policy choose, and what did it cost?

  python3 benchmarks/compare/show_actions.py <multi_cpp.json> [--min-seconds 0]

Reads the per_accel records of a multi run (calibrate / rf): per accelerator the chosen actions (NoOp, ApplyDelta,
RebuildPartition, RebuildAll, InvalidateAndRebuildOnRead) with their count, mean and max PRE_COMMIT time, and the
split by write kind; then, for the RF commits, what the policy saw and predicted for each option next to what the
maintenance actually took (a prediction far from the actual explains a bad choice).  Slowest accelerators first.
"""
import argparse
import json

ap = argparse.ArgumentParser()
ap.add_argument("json")
ap.add_argument("--min-seconds", type=float, default=0.0, help="hide accelerators whose slowest maintenance is shorter")
a = ap.parse_args()
r = json.load(open(a.json))
rows = sorted(r["per_accel"], key=lambda x: -x["pre_commit_max_us"])
print(f"{'accel':8s} {'max s':>8s}  action: n (mean s / max s)   |  by write kind")
for p in rows:
    if p["pre_commit_max_us"] / 1e6 < a.min_seconds:
        continue
    acts = p.get("actions")
    if acts is None:
        raise SystemExit("no 'actions' in this file: it predates the action recording (re-run)")
    parts = [f"{k}: {v['n']} ({v['mean_us'] / 1e6:.2f} / {v['max_us'] / 1e6:.2f})" for k, v in sorted(acts.items(), key=lambda kv: -kv[1]["max_us"])]
    kinds = "; ".join(f"{kind}: " + ",".join(f"{act}x{n}" for act, n in d.items()) for kind, d in p["actions_by_kind"].items()
                      if any(act not in ("NoOp",) for act in d))
    print(f"{p['accel']:8s} {p['pre_commit_max_us'] / 1e6:8.2f}  " + " | ".join(parts) + f"   ||  {kinds}")
    for kind, d in (p.get("decisions_by_kind") or {}).items():
        if kind.startswith("rf") and d["pred_apply_delta_us"] + d["pred_rebuild_all_us"] > 0:
            print(f"{'':8s} {'':8s}    {kind}: {d['rows_touched']:,.0f} rows, {d['partitions']:,.0f} partitions | predicted delta "
                  f"{d['pred_apply_delta_us'] / 1e6:.1f}s, partition {d['pred_rebuild_partition_us'] / 1e6:.1f}s, all "
                  f"{d['pred_rebuild_all_us'] / 1e6:.1f}s, invalidate {d['pred_invalidate_us'] / 1e6:.1f}s | actual {d['actual_us'] / 1e6:.1f}s")
