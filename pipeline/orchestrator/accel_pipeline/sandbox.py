"""bubblewrap sandbox shared by the C++ and Verus pipelines.

``prefix(binds, cwd)`` is the mode-agnostic part: the system mounts, fresh /tmp /run /home,
codex and the binds a mode asks for.  A bind is ``(mode, src, dst)``, mode ``rw`` | ``ro`` |
``symlink``; later binds win, so a read-only layer re-bound over a read-write tree stays
read-only.  The Verus mounts live in ``verus.sandbox``; the C++ ones (``cpp_mounts``) are here.

The sandbox cuts what an agent can write (substrate/, workloads/) and see; ownership between
queries and backends is checked after the stage (``write_ownership.check_stage``).
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import uuid
from pathlib import Path
from typing import Any

from accel_pipeline import layout

HOME = Path.home()
# Repo layers mounted read-only inside the sandbox (see accel_pipeline.layout).
READONLY_LAYERS = ("substrate", "workloads")
DOCKER_SOCKET = Path("/run/docker.sock")

Bind = tuple[str, Path, Path]


def codex_package() -> list[Path]:
    """The codex launcher and the package it resolves to."""
    out: list[Path] = []
    exe = shutil.which("codex")
    if exe:
        out.append(Path(exe))
        out.append(Path(os.path.realpath(exe)).parent.parent)  # .../@openai/codex
    return out


def codex_binds() -> list[Bind]:
    # the launcher stays a symlink (codex finds its native binary from its real path)
    return [("symlink", Path(os.readlink(p)), p) if p.is_symlink() else ("ro", p, p) for p in codex_package()]


def readonly_layer_binds(root: Path, layers: Any) -> list[Bind]:
    """Layers no agent may write, re-bound read-only over the tree."""
    return [("ro", p, p) for p in (root / str(layer) for layer in layers or []) if p.is_dir()]


def python_prefix_binds(root: Path) -> list[Bind]:
    """The orchestrator's python environment when it lives under $HOME outside the tree."""
    return [("ro", p, p) for p in {Path(sys.prefix).resolve(), Path(sys.base_prefix).resolve()}
            if p.is_relative_to(HOME) and not p.is_relative_to(root)]


def extra_binds(mode: str, paths: Any) -> list[Bind]:
    return [(mode, Path(x).resolve(), Path(x).resolve()) for x in paths or []]


def prefix(binds: list[Bind], cwd: Path, *, setting: str = "sandbox") -> list[str]:
    """The bwrap command line that runs a command in the sandbox (append the command)."""
    bwrap = shutil.which("bwrap")
    if not bwrap:
        raise RuntimeError(f"{setting} is bwrap but bubblewrap is not installed (or set {setting}: none)")
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
    for mode, src, dst in binds:
        if mode == "symlink":
            args += ["--symlink", str(src), str(dst)]
        elif mode == "tmpfs":
            args += ["--tmpfs", str(dst)]
        else:
            args += ["--bind" if mode == "rw" else "--ro-bind", str(src), str(dst)]
    args += ["--chdir", str(cwd)]
    return args


# ---------------------------------------------------------------- C++ mode


def _covered(path: Path, binds: list[Bind]) -> bool:
    return any(path.is_relative_to(dst) for _, _, dst in binds)


def _add(binds: list[Bind], mode: str, path: Path | str | None) -> None:
    """Bind an existing path at its real location unless a bind already holds it."""
    if not path or not Path(path).exists():
        return
    real = Path(path).resolve()
    if not _covered(real, binds):
        binds.append((mode, real, real))


def _add_dataset(binds: list[Bind], path: Path) -> None:
    """A dataset is never writable: its directory read-only, or, where a writable bind already holds it
    (a dataset inside the tree or the build dir), the dataset itself re-bound read-only over that."""
    if any(m == "ro" and path.is_relative_to(dst) for m, _, dst in binds):
        return
    if _covered(path, [b for b in binds if b[0] == "rw"]):
        binds.append(("ro", path, path))
    else:
        _add(binds, "ro", path if path.is_dir() else path.parent)


def backend_needs(cfg: Any) -> dict[str, Any] | None:
    """What the engine's backend says its verbs need (host.sh `info`: mounts, docker); None without a host.sh."""
    from accel_pipeline.engine_host import host_info

    if not cfg.engine_name:
        return None
    return host_info(Path(cfg.repo_root), cfg.engine_name, Path(cfg.build_dir))


def _primary(cfg: Any) -> Path:
    return Path(os.environ.get("ACCEL_PIPELINE_PRIMARY_REPO") or cfg.repo_root).resolve()


def cpp_mounts(cfg: Any) -> list[Bind]:
    """(mode, source, destination) for every bind of a C++-mode agent step beyond the system ones.

    The run's tree is read-write with substrate/ and workloads/ read-only over it; a pack worker's
    worktree sits under the primary checkout, which the MCP server also writes (primitive_publish,
    shared artifacts), so both are mounted, and the other workers' worktrees are hidden.
    """
    root = Path(cfg.repo_root).resolve()
    primary = _primary(cfg)
    layers = list(READONLY_LAYERS)
    binds: list[Bind] = []
    if primary != root:
        binds.append(("rw", primary, primary))
        binds += readonly_layer_binds(primary, layers)
        worktrees = primary / "pipeline" / "worktrees"
        if root.is_relative_to(worktrees):
            binds.append(("tmpfs", worktrees, worktrees))
    binds.append(("rw", root, root))
    binds += readonly_layer_binds(root, layers)
    for p in (cfg.build_dir, cfg.codex_home, os.environ.get("ACCEL_ENGINE_STATE_DIR")):
        if p:
            Path(p).resolve().mkdir(parents=True, exist_ok=True)  # resolve: a build-* symlink may dangle
            _add(binds, "rw", p)
    needs = backend_needs(cfg) or {}
    for mode in ("ro", "rw"):
        for d in (needs.get("mounts") or {}).get(mode) or []:
            _add(binds, mode, d)
    for f in [*cfg.datasets.values(), os.environ.get("ACCEL_DATASET_DIR")]:
        if f and Path(f).exists():
            _add_dataset(binds, Path(f).resolve())
    if needs.get("docker"):
        _add(binds, "rw", DOCKER_SOCKET)
        if DOCKER_SOCKET.exists() and Path("/var/run").is_symlink():  # the docker CLI's default path
            binds.append(("symlink", Path("/run"), Path("/var/run")))
    if (HOME / ".cache" / "ccache").is_dir():
        _add(binds, "rw", HOME / ".cache" / "ccache")
    _add(binds, "rw", os.environ.get("CCACHE_DIR"))
    binds += codex_binds()
    binds += [b for b in python_prefix_binds(root) if not _covered(b[2], binds)]
    binds += extra_binds("ro", getattr(cfg, "sandbox_extra_ro", []))
    binds += extra_binds("rw", getattr(cfg, "sandbox_extra_rw", []))
    return binds


def cpp_prefix(cfg: Any) -> list[str]:
    return prefix(cpp_mounts(cfg), Path(cfg.repo_root))


def cpp_selfcheck(cfg: Any, query: int) -> dict[str, Any]:
    """Run the sandbox: the tree and tools are there, substrate/ is read-only, the run's own
    directory and the build dir are writable, the engine's host script is reachable."""
    root = Path(cfg.repo_root)
    if cfg.workload_kind == "engine_backend":
        own = root / "backends" / cfg.engine_name
    else:
        own = root / layout.query_dir(cfg.benchmark_name, query)
    while not own.exists() and own != root:
        own = own.parent
    tag = f".sandbox_probe_{uuid.uuid4().hex[:8]}"
    frozen = f"{root}/{layout.SUBSTRATE}/{tag}"
    build = Path(cfg.build_dir)
    build.mkdir(parents=True, exist_ok=True)
    lines = [f'[ -e "{root}/pipeline" ] || echo "MISSING tree {root}"',
             'command -v codex >/dev/null || echo "MISSING codex"',
             f'if touch "{frozen}" 2>/dev/null; then rm -f "{frozen}"; echo "WRITABLE {layout.SUBSTRATE}"; fi',
             f'touch "{own}/{tag}" 2>/dev/null && rm -f "{own}/{tag}" || echo "READONLY {own}"',
             f'touch "{build}/{tag}" 2>/dev/null && rm -f "{build}/{tag}" || echo "READONLY build dir {build}"']
    needs = backend_needs(cfg)
    notes = []
    if needs is None and cfg.engine_name:  # no host.sh yet (early engine_backend layers): nothing extra mounted
        notes.append(f"backend {cfg.engine_name} has no usable host.sh info: no backend mounts or docker socket")
    elif needs is not None:
        lines.append(f'[ -x "{root}/backends/{cfg.engine_name}/host.sh" ] || echo "MISSING engine host script"')
        for mode in ("ro", "rw"):
            lines += [f'[ -e "{d}" ] || echo "MISSING backend mount {d}"' for d in (needs.get("mounts") or {}).get(mode) or []]
        if needs.get("docker"):
            lines.append(f'[ -S "{DOCKER_SOCKET}" ] || echo "MISSING docker socket {DOCKER_SOCKET}"')
    lines.append("true")
    proc = subprocess.run(cpp_prefix(cfg) + ["bash", "-c", "\n".join(lines)], capture_output=True, text=True,
                          timeout=60)
    problems = [l for l in proc.stdout.splitlines() if l.strip()]
    if proc.returncode != 0:
        problems.append(f"sandbox shell failed (exit {proc.returncode}): {proc.stderr[-500:]}")
    return {"ok": not problems, "problems": problems, "notes": notes}
