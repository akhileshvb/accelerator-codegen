"""The engine hosts' own behaviour, run as the gates run them (backends/<engine>/host.sh)."""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]


def test_the_duckdb_build_configures_again_only_when_the_duckdb_it_was_configured_against_changes(tmp_path):
    host = tmp_path / "repo" / "backends" / "duckdb" / "host.sh"
    host.parent.mkdir(parents=True)
    shutil.copy(REPO / "backends" / "duckdb" / "host.sh", host)
    bin_dir, build, log = tmp_path / "bin", tmp_path / "build", tmp_path / "cmake.log"
    bin_dir.mkdir()
    (bin_dir / "cmake").write_text(  # configures (writes the generator file) or builds, and logs which
        f'#!/bin/sh\nif [ "$1" = --build ]; then echo build >>{log}; else echo "configure $ACCEL_DUCKDB_ROOT" >>{log}; '
        f'touch {build}/CMakeCache.txt {build}/build.ninja; fi\n')
    (bin_dir / "cmake").chmod(0o755)
    build.mkdir()

    def build_with(root: str) -> None:
        env = {**os.environ, "PATH": f"{bin_dir}:{os.environ['PATH']}", "ACCEL_ENGINE_STATE_DIR": str(tmp_path / "state"),
               "ACCEL_BUILD_DIR": str(build), "ACCEL_DUCKDB_ROOT": root}
        subprocess.run(["bash", str(host), "build", "t"], env=env, check=True)

    build_with("/fork/a")
    build_with("/fork/a")
    build_with("/fork/b")
    assert log.read_text().split("\n")[:-1] == ["configure /fork/a", "build", "build", "configure /fork/b", "build"]
