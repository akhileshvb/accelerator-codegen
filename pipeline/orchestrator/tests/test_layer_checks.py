"""The layout checks: backends/substrate name no query; includes point down the layers."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
QUERY_FREE = REPO / "scripts" / "check_backend_query_free.py"
INCLUDES = REPO / "scripts" / "check_layer_includes.py"

# The per-query switch that used to live in the DuckDB backend (src/runtime/duckdb/tpch_rf.cpp).
OLD_RF_SWITCH = """\
TpchRfTableIds TpchRfTableIdsForQuery(int query_id) {
  // Must match CreateBinding configs in duckdb_runtime.cpp.
  TpchRfTableIds ids;
  switch (query_id) {
  case 1:
  case 6:
    ids.lineitem = 1;
    ids.watches_lineitem = true;
    break;
  case 13: // TpchQ13Config{customer 0, orders 1}; no lineitem
    ids.orders = 1;
    break;
  default:
    break;
  }
  return ids;
}
"""


def _run(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([sys.executable, *args], capture_output=True, text=True, timeout=120)


def test_query_free_flags_switch_if_chain_and_table(tmp_path: Path):
    f = tmp_path / "tpch_rf.cpp"
    f.write_text(OLD_RF_SWITCH, encoding="utf-8")
    r = _run(str(QUERY_FREE), "--file", str(f))
    assert r.returncode == 1 and "switch (query_id) case 1, 6, 13" in r.stdout, r.stdout
    g = tmp_path / "dispatch.cpp"
    g.write_text(
        "void F(const Entry &entry_) {\n"
        "  if (entry_.query_id == 11) { return; }\n"
        "  if (7 == plan.query_number) { return; }\n"
        "}\n"
        "static const std::map<int, int> kQueryTables = {\n"
        "    {1, 2},\n"
        "    {6, 3},\n"
        "};\n",
        encoding="utf-8",
    )
    r = _run(str(QUERY_FREE), "--file", str(g))
    assert r.returncode == 1
    assert "if: query_id == 11" in r.stdout and "if: query_number == 7" in r.stdout
    assert "table keyed by query number (2 rows)" in r.stdout


def test_query_free_ignores_generic_code(tmp_path: Path):
    f = tmp_path / "generic.cpp"
    f.write_text(
        "class ToyModule final : public core::AuxQueryModule {\n"
        "  int f(int query_count) { if (query_count == 2) return 1; switch (kind) { case 1: return 2; } return 0; }\n"
        "  bool g(int query_id) { return query_id == 0; }\n"
        "};\n"
        "// switch (query_id) { case 6: }  (comments are ignored)\n",
        encoding="utf-8",
    )
    r = _run(str(QUERY_FREE), "--file", str(f))
    assert r.returncode == 0, r.stdout


def test_query_free_scans_substrate_and_backends():
    r = _run(str(QUERY_FREE), "--json", "--no-prims-vocab")
    data = json.loads(r.stdout)
    assert data["files_scanned"] > 100
    assert all(h["file"].startswith(("substrate/", "backends/")) for h in data["hits"])


def test_prims_vocab_check_points_at_accelerators_prims():
    r = _run(str(QUERY_FREE), "--prims-only")
    assert r.returncode == 0, r.stdout


def _tree(tmp_path: Path) -> Path:
    root = tmp_path / "repo"
    files = {
        "substrate/aux/core/aux_types.hpp": "#pragma once\n#include <vector>\n",
        "backends/duckdb/api/register.hpp": '#include "substrate/aux/core/aux_types.hpp"\n',
        "accelerators/prims/flat_hash_map.hpp": "#pragma once\n",
        "accelerators/tpch/q6/duckdb/aux/module.hpp": '#include "prims/flat_hash_map.hpp"\n'
                                                      '#include "backends/duckdb/api/register.hpp"\n',
        "eval/harness/tpch_rf.hpp": "#pragma once\n",
    }
    for rel, text in files.items():
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        (root / rel).write_text(text, encoding="utf-8")
    return root


def test_include_direction_clean_tree_passes(tmp_path: Path):
    root = _tree(tmp_path)
    r = _run(str(INCLUDES), "--root", str(root))
    assert r.returncode == 0, r.stdout
    assert "include direction ok (2 files" in r.stdout


def test_include_direction_flags_wrong_way(tmp_path: Path):
    root = _tree(tmp_path)
    (root / "backends/duckdb/api/bad.cpp").write_text(
        '#include "prims/flat_hash_map.hpp"\n#include "tpch/q6/duckdb/aux/module.hpp"\n'
        '#include "eval/harness/tpch_rf.hpp"\n', encoding="utf-8")
    (root / "substrate/aux/core/bad.cpp").write_text(
        '#include "backends/duckdb/api/register.hpp"\n#include "catalog/query_catalog.hpp"\n'
        '#include "tpch/q9/layout.hpp"\n', encoding="utf-8")
    r = _run(str(INCLUDES), "--root", str(root), "--json")
    data = json.loads(r.stdout)
    assert r.returncode == 1
    got = {(h["file"], h["include"]) for h in data["hits"]}
    assert got == {
        ("backends/duckdb/api/bad.cpp", "prims/flat_hash_map.hpp"),
        ("backends/duckdb/api/bad.cpp", "tpch/q6/duckdb/aux/module.hpp"),
        ("substrate/aux/core/bad.cpp", "backends/duckdb/api/register.hpp"),
        ("substrate/aux/core/bad.cpp", "catalog/query_catalog.hpp"),
        ("substrate/aux/core/bad.cpp", "tpch/q9/layout.hpp"),
    }
    assert [w["include"] for w in data["warnings"]] == ["eval/harness/tpch_rf.hpp"]
    assert _run(str(INCLUDES), "--root", str(root), "--strict").returncode == 1


def test_include_direction_scans_real_tree():
    data = json.loads(_run(str(INCLUDES), "--json").stdout)
    assert data["files_scanned"] > 100
