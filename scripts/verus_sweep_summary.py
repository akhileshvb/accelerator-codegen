"""Summarize verus_kit_runner bench results written by run_verus_sf20_sweep.sh.

    python3 scripts/verus_sweep_summary.py <out_dir>          # table of all q*.json -> summary.md
    python3 scripts/verus_sweep_summary.py <out_dir> q3       # one line for one query
"""
import json
import pathlib
import sys


def load(path: pathlib.Path):
    try:
        return json.loads(path.read_text().strip().splitlines()[-1])
    except (OSError, ValueError, IndexError):
        return None


def line(d) -> str:
    mixes = "  ".join(f"{m['write_fraction']:g}:{m['speedup']:.1f}x" for m in d["mixes"])
    return (f"  build {d['build_ms'] / 1000:.1f}s  read {d['read_speedup']:.1f}x  {mixes}  "
            f"write {d['write_us_mean'] / 1000:.1f}ms vs plain {d['plain_write_us_mean'] / 1000:.1f}ms  "
            f"[{d.get('write_mode', '?')}]")


def table(out: pathlib.Path) -> str:
    files = sorted(out.glob("q*.json"), key=lambda p: int(p.stem[1:]))
    results = {f.stem: load(f) for f in files}
    fractions = sorted({m["write_fraction"] for d in results.values() if d for m in d["mixes"]})
    rows = ["| query | writes | build | read | " + " | ".join(f"{k:g} writes" for k in fractions) + " |",
            "|" + "---|" * (4 + len(fractions))]
    for q, d in results.items():
        if not d:
            rows.append(f"| {q} | failed |" + " |" * (2 + len(fractions)))
            continue
        speed = {m["write_fraction"]: m["speedup"] for m in d["mixes"]}
        cells = " | ".join(f"{speed[k]:.1f}x" if k in speed else "" for k in fractions)
        rows.append(f"| {q} | {d.get('write_mode', '?')} | {d['build_ms'] / 1000:.1f}s | "
                    f"{d['read_speedup']:.1f}x | {cells} |")
    return "\n".join(rows) + "\n"


if __name__ == "__main__":
    out = pathlib.Path(sys.argv[1])
    if len(sys.argv) > 2:
        d = load(out / f"{sys.argv[2]}.json")
        print(line(d) if d else f"  {sys.argv[2]}: no result")
    else:
        text = table(out)
        (out / "summary.md").write_text(text)
        print(text)
