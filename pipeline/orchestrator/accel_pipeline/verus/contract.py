"""Glue contract check: does the (LLM-written, trusted) host glue deliver what the
declared kernel inputs say?

The generated C ABI has a record mode: it logs every row the glue passes
(build scans, each transaction's deltas, rescan keys and rows) and returns
without a kernel.  The runner's ``record`` mode dumps the declared SQL
("probes") before the build and before / after each transaction.  Compared:

* each scan's rows == its declared SQL's rows (bags);
* each transaction's delta rows, netted by sign == the declared projection's
  rows after minus before (so updates as -old/+new are fine);
* each rescan's rows == its declared SQL's post-transaction rows whose KEYS
  column is one of the keys (the checker supplies the keys).

Values are compared by SQL type (exact decimals, dates, timestamps, booleans,
text as bytes).  This checks the glue on the test data only; the glue audit
agent reads it for the rest.
"""

from __future__ import annotations

import datetime as dt
import json
import re
import shutil
import struct
from collections import Counter
from decimal import Decimal, InvalidOperation
from pathlib import Path
from typing import Any

from accel_pipeline.verus.context import VerusContext

RESERVED = {"as", "break", "const", "continue", "crate", "else", "enum", "extern", "false", "fn", "for", "if", "impl",
            "in", "let", "loop", "match", "mod", "move", "mut", "pub", "ref", "return", "self", "static", "struct",
            "super", "trait", "true", "type", "unsafe", "use", "where", "while", "async", "await", "dyn", "int", "nat",
            "seq", "set", "map", "spec", "proof", "ghost", "tracked"}


def sanitize(s: str) -> str:
    o = re.sub(r"[^a-z0-9]+", "_", s.lower()).strip("_")
    if not o or o[0].isdigit():
        o = f"c_{o}"
    if o in RESERVED or o.startswith(("g_", "i_")):
        o += "_"
    return o


def parse_inputs(text: str) -> list[dict[str, Any]]:
    body = "\n".join(l for l in text.splitlines() if not l.lstrip().startswith("--"))
    out = []
    for stmt in body.split(";"):
        stmt = stmt.strip()
        if not stmt or ":" not in stmt:
            continue
        head, sql = stmt.split(":", 1)
        toks = head.split()
        d: dict[str, Any] = {"kind": toks[0].lower(), "name": toks[1], "sql": " ".join(sql.split())}
        for i in range(2, len(toks) - 1, 2):
            d[toks[i].lower()] = toks[i + 1]
        out.append(d)
    return out


def _key_output(sql: str, key: str) -> str:
    m = re.search(rf"\b{re.escape(key)}\s+AS\s+([A-Za-z_][A-Za-z0-9_]*)", sql, re.I)
    return m.group(1) if m else key


# ---- normalization ----

def _ns_of(text: str) -> int:
    base, _, frac = text.partition(".")
    t = dt.datetime.strptime(base, "%Y-%m-%d %H:%M:%S") if " " in base else dt.datetime.strptime(base, "%Y-%m-%d")
    days = (t.date() - dt.date(1970, 1, 1)).days
    return ((days * 24 + t.hour) * 60 + t.minute) * 60 * 10**9 + t.second * 10**9 + int((frac + "000000000")[:9] or 0)


def norm_recorded(v: Any, tag: str) -> Any:
    if v is None:
        return None
    if tag == "text":
        return bytes.fromhex(v)
    if tag == "date":
        return (dt.date(1970, 1, 1) + dt.timedelta(days=int(v))).isoformat()
    if tag == "bool":
        return "true" if int(v) else "false"
    if tag == "ts":
        return int(v)
    if tag == "f64":
        return struct.unpack("<d", struct.pack("<Q", int(v)))[0]
    if tag.startswith("dec"):
        return Decimal(int(v)).scaleb(-int(tag[3:])).normalize()
    return int(v)


def norm_sql(v: str, tag: str) -> Any:
    if v == "NULL":
        return None
    try:
        if tag == "text":
            return v.encode("utf-8")
        if tag == "date":
            return v[:10]
        if tag == "bool":
            return "true" if v.lower() in ("true", "1", "t") else "false"
        if tag == "ts":
            return _ns_of(v)
        if tag == "f64":
            return float(v)
        if tag.startswith("dec"):
            return Decimal(v).normalize()
        return int(Decimal(v))
    except (ValueError, InvalidOperation):
        return ("unparsed", v)


def _plan(fields: list[str], columns: list[str]) -> list[tuple[int, str]] | str:
    """For each probe column, the recorded field (index, tag) it corresponds to."""
    named = [(i, f.rsplit(":", 1)[0].removeprefix("row."), f.rsplit(":", 1)[1]) for i, f in enumerate(fields)]
    named = [x for x in named if x[1] != "sign"]
    if len(named) == 1 and len(columns) == 1:
        return [(named[0][0], named[0][2])]
    plan = []
    for c in columns:
        hit = [x for x in named if x[1] == sanitize(c)]
        if not hit:
            return f"no recorded field for column {c} (fields {fields})"
        plan.append((hit[0][0], hit[0][2]))
    return plan


def _rows_rec(rec: dict[str, Any], plan: list[tuple[int, str]]) -> Counter:
    sign_i = next((i for i, f in enumerate(rec["fields"]) if f.rsplit(":", 1)[0].endswith("sign")), None)
    c: Counter = Counter()
    for row in rec["rows"]:
        key = tuple(norm_recorded(row[i], tag) for i, tag in plan)
        c[key] += int(row[sign_i]) if sign_i is not None else 1
    return c


def _rows_probe(probe: dict[str, Any], plan: list[tuple[int, str]], keep=None) -> Counter:
    c: Counter = Counter()
    for row in probe["rows"]:
        if keep and not keep(row):
            continue
        c[tuple(norm_sql(row[j], tag) for j, (_, tag) in enumerate(plan))] += 1
    return c


def _diff(got: Counter, want: Counter) -> dict[str, Any] | None:
    g = {k: v for k, v in got.items() if v}
    w = {k: v for k, v in want.items() if v}
    if g == w:
        return None
    extra = [(repr(k)[:160], v - w.get(k, 0)) for k, v in g.items() if v != w.get(k, 0)][:5]
    missing = [(repr(k)[:160], v - g.get(k, 0)) for k, v in w.items() if v != g.get(k, 0)][:5]
    return {"delivered_not_declared": extra, "declared_not_delivered": missing,
            "delivered_rows": sum(abs(v) for v in g.values()), "declared_rows": sum(abs(v) for v in w.values())}


def check(ctx: VerusContext, runner_call, sql_call, tests_dir: Path, db: Path) -> dict[str, Any]:
    """runner_call(args, env) -> dict; sql_call(query) -> dict (read-only, test DB)."""
    decls = parse_inputs((ctx.ws / "frozen" / "q.inputs").read_text(encoding="utf-8"))
    out = Path(ctx.run_dir) / "contract"
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    probes: list[tuple[str, str]] = []
    keys: list[int] = []
    rescans: dict[str, dict[str, Any]] = {}
    for d in decls:
        n = sanitize(d["name"])
        if d["kind"] == "scan":
            probes.append((f"scan_{n}", d["sql"]))
        elif d["kind"] == "delta":
            probes.append((f"delta_{n}", d["sql"]))
        elif d["kind"] == "rescan":
            probes.append((f"rescan_{n}", d["sql"]))
            kout = _key_output(d["sql"], d["keys"])
            rescans[n] = {"key_out": kout}
            r = sql_call(f'SELECT DISTINCT "{kout}" FROM ({d["sql"]}) AS r ORDER BY hash("{kout}") LIMIT 64')
            keys += [int(Decimal(x[0])) for x in r.get("rows") or [] if x and x[0] != "NULL"]
    (out / "probes.tsv").write_text("".join(f"{n}\t{s}\n" for n, s in probes), encoding="utf-8")
    (out / "keys.txt").write_text(",".join(str(k) for k in keys), encoding="utf-8")
    run = runner_call(["record", str(db), ctx.accel_id, str(tests_dir), str(out / "probes.tsv"), str(out)],
                      {"ACCEL_VERUS_RECORD_DIR": str(out), "ACCEL_VERUS_RECORD_KEYS": str(out / "keys.txt"),
                       "ACCEL_AUX_STORE": "cpp"})
    if "error" in run:
        return {"ok": False, "error": run["error"]}
    problems: list[dict[str, Any]] = []
    checked = Counter()
    declined: list[dict[str, Any]] = []  # transactions / scans the glue rightly refused (NULLs the ABI cannot carry)

    def probe(label: str, name: str) -> dict[str, Any]:
        p = out / f"probe_{label}_{name}.json"
        return json.loads(p.read_text(encoding="utf-8")) if p.is_file() else {"columns": [], "rows": []}

    # segment the log: build, then each transaction
    segments: list[tuple[str, list[dict[str, Any]]]] = []
    for line in (out / "records.jsonl").read_text(encoding="utf-8").splitlines():
        rec = json.loads(line)
        if "marker" in rec:
            segments.append((rec["marker"] if rec["marker"] != "txn" else f"txn{rec['i']}", []))
        elif segments:
            segments[-1][1].append(rec)
    for label, recs in segments:
        if label == "build":
            for d in decls:
                if d["kind"] != "scan":
                    continue
                n = sanitize(d["name"])
                rec = next((r for r in recs if r["fn"] == "build" and r["param"] == n), None)
                pr = probe("build", f"scan_{n}")
                if rec is None:
                    if any("NULL" in row for row in pr.get("rows") or []):
                        checked["declined"] += 1  # the build declined: a NULL the ABI cannot carry
                        declined.append({"at": "build", "why": f"scan {n} returns NULL (outside the ABI)"})
                        continue
                    problems.append({"at": "build", "input": f"scan {n}", "error": "the glue did not deliver it"})
                    continue
                plan = _plan(rec["fields"], pr["columns"])
                if isinstance(plan, str):
                    problems.append({"at": "build", "input": f"scan {n}", "error": plan})
                    continue
                checked["scan"] += 1
                if (d_ := _diff(_rows_rec(rec, plan), _rows_probe(pr, plan))):
                    problems.append({"at": "build", "input": f"scan {n}", **d_})
        elif label.startswith("txn"):
            i = label[3:]
            applies = [r for r in recs if r["fn"] == "apply"]
            if not applies:
                # The glue declined the transaction (nothing reached the kernel).  That is the
                # required behavior when a changed row holds a value the C ABI cannot carry:
                # a NULL where the declared (NOT NULL) schema promises none.  The host then
                # rebuilds / answers by SQL, so it is correct, not a contract failure.
                nulls = []
                for d in decls:
                    if d["kind"] != "delta":
                        continue
                    n = sanitize(d["name"])
                    before, after = probe(f"before_{i}", f"delta_{n}"), probe(f"after_{i}", f"delta_{n}")
                    want = Counter([tuple(r) for r in after["rows"]])
                    want.subtract(Counter([tuple(r) for r in before["rows"]]))
                    nulls += [f"{n}: {row}" for row, c in want.items() if c and "NULL" in row]
                if nulls:
                    checked["declined"] += 1
                    declined.append({"at": f"txn {i}", "why": "a changed row holds NULL (outside the ABI)",
                                     "rows": nulls[:3]})
                    continue
            for d in decls:
                if d["kind"] == "delta":
                    n = sanitize(d["name"])
                    before, after = probe(f"before_{i}", f"delta_{n}"), probe(f"after_{i}", f"delta_{n}")
                    rec = next((r for r in applies if r["param"] == f"{n}_delta"), None)
                    fields = rec["fields"] if rec else None
                    if fields is None:
                        want = Counter([tuple(r) for r in after["rows"]])
                        want.subtract(Counter([tuple(r) for r in before["rows"]]))
                        if any(want.values()):
                            problems.append({"at": f"txn {i}", "input": f"delta {n}",
                                             "error": "the table changed but the glue delivered no changes"})
                        continue
                    plan = _plan(fields, after["columns"])
                    if isinstance(plan, str):
                        problems.append({"at": f"txn {i}", "input": f"delta {n}", "error": plan})
                        continue
                    want = _rows_probe(after, plan)
                    want.subtract(_rows_probe(before, plan))
                    checked["delta"] += 1
                    if (d_ := _diff(_rows_rec(rec, plan), want)):
                        problems.append({"at": f"txn {i}", "input": f"delta {n}", **d_})
                elif d["kind"] == "rescan":
                    n = sanitize(d["name"])
                    rec = next((r for r in applies if r["param"] == f"{n}_rows"), None)
                    krec = next((r for r in applies if r["param"] == f"{n}_keys"), None)
                    if rec is None or krec is None:
                        continue  # no transaction reached apply
                    after = probe(f"after_{i}", f"rescan_{n}")
                    plan = _plan(rec["fields"], after["columns"])
                    if isinstance(plan, str):
                        problems.append({"at": f"txn {i}", "input": f"rescan {n}", "error": plan})
                        continue
                    ks = {int(r[0]) for r in krec["rows"]}
                    kcol = after["columns"].index(rescans[n]["key_out"]) if rescans[n]["key_out"] in after["columns"] else 0
                    checked["rescan"] += 1
                    want = _rows_probe(after, plan, keep=lambda row: row[kcol] != "NULL" and int(Decimal(row[kcol])) in ks)
                    if (d_ := _diff(_rows_rec(rec, plan), want)):
                        problems.append({"at": f"txn {i}", "input": f"rescan {n}", **d_})
    return {"ok": not problems and bool(checked), "checked": dict(checked), "problems": problems[:12],
            "declined": declined[:6], "write_errors": run.get("write_errors")}
