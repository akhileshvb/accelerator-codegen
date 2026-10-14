#!/usr/bin/env python3
"""Measure on-disk GenDB TPC-H SF20 storage after a real ingest+index run.

Walks output/tpc-h-sf20/storage (produced by ingest/run_sf20_ingest.sh) and
records st_size for every column/index file. used_by_aux_11 marks files
mmapped by TPC-H Q1,3,6,9,10,11,12,14,17,18,21 in the published best.cpp
artifacts.
"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
RESULTS = Path(__file__).resolve().parent / "results"
DEFAULT_STORAGE = ROOT.parent / "GenDB" / "output" / "tpc-h-sf20" / "storage"
OUT_CSV = RESULTS / "gendb_tpch_sf20_memory.csv"

# Logical names opened by the 11 aux-query best.cpp artifacts (union).
# Varlen columns cover both *_offsets.bin and *_data.bin.
AUX_11 = frozenset(
    {
        "lineitem.l_orderkey",
        "lineitem.l_partkey",
        "lineitem.l_suppkey",
        "lineitem.l_quantity",
        "lineitem.l_extendedprice",
        "lineitem.l_discount",
        "lineitem.l_tax",
        "lineitem.l_returnflag",
        "lineitem.l_linestatus",
        "lineitem.l_shipdate",
        "lineitem.l_commitdate",
        "lineitem.l_receiptdate",
        "lineitem.l_shipmode",
        "lineitem.l_shipmode_dict",
        "orders.o_orderkey",
        "orders.o_custkey",
        "orders.o_orderdate",
        "orders.o_shippriority",
        "orders.o_totalprice",
        "orders.o_orderstatus",
        "orders.o_orderpriority",
        "orders.o_orderpriority_dict",
        "customer.c_custkey",
        "customer.c_mktsegment",
        "customer.c_mktsegment_dict",
        "customer.c_nationkey",
        "customer.c_acctbal",
        "customer.c_name",
        "customer.c_address",
        "customer.c_phone",
        "customer.c_comment",
        "part.p_partkey",
        "part.p_name",
        "part.p_brand",
        "part.p_brand_dict",
        "part.p_container",
        "part.p_container_dict",
        "part.p_type",
        "part.p_type_dict",
        "supplier.s_suppkey",
        "supplier.s_nationkey",
        "supplier.s_name",
        "partsupp.ps_partkey",
        "partsupp.ps_suppkey",
        "partsupp.ps_supplycost",
        "partsupp.ps_availqty",
        "nation.n_nationkey",
        "nation.n_name",
        "index.lineitem_l_shipdate_zonemap",
        "index.lineitem_l_receiptdate_zonemap",
        "index.lineitem_l_orderkey_grouped",
        "index.orders_o_orderkey_lookup",
        "index.orders_o_orderdate_zonemap",
        "index.partsupp_pk_hash",
        "index.part_p_partkey_lookup",
        "index.customer_c_custkey_lookup",
    }
)


def logical_name(table: str, filename: str) -> str:
    stem = filename[:-4] if filename.endswith(".bin") else filename
    if table == "indexes":
        return f"index.{stem}"
    if stem.endswith("_offsets"):
        return f"{table}.{stem[: -len('_offsets')]}"
    if stem.endswith("_data"):
        return f"{table}.{stem[: -len('_data')]}"
    return f"{table}.{stem}"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--storage", type=Path, default=DEFAULT_STORAGE)
    args = parser.parse_args()
    storage: Path = args.storage
    if not storage.is_dir():
        raise SystemExit(f"missing GenDB storage dir (run ingest first): {storage}")

    rows: list[dict[str, str | int]] = []
    found_aux: set[str] = set()
    for path in sorted(storage.rglob("*.bin")):
        rel = path.relative_to(storage)
        table = rel.parts[0]
        name = logical_name(table, rel.name)
        kind = "index" if table == "indexes" else "base"
        used = "1" if name in AUX_11 else "0"
        if used == "1":
            found_aux.add(name)
        rows.append(
            {
                "section": "storage",
                "name": name,
                "file": str(rel),
                "kind": kind,
                "bytes": path.stat().st_size,
                "used_by_aux_11": used,
                "notes": "stat() after ingest+index build",
            }
        )

    missing = AUX_11 - found_aux
    if missing:
        raise SystemExit(f"storage missing aux-11 files: {sorted(missing)}")

    aux_bytes = sum(int(row["bytes"]) for row in rows if row["used_by_aux_11"] == "1")
    all_bytes = sum(int(row["bytes"]) for row in rows)
    base_aux = sum(
        int(row["bytes"])
        for row in rows
        if row["used_by_aux_11"] == "1" and row["kind"] == "base"
    )
    index_aux = sum(
        int(row["bytes"])
        for row in rows
        if row["used_by_aux_11"] == "1" and row["kind"] == "index"
    )
    rows.append(
        {
            "section": "summary",
            "name": "aux_11_total",
            "file": "",
            "kind": "total",
            "bytes": aux_bytes,
            "used_by_aux_11": "1",
            "notes": (
                f"measured files; base={base_aux} index={index_aux}; "
                f"{aux_bytes / (1024 ** 3):.4f} GiB"
            ),
        }
    )
    rows.append(
        {
            "section": "summary",
            "name": "all_columns_and_indexes",
            "file": "",
            "kind": "total",
            "bytes": all_bytes,
            "used_by_aux_11": "0",
            "notes": f"measured files; {all_bytes / (1024 ** 3):.4f} GiB full storage",
        }
    )

    RESULTS.mkdir(parents=True, exist_ok=True)
    fields = ["section", "name", "file", "kind", "bytes", "used_by_aux_11", "notes"]
    with OUT_CSV.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)

    print(f"wrote {OUT_CSV}")
    print(f"storage:              {storage}")
    print(f"aux 11-query encoded: {aux_bytes / (1024 ** 3):.4f} GiB")
    print(f"  base columns:       {base_aux / (1024 ** 3):.4f} GiB")
    print(f"  indexes:            {index_aux / (1024 ** 3):.4f} GiB")
    print(f"full storage:         {all_bytes / (1024 ** 3):.4f} GiB")


if __name__ == "__main__":
    main()
