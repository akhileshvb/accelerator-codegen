#!/usr/bin/env python3
"""Q21 C++ <-> Dafny-twin extract-diff, twin half.

Reads the JSON lines produced by q21_twin_trace (the generated C++ module's
observable behaviour on seeded scenarios) and replays every scenario on the
compiled Dafny twin (Q21ModuleTwin.dfy -> Python), with the same harness
protocol:
  * Build -> install images (later images win on a repeated partition id);
  * per transaction: AccumulateMutation over the typed mutations,
    StageMaintenance(dec1 = RebuildAll, dec2 = ApplyDelta: the harness
    policy), then invalidate-all, or publish the updates;
  * per query: an invalid store executes ExecuteFallback on the current tables
    and the store is rebuilt after the round; a valid store runs Execute.
Compared: maintenance action, invalidate-all flag, published partition ids,
store validity at each query, and the emitted rows.

Usage: q21_twin_replay.py <twin-python-dir> < trace.jsonl
Finite testing only (not a universal C++ <-> Twin proof).
"""
import json
import sys

sys.setrecursionlimit(100000)
sys.path.insert(0, sys.argv[1])
import _dafny  # noqa: E402
import Q21Types as TY  # noqa: E402
import Q21ModuleTwin as TW  # noqa: E402

D = TW.default__


def dstr(s):
    return _dafny.Seq([_dafny.CodePoint(c) for c in s])


def dch(c):
    return _dafny.CodePoint(c)


def pystr(s):
    return "".join(str(c) for c in s)


def ei(s):
    return D.IntKeys(s)


def es(s):
    return D.StrKeys(s)


def tdb(db):
    sup = _dafny.Seq([TY.Sup_Sup(r[0], dstr(r[1]), r[2]) for r in db["sup"]])
    li = _dafny.Seq([TY.Li_Li(r[0], r[1], r[2], r[3]) for r in db["li"]])
    ords = _dafny.Seq([TY.Ord_Ord(r[0], dch(r[1])) for r in db["ord"]])
    nats = _dafny.Seq([TY.Nat_Nat(r[0], dstr(r[1])) for r in db["nat"]])
    return TY.TDB_TDB(sup, li, ords, nats)


def install(images):
    store = {}
    for (pid, img) in images:
        store[pid] = img
    return store


def rows_py(rows):
    return [[pystr(r.name), r.numwait] for r in rows]


KIND = {"ins": TW.MutKind_Insert(), "del": TW.MutKind_Delete(), "upd": TW.MutKind_Update()}


def mutation(m):
    if m["t"] == "L":
        conv = lambda rs: _dafny.Seq([TW.OptLi_LiOk(TY.Li_Li(*r)) for r in rs])
        return TW.Mutation_LiMut(KIND[m["k"]], conv(m["old"]), conv(m["new"]))
    if m["t"] == "O":
        conv = lambda rs: _dafny.Seq([TW.OptOrd_OrdOk(TY.Ord_Ord(r[0], dch(r[1]))) for r in rs])
        return TW.Mutation_OrdMut(KIND[m["k"]], conv(m["old"]), conv(m["new"]))
    if m["t"] == "S":
        return TW.Mutation_SupMut()
    return TW.Mutation_NatMut()


def apply_db(db, m):
    """Apply one typed mutation to the plain tables (same edits as the C++ harness)."""
    key = {"L": "li", "O": "ord", "S": "sup", "N": "nat"}[m["t"]]
    tab = db[key]
    for r in m["old"]:
        tab.remove(r)
    for r in m["new"]:
        tab.append(r)


ACTION = {"NoOp": TW.Action_NoOp, "ApplyDelta": TW.Action_ApplyDelta,
          "RebuildPartition": TW.Action_RebuildPartition, "RebuildAll": TW.Action_RebuildAll,
          "InvalidateAndFallback": TW.Action_InvalidateAndFallback}


def action_name(a):
    for k, v in ACTION.items():
        if a == v():
            return k
    return "?"


def main():
    scenarios = mismatches = steps = queries = applied = 0
    for line in sys.stdin:
        sc = json.loads(line)
        scenarios += 1
        db = {k: [list(r) for r in v] for k, v in sc["db"].items()}
        names = sc["names"]
        store = install(D.BuildImages(tdb(db), ei, es))
        valid = True

        def run_queries(expected):
            nonlocal store, valid, mismatches, queries
            rebuilt = False
            for q in expected:
                queries += 1
                nn = q["name"]
                if valid:
                    r = D.Execute(TW.Arg_ArgText(dstr(nn)), False, True, _dafny.Map(store), ei)
                    rows = r.rows if r.is_Emitted else D.ExecuteFallback(tdb(db), TW.Arg_ArgText(dstr(nn)), ei, es)
                else:
                    rows = D.ExecuteFallback(tdb(db), TW.Arg_ArgText(dstr(nn)), ei, es)
                    rebuilt = True
                got = {"valid": valid, "rows": rows_py(rows)}
                want = {"valid": q["valid"], "rows": q["rows"]}
                if got != want:
                    mismatches += 1
                    print(f"seed {sc['seed']} query {nn}: twin={got} cpp={want}")
            if rebuilt:
                store = install(D.BuildImages(tdb(db), ei, es))
                valid = True

        for st in sc["steps"]:
            steps += 1
            if st["txn"] or "action" in st:
                buf = D.EmptyBuffer()
                for m in st["txn"]:
                    buf = D.AccumulateMutation(mutation(m), buf)
                    apply_db(db, m)
                snap = _dafny.Map(store) if valid else _dafny.Map({})
                out = D.StageMaintenance(buf, TW.Action_RebuildAll(), TW.Action_ApplyDelta(), snap, snap, ei, es)
                published = sorted({u[0] for u in out.updates})
                got = [action_name(out.action), bool(out.invalidate__all), published]
                want = [st["action"], st["invalidate_all"], st["published"]]
                if got != want:
                    mismatches += 1
                    print(f"seed {sc['seed']} txn {steps}: twin={got} cpp={want}")
                if out.action == TW.Action_ApplyDelta():
                    applied += 1
                if out.invalidate__all:
                    valid = False
                elif len(out.updates) > 0:
                    for (pid, img) in out.updates:
                        store[pid] = img
            run_queries(st["queries"])
    print(f"twin replay: {scenarios} scenarios, {steps} steps, {queries} queries, "
          f"{applied} ApplyDelta commits, mismatches: {mismatches}")
    sys.exit(1 if mismatches else 0)


if __name__ == "__main__":
    main()
