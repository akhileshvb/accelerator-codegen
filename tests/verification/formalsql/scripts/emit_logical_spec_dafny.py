#!/usr/bin/env python3
"""Emit Dafny LogicalSpec DATA from Rocq-serialized logical_spec.dump.json.

Usage:
  emit_logical_spec_dafny.py q1
Writes:
  tests/verification/q1/dafny/generated/LogicalSpecData.dfy

Fail-closed: unsupported constructors abort. No evaluator code is emitted.
"""

from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]


def die(msg: str) -> None:
    print(f"emit_logical_spec_dafny FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


SCALE = {0: "Scale0", 2: "Scale2", 4: "Scale4", 6: "Scale6"}
COL = {
    "l_shipdate": "ColShipdate",
    "l_quantity": "ColQty",
    "l_extendedprice": "ColPrice",
    "l_discount": "ColDisc",
    "l_tax": "ColTax",
    "l_returnflag": "ColRf",
    "l_linestatus": "ColLs",
}
PARAM = {"cutoff": "ParamCutoff"}


def emit_scale(n: int) -> str:
    if n not in SCALE:
        die(f"unsupported scale {n}")
    return SCALE[n]


def emit_expr(e: dict) -> str:
    kind = e.get("kind")
    if kind == "column":
        name = e.get("name")
        if name not in COL:
            die(f"unsupported column {name!r}")
        return f"EColumn({COL[name]})"
    if kind == "const":
        return f"EConst({int(e['value'])}, {emit_scale(int(e['scale']))})"
    if kind == "param":
        pid = e.get("id")
        if pid not in PARAM:
            die(f"unsupported param {pid!r}")
        return f"EParam({PARAM[pid]}, {emit_scale(int(e['scale']))})"
    if kind == "add":
        return f"EAdd({emit_expr(e['left'])}, {emit_expr(e['right'])})"
    if kind == "sub":
        return f"ESub({emit_expr(e['left'])}, {emit_expr(e['right'])})"
    if kind == "mul":
        return f"EMul({emit_expr(e['left'])}, {emit_expr(e['right'])})"
    die(f"unsupported expr kind {kind!r}")


def emit_pred(p: dict) -> str:
    if p.get("kind") != "le":
        die(f"unsupported pred {p.get('kind')!r}")
    return f"PLe({emit_expr(p['left'])}, {emit_expr(p['right'])})"


def emit_agg(a: dict) -> str:
    kind = a.get("kind")
    if kind == "sum":
        return f"AggSum({emit_expr(a['expr'])})"
    if kind == "avg":
        return f"AggAvg({emit_expr(a['expr'])})"
    if kind == "count_star":
        return "AggCountStar"
    die(f"unsupported agg {kind!r}")


def emit_order(k: dict) -> str:
    d = k.get("dir")
    if d == "asc":
        sd = "SortAsc"
    elif d == "desc":
        sd = "SortDesc"
    else:
        die(f"unsupported sort dir {d!r}")
    return f"OrderKey({emit_expr(k['expr'])}, {sd})"


def main() -> None:
    if len(sys.argv) != 2 or sys.argv[1] != "q1":
        die("usage: emit_logical_spec_dafny.py q1")
    query = sys.argv[1]
    dump_path = ROOT / "tests/verification/specs" / query / "logical_spec.dump.json"
    if not dump_path.is_file():
        die(f"missing {dump_path}")
    wrapped = json.loads(dump_path.read_text(encoding="utf-8"))
    if wrapped.get("authority") != "rocq_serialize_LogicalSpec_Q1_json":
        die("unexpected authority")

    # v2: {template, catalog_params, catalog_delta} or legacy flat logical_spec
    body = wrapped.get("export") or wrapped.get("logical_spec")
    if body is None:
        die("missing export/logical_spec")
    if body.get("version") != "logical_spec_expr_ir_v2":
        die("unexpected version (need logical_spec_expr_ir_v2)")

    if "template" in body:
        ls = body["template"]
        catalog_params = body["catalog_params"]
        catalog_delta = int(body.get("catalog_delta", 90))
    else:
        ls = body
        catalog_params = ls.get("params") or wrapped.get("catalog_params")
        catalog_delta = 90

    if ls.get("well_scaled") is not True:
        die("well_scaled must be true")
    if ls.get("inputs") != ["lineitem"]:
        die("Q1 inputs must be [lineitem]")
    if ls.get("join") is not None:
        die("Q1 join must be null")
    if not catalog_params:
        die("missing catalog_params")

    cutoff = int(catalog_params["cutoff"])
    base_date = int(catalog_params["base_date"])
    filters = ",\n      ".join(emit_pred(p) for p in ls["filters"])
    groups = ",\n      ".join(emit_expr(e) for e in ls["group_keys"])
    aggs = ",\n      ".join(emit_agg(a) for a in ls["aggs"])
    orders = ",\n      ".join(emit_order(k) for k in ls["order_keys"])

    # Fail-closed: filter must use EParam, not baked EConst cutoff.
    filt_txt = json.dumps(ls["filters"])
    if '"kind": "const"' in filt_txt or '"kind":"const"' in filt_txt:
        die("Q1 filter must not bake cutoff as const; use param")
    if "param" not in filt_txt:
        die("Q1 filter must reference param cutoff")

    pin_obj = {"template": ls, "catalog_params": catalog_params, "catalog_delta": catalog_delta}
    pin = hashlib.sha256(
        json.dumps(pin_obj, separators=(",", ":"), ensure_ascii=True).encode()
    ).hexdigest()

    out = ROOT / "tests/verification/q1/dafny/generated/LogicalSpecData.dfy"
    body_dfy = f"""\
// GENERATED from Rocq serialize_LogicalSpec_Q1_export_json — DO NOT EDIT.
// source: formalsql/resultspec/LogicalSpec.v + LogicalSpecSerialize.v
// dump: tests/verification/specs/q1/logical_spec.dump.json
// source_sha256={pin}
//
// Expression-IR LogicalSpec DATA only. No evaluator.
// Canonical template is parameterized (EParam); catalog Params are separate.
// Group/order keys are semantic Expr lists (tuples of Values), not a physical int key.
// Scales: Const/Param/Add/Sub/Mul/Sum/Avg follow Rocq column_scale / expr_scale rules.

module LogicalSpecData {{

  datatype Scale = Scale0 | Scale2 | Scale4 | Scale6

  datatype ColumnId =
    | ColShipdate | ColQty | ColPrice | ColDisc | ColTax | ColRf | ColLs

  datatype ParamId = ParamCutoff

  datatype Expr =
    | EColumn(c: ColumnId)
    | EConst(z: int, s: Scale)
    | EParam(id: ParamId, s: Scale)
    | EAdd(a: Expr, b: Expr)
    | ESub(a: Expr, b: Expr)
    | EMul(a: Expr, b: Expr)

  datatype Pred = PLe(a: Expr, b: Expr)

  datatype Agg =
    | AggSum(e: Expr)
    | AggAvg(e: Expr)
    | AggCountStar

  datatype SortDir = SortAsc | SortDesc

  datatype OrderKey = OrderKey(expr: Expr, dir: SortDir)

  datatype SpecTable = SpecTableLineitem | SpecTableOrders

  datatype SpecJoin = SpecJoinNone

  datatype LogicalSpec = LogicalSpec(
    inputs: seq<SpecTable>,
    join: SpecJoin,
    filters: seq<Pred>,
    group_keys: seq<Expr>,
    aggs: seq<Agg>,
    order_keys: seq<OrderKey>
  )

  datatype Params = Params(cutoff: int)

  const BaseDate: int := {base_date}

  const CatalogDelta: int := {catalog_delta}

  const CatalogCutoff: int := {cutoff}

  const CatalogParams: Params := Params(CatalogCutoff)

  // Canonical parameterized Q1 template (no baked cutoff).
  const LogicalSpec_Q1: LogicalSpec := LogicalSpec(
    [SpecTableLineitem],
    SpecJoinNone,
    [
      {filters}
    ],
    [
      {groups}
    ],
    [
      {aggs}
    ],
    [
      {orders}
    ]
  )

  ghost function ParamsOfCutoff(c: int): Params {{
    Params(c)
  }}

  lemma CatalogParams_delta90()
    ensures CatalogParams.cutoff == BaseDate - CatalogDelta
    ensures CatalogParams == ParamsOfCutoff(CatalogCutoff)
  {{
  }}
}}
"""
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(body_dfy, encoding="utf-8")
    print(f"emit_logical_spec_dafny OK -> {out}")
    print(f"source_sha256={pin}")


if __name__ == "__main__":
    main()
