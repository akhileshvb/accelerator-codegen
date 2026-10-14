"""The agents' OS sandbox (bubblewrap): what an agent step can see.

The workspace is the agents' view by convention; this makes it enforced.
Each codex exec step (and the MCP server, codex's child) runs in a mount
namespace that holds only:

    the run's tree (repo root), read-write   -- builds, workspace, run dir
      except substrate/ and workloads/, read-only (no agent writes them)
    the sibling DuckDB checkout, read-only   -- the engine + test databases
    the Verus binary (tools/verus/verus-x86-linux), read-only
    the rust toolchains (~/.rustup), read-only; the crate cache (~/.cargo)
    codex itself (node package), read-only
    the system (/usr, /etc), read-only; fresh /tmp, /run, /proc, /dev
    network (the model API)

Everything else under /home (other checkouts, ~/.codex, ~/.claude, run
artifacts of other pipelines) is an empty tmpfs.  Run the pipeline from a
history-free export of the clean tree: git history inside the root is
visible to the agents like any other file.

No VM and no copying: bind mounts on the same kernel, so build and benchmark
timings are unaffected.  Approvals stay bypassed (codex exec cannot answer
approval prompts and cancels such MCP calls, openai/codex#16685); isolation
comes from here instead of codex's own sandbox.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

from accel_pipeline.verus.context import VerusContext

HOME = Path.home()
# Repo layers mounted read-only inside the sandbox (see accel_pipeline.layout).
READONLY_LAYERS = ("substrate", "workloads")


def _duckdb_root(ctx: VerusContext) -> Path | None:
    for cand in (os.environ.get("ACCEL_DUCKDB_ROOT"), str(ctx.root.parent / "accelerator-codegen-duckdb")):
        if cand and Path(cand).is_dir():
            return Path(cand).resolve()
    return None


def _codex_package() -> list[Path]:
    """The codex launcher and the package it resolves to."""
    out: list[Path] = []
    exe = shutil.which("codex")
    if exe:
        out.append(Path(exe))
        out.append(Path(os.path.realpath(exe)).parent.parent)  # .../@openai/codex
    return out


def mounts(ctx: VerusContext) -> list[tuple[str, Path, Path]]:
    """(mode, source, destination) for every bind beyond the system ones."""
    root = ctx.root.resolve()
    binds: list[tuple[str, Path, Path]] = [("rw", root, root)]
    # Layers no agent may write, re-bound read-only over the tree (later binds win):
    # substrate/** (never writable) and workloads/** (the query SQL and schemas).
    # accelerators/prooflib/ stays writable: prooflib_publish runs in the MCP server,
    # inside this sandbox.
    for layer in ctx.setting("sandbox_readonly_layers", list(READONLY_LAYERS)) or []:
        p = root / str(layer)
        if p.is_dir():
            binds.append(("ro", p, p))
    if (duck := _duckdb_root(ctx)) is not None:
        binds.append(("ro", duck, duck))
    # the Verus binary directory (VERUS_BIN's, else the tree's tools/verus),
    # mounted at its own path so VERUS_BIN means the same inside
    verus_bin = str(ctx.setting("verus_bin") or os.environ.get("VERUS_BIN")
                    or root / "tools" / "verus" / "verus-x86-linux" / "verus")
    verus_dir = Path(os.path.realpath(verus_bin)).parent
    if verus_dir.is_dir() and not verus_dir.is_relative_to(root):
        binds.append(("ro", verus_dir, verus_dir))
    if (HOME / ".rustup").is_dir():
        binds.append(("ro", HOME / ".rustup", HOME / ".rustup"))
    if (HOME / ".cargo").is_dir():  # crate cache (sql2verus builds); cargo writes its locks here
        binds.append(("rw", HOME / ".cargo", HOME / ".cargo"))
    from accel_pipeline.verus.actions import cargo_target_dir  # sql2verus's shared build dir
    target = cargo_target_dir(ctx)
    target.mkdir(parents=True, exist_ok=True)
    if not target.is_relative_to(root):
        binds.append(("rw", target, target))
    for p in _codex_package():
        # the launcher stays a symlink (codex finds its native binary from its real path)
        binds.append(("symlink", Path(os.readlink(p)), p) if p.is_symlink() else ("ro", p, p))
    # databases or a python environment outside the tree
    for f in (ctx.test_db, ctx.bench_db):
        if f and Path(f).exists():
            d = Path(f).resolve().parent
            if not any(d.is_relative_to(src) for _, src, _ in binds):
                binds.append(("ro", d, d))
    for prefix in {Path(sys.prefix).resolve(), Path(sys.base_prefix).resolve()}:
        if prefix.is_relative_to(HOME) and not prefix.is_relative_to(root):
            binds.append(("ro", prefix, prefix))
    for extra in ctx.setting("sandbox_extra_ro", []) or []:
        p = Path(extra).resolve()
        binds.append(("ro", p, p))
    return binds


def prefix(ctx: VerusContext, cwd: Path) -> list[str]:
    """The bwrap command line that runs a command in the sandbox (append the command)."""
    bwrap = shutil.which("bwrap")
    if not bwrap:
        raise RuntimeError("verus.sandbox is bwrap but bubblewrap is not installed (or set verus.sandbox: none)")
    args = [bwrap, "--unshare-all", "--share-net", "--die-with-parent",
            "--ro-bind", "/usr", "/usr", "--ro-bind", "/etc", "/etc"]
    for link in ("bin", "lib", "lib64", "sbin"):
        p = Path("/") / link
        if p.is_symlink():
            args += ["--symlink", os.readlink(p), str(p)]
        elif p.is_dir():
            args += ["--ro-bind", str(p), str(p)]
    if Path("/opt").is_dir():
        args += ["--ro-bind", "/opt", "/opt"]
    args += ["--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp", "--tmpfs", "/run"]
    # /etc/resolv.conf may point into /run (systemd-resolved)
    resolv = Path(os.path.realpath("/etc/resolv.conf"))
    if resolv.is_relative_to("/run") and resolv.parent.is_dir():
        args += ["--ro-bind", str(resolv.parent), str(resolv.parent)]
    args += ["--tmpfs", "/home"]
    if not HOME.is_relative_to("/home"):
        args += ["--tmpfs", str(HOME)]
    args += ["--dir", str(HOME)]
    for mode, src, dst in mounts(ctx):
        if mode == "symlink":
            args += ["--symlink", str(src), str(dst)]
        else:
            args += ["--bind" if mode == "rw" else "--ro-bind", str(src), str(dst)]
    args += ["--chdir", str(cwd)]
    return args


def selfcheck(ctx: VerusContext) -> dict[str, Any]:
    """Probe the sandbox: the tree and tools are there; of $HOME and the
    root's parent, only the mounted paths are (their parents appear as empty
    skeleton directories holding just the way to the mount)."""
    allowed = [Path(os.path.abspath(dst)) for _, _, dst in mounts(ctx)]
    cands = sorted({p.resolve() for p in list(HOME.iterdir()) + list(ctx.root.resolve().parent.iterdir())})
    lines = []
    for p in cands:
        if any(p == a or p.is_relative_to(a) for a in allowed):
            continue
        under = sorted({a.relative_to(p).parts[0] for a in allowed if a.is_relative_to(p)})
        if not under:
            lines.append(f'[ -e "{p}" ] && echo "VISIBLE {p}"')
        else:
            keep = "|".join(under)
            lines.append(f'for e in $(ls -A "{p}" 2>/dev/null); do case "$e" in {keep}) ;; *) echo "VISIBLE {p}/$e";; esac; done')
    verus = str(ctx.setting("verus_bin") or os.environ.get("VERUS_BIN")
                or f"{ctx.root}/tools/verus/verus-x86-linux/verus")
    lines += [f'[ -e "{ctx.root}/pipeline" ] || echo "MISSING {ctx.root}"',
              'command -v codex >/dev/null || echo "MISSING codex"',
              f'[ -x "{verus}" ] || echo "MISSING verus ({verus})"',
              "true"]
    proc = subprocess.run(prefix(ctx, ctx.root) + ["bash", "-c", "\n".join(lines)], capture_output=True,
                          text=True, timeout=60)
    out = [l for l in proc.stdout.splitlines() if l.strip()]
    visible = [l[len("VISIBLE "):] for l in out if l.startswith("VISIBLE ")]
    missing = [l[len("MISSING "):] for l in out if l.startswith("MISSING ")]
    return {"ok": proc.returncode == 0 and not visible and not missing, "probed": len(cands),
            "visible": visible, "missing": missing, "stderr": proc.stderr[-2000:]}
