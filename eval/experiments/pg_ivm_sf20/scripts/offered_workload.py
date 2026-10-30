#!/usr/bin/env python3
"""Offered-workload speedup of the accelerators over materialized views + pg_ivm (rf-fixed sweep, mvcmp).

Both systems are charged for the same work: the reads the MV run completed (N_q per query, each at the system's own mean
read latency under the write load it actually carried) plus the OFFERED write stream of the run -- the first commits of
the deterministic refresh stream until target rows/s x window rows -- whether or not the system kept up.  A commit the
system ran is charged its measured time (maintenance included); one it never reached is charged that system's mean time
for commits of the same kind (rf1 / supplier update / ...), or its mean time per row times the commit's rows.

    speedup = (sum_q N_q * MV_mean_read_q + MV write cost of the offered stream)
            / (sum_q N_q * acc_mean_read_q + acc write cost of the offered stream)

The accelerator's reads were slowed by the commits it really applied; the views' reads saw only the few commits their
writer finished, so the read side is generous to the views.  Per-query geomean: writes split evenly over the 22.
When the recorded stream is shorter than target rows/s x window, it is extended by repeating its own commit mix.
Usage: offered_workload.py [RF_FIXED_DIR (default ../data/postgres/rf-fixed)] [CSV_OUT (default ../data/offered_workload.csv)]
plot.py imports load / mean_reads / ok_commits / charge from here."""
import json, glob, os, sys, math, csv, collections
HERE = os.path.dirname(os.path.abspath(__file__))
RATES = {"interval0s": 0, "interval268.6s": 10, "interval53.7s": 50, "interval13.43s": 200, "interval2.686s": 1000,
         "interval0.537s": 5000, "intervaluncappeds": None}

def load(f):
    r = json.load(open(f)); return r[-1] if isinstance(r, list) else r

def mean_reads(r):
    d = collections.defaultdict(list)
    for q, t, us, st in r["reads_raw"]:
        if st == "ok" and t >= r.get("warmup", 30): d[str(q)].append(us)
    return {q: sum(v) / len(v) for q, v in d.items()}

def ok_commits(r): return [c for c in r["commits_raw"] if c[5] == "ok"]

def charge(stream, own):
    """Write cost (us) of `stream` [(kind, rows)] for a system whose completed commits are `own`."""
    by_kind = collections.defaultdict(list)
    for c in own: by_kind[c[0]].append(c[3])
    per_row = sum(c[3] for c in own) / max(1, sum(c[4] for c in own))
    total = 0.0
    for i, (kind, rows) in enumerate(stream):
        if i < len(own) and own[i][0] == kind: total += own[i][3]
        elif by_kind[kind]: total += sum(by_kind[kind]) / len(by_kind[kind])
        else: total += per_row * rows
    return total


def main():
    D = sys.argv[1] if len(sys.argv) > 1 else f"{HERE}/../data/postgres/rf-fixed"
    OUT = sys.argv[2] if len(sys.argv) > 2 else f"{HERE}/../data/offered_workload.csv"
    rows_out = []
    for iv, rate in RATES.items():
        for d in sorted(glob.glob(f"{D}/{iv}/*/rep*")):
            if not (os.path.exists(f"{d}/mv.json") and os.path.exists(f"{d}/accel.json")): continue
            a, m = load(f"{d}/accel.json"), load(f"{d}/mv.json")
            ma, mm = mean_reads(a), mean_reads(m)
            N = {q: m["per_query"][q]["reads"] for q in m["per_query"]}
            ac, mc = ok_commits(a), ok_commits(m)
            longer = ac if len(ac) >= len(mc) else mc
            target_rows = (rate if rate is not None else a.get("rows_per_s", 0)) * m["seconds"]  # uncapped: what accel offered itself
            stream, acc_rows = [], 0
            for c in longer:
                if acc_rows >= target_rows: break
                stream.append((c[0], c[4])); acc_rows += c[4]
            # Neither system produced target rows/s x window (5,000 rows/s, uncapped): the rest of the offered stream is
            # the recorded stream's own mix repeated (same commit kinds and sizes), each charged at the system's mean
            # measured cost for that kind (charge() does that for commits past the ones it ran).
            short = acc_rows < target_rows
            i = 0
            while acc_rows < target_rows and longer:
                c = longer[i % len(longer)]; stream.append((c[0], c[4])); acc_rows += c[4]; i += 1
            ra = sum(N[q] * ma[q] for q in N); rm = sum(N[q] * mm[q] for q in N)
            wa, wm = charge(stream, ac), charge(stream, mc)
            tot = (rm + wm) / (ra + wa)
            geo = math.exp(sum(math.log((N[q] * mm[q] + wm / 22) / (N[q] * ma[q] + wa / 22)) for q in N) / len(N))
            rows_out.append(dict(rate=("uncapped" if rate is None else rate), run=os.path.relpath(d, D), offered_commits=len(stream),
                                 offered_rows=acc_rows, stream_short=short, mv_read_s=rm / 1e6, mv_write_s=wm / 1e6,
                                 acc_read_s=ra / 1e6, acc_write_s=wa / 1e6, speedup=tot, geomean_per_query=geo,
                                 mv_commits_done=len(mc), acc_commits_done=len(ac), accel_valid=a["valid"]))
    w = csv.DictWriter(open(OUT, "w", newline=""), fieldnames=list(rows_out[0])); w.writeheader(); w.writerows(rows_out)
    for r in rows_out:
        print(f"{str(r['rate']):>8} rows/s {r['run']:30s} offered {r['offered_commits']:4d} commits / {r['offered_rows']:7.0f} rows{' (extended)' if r['stream_short'] else ''} | "
              f"MV {r['mv_read_s']:5.0f}+{r['mv_write_s']:6.0f}s  accel {r['acc_read_s']:5.0f}+{r['acc_write_s']:4.0f}s | speedup {r['speedup']:5.2f}x  geo {r['geomean_per_query']:5.2f}x"
              f"{'' if r['accel_valid'] else '  (accel run invalid)'}")


if __name__ == "__main__":
    main()
