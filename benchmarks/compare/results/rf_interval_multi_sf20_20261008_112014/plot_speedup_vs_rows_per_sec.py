import sys, os
sys.path.insert(0, "/home/akhilvb/accelerator-codegen/benchmarks/compare")
import matplotlib.pyplot as plt, matplotlib.ticker as tk
from plot_rf_multi_sweep import load, rows_per_slot, rep_of
from plot_wps_sweep import stat
from workload import workload_speedup
out = sys.argv[1]; runs = load(out, None); rows = rows_per_slot("/flash1/akhilvb/tpch/refresh_sf20")
SYS = [("cpp", "C++ accelerators", "#2563eb", "o"), ("verus_cpp", "Verus, C++ store protocol", "#d97706", "s"),
       ("verus", "Verus, Verus store protocol", "#dc2626", "^")]
Ts = sorted(runs, reverse=True)
plt.rcParams.update({"font.size": 10, "axes.labelsize": 10, "ytick.labelsize": 9, "xtick.labelsize": 8})
fig, ax = plt.subplots(figsize=(6.5, 4))
print(f"{'T':>4} {'offered':>8}  system: speedup median [min-max], achieved rows/s, valid reps")
for s, label, c, m in SYS:
    pts = []
    for T in Ts:
        vals, ach = [], []
        for r in runs[T].get(s, []):
            nrf = sum(v for k, v in r["write_kinds"].items() if k.startswith("rf"))
            base = rep_of(runs[T]["base"], r.get("rep"))
            sp = workload_speedup(base, r) if base else None
            if nrf >= 2 and sp: vals.append(sp); ach.append(nrf * rows / r["seconds"])
        offered = rows / T
        if len(vals) < 2:
            print(f"{T:>3g}s {offered:>8,.0f}  {s}: no RF1+RF2 cycle committed / reads starved -> not plotted"); continue
        st = stat(vals); a = sorted(ach)[len(ach) // 2]
        pts.append((offered, st, a >= 0.9 * offered))
        print(f"{T:>3g}s {offered:>8,.0f}  {s}: {st[0]:.0f}x [{st[1]:.0f}-{st[2]:.0f}]  achieved {a:,.0f} rows/s  n={len(vals)}")
    xs = [p[0] for p in pts]; ys = [p[1][0] for p in pts]
    ax.plot(xs, ys, "-", color=c, lw=2, label=label, marker=m, ms=7, markevery=[])
    for x, st, ok in pts:
        ax.errorbar([x], [st[0]], yerr=[[st[0] - st[1]], [st[2] - st[0]]], fmt=m, color=c, ms=7, capsize=3,
                    mfc=c if ok else "white", mew=1.8)
    ax.annotate(f"{ys[0]:.0f}x", (xs[0], ys[0]), xytext=(-8, 0), textcoords="offset points", va="center", ha="right", fontsize=9, color="#374151")
    ax.annotate(f"{ys[-1]:.0f}x", (xs[-1], ys[-1]), xytext=(8, 0), textcoords="offset points", va="center", fontsize=9, color="#374151")
ax.plot([], [], "o", color="#6b7280", mfc="white", mew=1.8, label="fell behind target (<90% committed)")
ax.set_xscale("log"); ax.set_yscale("log")
ax.set_xticks([rows / T for T in Ts]); ax.set_xticklabels([f"{rows/T/1e3:.1f}k\n{T:g}s" for T in Ts], fontsize=8)
ax.xaxis.set_minor_locator(tk.NullLocator())
ax.set_xlabel("Offered refresh load, rows touched/s (refresh interval)")
ax.set_ylabel("Workload speedup over DuckDB")
ax.grid(alpha=0.3); ax.legend(frameon=False, fontsize=8, loc="center right")
for sp in ("top", "right"): ax.spines[sp].set_visible(False)
ax.margins(x=0.16); fig.tight_layout()
for ext in ("png", "pdf"):
    p = os.path.join(out, f"speedup_vs_rows_per_sec.{ext}"); fig.savefig(p, dpi=170); print("wrote", p)
