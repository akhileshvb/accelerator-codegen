#!/usr/bin/env python3
import csv, json, math, statistics, sys, os
d = sys.argv[1]
rows = list(csv.DictReader(open(os.path.join(d, "cases.csv"))))
qs = {}
for l in open(os.path.join(d, "qstats.jsonl")):
    r = json.loads(l); qs[r["query"]] = r
def spread(xs, f="%.1f"):
    return "-" if not xs else (f + " [" + f + "-" + f + "]") % (statistics.median(xs), min(xs), max(xs))
print("| Q | speedup x median [min-max] | base ms (median) | accel ms (median) | build s | mismatches | fallbacks | errors/timeouts | requests served |")
print("|---|---|---|---|---|---|---|---|---|")
for q in sorted({int(r["query"]) for r in rows}):
    rs = [r for r in rows if int(r["query"]) == q]
    per = {}
    for r in rs:
        if r["base_us"] and r["accel_us"]:
            per.setdefault(r["rep"], []).append(math.log(float(r["base_us"]) / max(float(r["accel_us"]), 1)))
    sp = [math.exp(sum(v) / len(v)) for v in per.values()]
    bm = statistics.median(float(r["base_us"]) for r in rs if r["base_us"]) / 1000 if any(r["base_us"] for r in rs) else float("nan")
    am = statistics.median(float(r["accel_us"]) for r in rs if r["accel_us"]) / 1000 if any(r["accel_us"] for r in rs) else float("nan")
    mm = sum(1 for r in rs if r["match"] == "False")
    er = sum(1 for r in rs if r["base_status"] != "ok" or r["accel_status"] != "ok")
    st = qs.get(q, {}); a = st.get("stats_after", {}); b = st.get("stats_before", {})
    fb = (a.get("fallback", 0) - b.get("fallback", 0)) if a else "?"
    srv = f'{a.get("requests","?")} {a.get("served","?")}'
    bu = st.get("build_us")
    print(f"| {q} | {spread(sp)} | {bm:.0f} | {am:.2f} | {bu/1e6 if bu else float('nan'):.1f} | {mm}/{len(rs)} | {fb} | {er} | {srv} |")
