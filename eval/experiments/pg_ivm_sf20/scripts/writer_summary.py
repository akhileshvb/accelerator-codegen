"""Writer-alone summary of a run_sql_backend writerate dir: commit seconds per kind (from commits_raw).
  writer_summary.py DIR            print per-kind times and the total
  writer_summary.py --best D1 D2   print the chunk of the dir with the smallest total write time"""
import glob, json, sys


def load(d):
    f = sorted(glob.glob(f"{d}/**/*.json", recursive=True))
    f = [x for x in f if "seeds" not in x]
    return json.load(open(f[-1])) if f else None


def total(r):
    return sum(c[3] for c in r["commits_raw"] if c[5] == "ok") / 1e6


if sys.argv[1] == "--best":
    rs = [(total(r), r["chunk"]) for r in map(load, sys.argv[2:]) if r]
    print(min(rs)[1])
    sys.exit()
r = load(sys.argv[1])
if not r:
    print("  no result"); sys.exit()
by = {}
for k, s, lag, us, rows, st in r["commits_raw"]:
    by.setdefault(k, []).append((us / 1e6, rows, st))
for k, v in by.items():
    print(f"  {k:18} n={len(v):2}  s={' '.join(f'{x[0]:.1f}' for x in v)}  rows={v[0][1]}  {set(x[2] for x in v)}")
print(f"  chunk {r['chunk']}: total write {total(r):.1f} s, restore {r['restore_s']:.1f} s, restore_ok {r['restore_ok']}, "
      f"refresh {r.get('mv_refresh_s_total', 0):.1f} s")
