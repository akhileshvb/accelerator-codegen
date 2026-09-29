#!/usr/bin/env python3
"""Emit Dafny RelNode LogicalSpec DATA from Rocq-serialized logical_spec_relnode.dump.json.

Usage:
  emit_relnode_logical_spec_dafny.py q1
Writes:
  tests/verification/q1/dafny/generated/Q1RelNodeData.dfy

Fail-closed: unsupported constructors abort. No evaluator / Adapt code.
Authority: Rocq LogicalSpec_Q1_rel (serialize_LogicalSpec_Q1_rel_export_json).
"""

from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]


def die(msg: str) -> None:
    print(f"emit_relnode_logical_spec_dafny FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def emit_ty(t: dict) -> str:
    kind = t.get("kind")
    if kind == "int":
        return "TInt"
    if kind == "numeric":
        return f"TNumeric({int(t['scale'])})"
    if kind == "bool":
        return "TBool"
    if kind == "date":
        return "TDate"
    if kind == "text":
        return "TText"
    if kind == "unknown":
        return "TTyUnknown"
    die(f"unsupported ty {kind!r}")


def emit_val(v: dict) -> str:
    kind = v.get("kind")
    if kind == "null":
        return "VNull"
    if kind == "int":
        return f"VInt({int(v['value'])})"
    if kind == "bool":
        return "VBool(true)" if v.get("value") is True else "VBool(false)"
    if kind == "text":
        s = str(v.get("value", "")).replace("\\", "\\\\").replace('"', '\\"')
        return f'VText("{s}")'
    if kind == "avg_exact":
        return f"VAvgExact({int(v['sum'])}, {int(v['count'])})"
    die(f"unsupported value {kind!r}")


BOP = {
    "add": "BAdd",
    "sub": "BSub",
    "mul": "BMul",
    "div": "BDiv",
    "eq": "BEq",
    "ne": "BNe",
    "lt": "BLt",
    "le": "BLe",
    "gt": "BGt",
    "ge": "BGe",
    "and": "BAnd",
    "or": "BOr",
    "like": "BLike",
    "prefix": "BPrefixMatch",
}
UOP = {
    "neg": "UNeg",
    "not": "UNot",
    "is_null": "UIsNull",
    "is_not_null": "UIsNotNull",
}


def emit_expr(e: dict) -> str:
    kind = e.get("kind")
    if kind == "input_ref":
        return f"EInputRef({int(e['slot'])})"
    if kind == "const":
        return f"EConst({emit_val(e['value'])}, {emit_ty(e['ty'])})"
    if kind == "param":
        pid = e.get("id")
        if not isinstance(pid, str):
            die("param id must be string")
        return f'EParam("{pid}", {emit_ty(e["ty"])})'
    if kind == "unary":
        op = UOP.get(e.get("op"))
        if op is None:
            die(f"unsupported unary op {e.get('op')!r}")
        return f"EUnary({op}, {emit_expr(e['expr'])})"
    if kind == "binary":
        op = BOP.get(e.get("op"))
        if op is None:
            die(f"unsupported binary op {e.get('op')!r}")
        return f"EBinary({op}, {emit_expr(e['left'])}, {emit_expr(e['right'])})"
    if kind == "group_ref":
        return f"EGroupRef({int(e['i'])})"
    if kind == "agg_ref":
        return f"EAggRef({int(e['i'])})"
    if kind == "cast":
        return f"ECast({emit_expr(e['expr'])}, {emit_ty(e['ty'])})"
    if kind == "function":
        args = ", ".join(emit_expr(a) for a in e.get("args") or [])
        return f'EFunction("{e["id"]}", [{args}])'
    if kind == "case":
        die("ECase not required for Q1 RelNode DATA this pass")
    die(f"unsupported expr kind {kind!r}")


AGG = {
    "sum": "AggSum",
    "avg": "AggAvg",
    "count": "AggCount",
    "count_star": "AggCountStar",
    "min": "AggMin",
    "max": "AggMax",
}


def emit_agg(a: dict) -> str:
    kind = a.get("kind")
    ak = AGG.get(kind)
    if ak is None:
        die(f"unsupported agg {kind!r}")
    if kind == "count_star":
        return f"AggSpec({ak}, NoExpr)"
    if "expr" not in a:
        die(f"agg {kind} missing expr")
    return f"AggSpec({ak}, SomeExpr({emit_expr(a['expr'])}))"


def emit_order(k: dict) -> str:
    d = k.get("dir")
    if d == "asc":
        sd = "SortAsc"
    elif d == "desc":
        sd = "SortDesc"
    else:
        die(f"unsupported sort dir {d!r}")
    n = k.get("nulls")
    if n == "first":
        nn = "NullsFirst"
    elif n == "last":
        nn = "NullsLast"
    else:
        die(f"unsupported nulls {n!r}")
    return f"OrderKey({emit_expr(k['expr'])}, {sd}, {nn})"


def emit_rel(n: dict) -> str:
    kind = n.get("kind")
    if kind == "scan":
        return f'RelScan("{n["table"]}")'
    if kind == "filter":
        return f"RelFilter({emit_rel(n['input'])}, {emit_expr(n['pred'])})"
    if kind == "project":
        es = ", ".join(emit_expr(e) for e in n["exprs"])
        return f"RelProject({emit_rel(n['input'])}, [{es}])"
    if kind == "aggregate":
        gs = ", ".join(emit_expr(e) for e in n["group_exprs"])
        ag = ", ".join(emit_agg(a) for a in n["aggs"])
        return (
            f"RelAggregate({emit_rel(n['input'])}, [{gs}], [{ag}])"
        )
    if kind == "sort":
        oks = ", ".join(emit_order(k) for k in n["order_keys"])
        return f"RelSort({emit_rel(n['input'])}, [{oks}])"
    if kind == "distinct":
        return f"RelDistinct({emit_rel(n['input'])})"
    if kind == "limit":
        return (
            f"RelLimit({emit_rel(n['input'])}, "
            f"{int(n['offset'])}, {int(n['count'])})"
        )
    if kind == "join":
        jk = n.get("join_kind", "inner")
        kind_map = {
            "inner": "JoinInner",
            "left": "JoinLeft",
            "right": "JoinRight",
            "full": "JoinFull",
            "cross": "JoinCross",
        }
        if jk not in kind_map:
            die(f"unsupported join_kind {jk!r}")
        return (
            f"RelJoin({kind_map[jk]}, {emit_rel(n['left'])}, "
            f"{emit_rel(n['right'])}, {emit_expr(n['cond'])})"
        )
    if kind == "semi_join":
        return (
            f"RelSemiJoin({emit_rel(n['left'])}, {emit_rel(n['right'])}, "
            f"{emit_expr(n['cond'])})"
        )
    if kind == "anti_join":
        return (
            f"RelAntiJoin({emit_rel(n['left'])}, {emit_rel(n['right'])}, "
            f"{emit_expr(n['cond'])})"
        )
    die(f"unsupported rel kind {kind!r}")


def seq_emit(items: list[str], indent: str = "      ") -> str:
    if not items:
        return "[]"
    inner = (",\n" + indent).join(items)
    return "[\n" + indent + inner + "\n    ]"


def main() -> None:
    if len(sys.argv) != 2 or sys.argv[1] != "q1":
        die("usage: emit_relnode_logical_spec_dafny.py q1")
    query = sys.argv[1]
    dump_path = (
        ROOT / "tests/verification/specs" / query / "logical_spec_relnode.dump.json"
    )
    if not dump_path.is_file():
        die(f"missing {dump_path}")
    wrapped = json.loads(dump_path.read_text(encoding="utf-8"))
    if wrapped.get("authority") != "rocq_serialize_LogicalSpec_Q1_rel_json":
        die("unexpected authority")
    body = wrapped.get("export")
    if not isinstance(body, dict):
        die("missing export")
    if body.get("version") != "logical_spec_relnode_v1":
        die("unexpected version")

    table = body.get("table_lineitem")
    if table != "lineitem":
        die("table_lineitem must be lineitem")
    pred = body["pred_shipdate"]
    groups = body["group_exprs"]
    aggs = body["aggs"]
    project = body["project_exprs"]
    orders = body["order_keys"]
    tree = body["tree"]
    catalog_params = body["catalog_params"]
    catalog_delta = int(body["catalog_delta"])

    # Fail-closed: pred uses param.
    pred_txt = json.dumps(pred)
    if '"kind": "param"' not in pred_txt and '"kind":"param"' not in pred_txt:
        die("pred_shipdate must reference param cutoff")

    # Reconstruct tree from pieces and compare to serialized tree.
    rebuilt = {
        "kind": "sort",
        "input": {
            "kind": "project",
            "input": {
                "kind": "aggregate",
                "input": {
                    "kind": "filter",
                    "input": {"kind": "scan", "table": table},
                    "pred": pred,
                },
                "group_exprs": groups,
                "aggs": aggs,
            },
            "exprs": project,
        },
        "order_keys": orders,
    }
    if rebuilt != tree:
        die("piecewise reconstruction != tree (Rocq serialize inconsistency)")

    pin_obj = {
        "version": body["version"],
        "table_lineitem": table,
        "pred_shipdate": pred,
        "group_exprs": groups,
        "aggs": aggs,
        "project_exprs": project,
        "order_keys": orders,
        "tree": tree,
        "catalog_params": catalog_params,
        "catalog_delta": catalog_delta,
    }
    pin = hashlib.sha256(
        json.dumps(pin_obj, separators=(",", ":"), ensure_ascii=True).encode()
    ).hexdigest()

    pred_dfy = emit_expr(pred)
    groups_dfy = seq_emit([emit_expr(e) for e in groups])
    aggs_dfy = seq_emit([emit_agg(a) for a in aggs])
    project_dfy = seq_emit([emit_expr(e) for e in project])
    orders_dfy = seq_emit([emit_order(k) for k in orders])
    tree_dfy = emit_rel(tree)

    out = ROOT / "tests/verification/q1/dafny/generated/Q1RelNodeData.dfy"
    text = f"""\
// GENERATED from Rocq serialize_LogicalSpec_Q1_rel_export_json — DO NOT EDIT.
// authority: formalsql/q1/Q1LogicalSpecTree.LogicalSpec_Q1_rel
// serialize: formalsql/q1/Q1LogicalSpecSerialize.v
// dump: tests/verification/specs/q1/logical_spec_relnode.dump.json
// source_sha256={pin}
//
// RelNode LogicalSpec DATA only. No evaluator / Adapt.
// Query-specific row/schema/output adapters remain in Q1RelNodeSpec.dfy.

include "../../../logical/dafny/LogicalSpec.dfy"

module Q1RelNodeData {{

  import opened LogicalSpec

  const TableLineitem: TableId := "{table}"

  ghost function Q1PredShipdate(): Pred {{
    {pred_dfy}
  }}

  ghost function Q1GroupExprs(): seq<Expr> {{
    {groups_dfy}
  }}

  ghost function Q1AggsRel(): seq<AggSpec> {{
    {aggs_dfy}
  }}

  ghost function Q1ProjectExprs(): seq<Expr> {{
    {project_dfy}
  }}

  ghost function Q1OrderKeysRel(): seq<OrderKey> {{
    {orders_dfy}
  }}

  ghost function LogicalSpec_Q1_rel(): LogicalSpec {{
    {tree_dfy}
  }}
}}
"""
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text, encoding="utf-8")
    print(f"emit_relnode_logical_spec_dafny OK -> {out}")
    print(f"source_sha256={pin}")


if __name__ == "__main__":
    main()
