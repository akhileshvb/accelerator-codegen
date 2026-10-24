"""A fake engine for the data tools: a host.sh that answers `sql` from a SQLite file, and its profile."""

from __future__ import annotations

import sqlite3
import stat
import sys
from pathlib import Path
from types import SimpleNamespace

from accel_pipeline.config import DataToolsConfig
from accel_pipeline.tools.data_tools import DataToolContext

HOST = """#!/usr/bin/env bash
case "$1" in
  up) mkdir -p "$ACCEL_ENGINE_STATE_DIR" && : >"$ACCEL_ENGINE_STATE_DIR/env" ;;
  load) ;;
  sql) exec "{python}" -I "$(dirname "$0")/responder.py" "$FAKE_ENGINE_DB" ;;
esac
"""
RESPONDER = """import csv, sqlite3, sys
con = sqlite3.connect("file:" + sys.argv[1] + "?mode=ro", uri=True)
try:
    cur = con.execute(sys.stdin.read())
except sqlite3.Error as exc:
    print(exc, file=sys.stderr)
    sys.exit(1)
out = csv.writer(sys.stdout, lineterminator="\\n")
out.writerow([d[0] for d in cur.description])
out.writerows(cur)
"""
PROFILE = """engine_profile:
  data_sql:
    dialect: sqlite
    catalog_columns_sql: >-
      SELECT m.name, p.name, p.type FROM sqlite_master m, pragma_table_info(m.name) p WHERE m.type = 'table'
      ORDER BY m.name, p.cid
"""

QUERY = """
SELECT o.region, SUM(e.amount) AS total
FROM orders o, events e
WHERE o.kind = $1 AND e.order_id = o.id AND o.placed < $2 AND e.happened > $2 AND e.amount > $3
GROUP BY o.region ORDER BY total DESC
"""


def make_dataset(path: Path) -> Path:
    con = sqlite3.connect(path)
    con.execute("CREATE TABLE orders(id INTEGER, kind TEXT, region TEXT, placed INTEGER)")
    con.execute("CREATE TABLE events(order_id INTEGER, happened INTEGER, amount NUMERIC)")
    con.executemany(
        "INSERT INTO orders VALUES (?, ?, ?, ?)",
        [(i, f"k{i % 3}", f"r{i % 2}", i) for i in range(1, 101)],
    )
    con.executemany(
        "INSERT INTO events VALUES (?, ?, ?)",
        [(1 + i % 100, i % 100 + 5 + i % 7, i % 10) for i in range(400)],
    )
    con.commit()
    con.close()
    return path


def make_engine(tmp: Path, monkeypatch, *, up: bool = True, **limits) -> DataToolContext:
    """A tree with a fake `fake` engine whose instance is up (or not) over a SQLite dataset."""
    repo = tmp / "repo"
    host = repo / "backends" / "fake" / "host.sh"
    host.parent.mkdir(parents=True)
    host.write_text(HOST.replace("{python}", sys.executable))
    host.chmod(host.stat().st_mode | stat.S_IXUSR)
    (host.parent / "responder.py").write_text(RESPONDER)
    profile = repo / "pipeline" / "config" / "workloads" / "engine_fake.yaml"
    profile.parent.mkdir(parents=True)
    profile.write_text(PROFILE)
    (repo / "workloads" / "acme").mkdir(parents=True)
    (repo / "workloads" / "acme" / "q1.sql").write_text(QUERY)
    monkeypatch.setenv("FAKE_ENGINE_DB", str(make_dataset(tmp / "acme.sqlite")))
    state = tmp / "state"
    monkeypatch.setenv("ACCEL_ENGINE_STATE_DIR", str(state))
    if up:
        state.mkdir()
        (state / "env").write_text("")
    cfg = SimpleNamespace(repo_root=repo, build_dir=tmp / "build", data_tools=DataToolsConfig(**limits))
    art = tmp / "art"
    art.mkdir()
    return DataToolContext(cfg, {"engine_name": "fake", "benchmark_name": "acme"}, 1, art)
