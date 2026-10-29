#!/usr/bin/env python3
"""Regenerate the sql2verus-generated files of a benchmark's Verus kits: gen_world.rs, gen_ffi.rs
and the C header.  gen_spec.rs and gen_obligations.rs are only compared (the spec is frozen):
a difference is an error and nothing is written for that kit.  A kit without them yet (a new
kit) gets them written, with a kernel stub (kernel.rs) if it has no kernel, a minimal lib.rs, and the derived host glue (glue.cpp) if it has none.

    python3 scripts/regen_verus_kits.py [--engine E] [--bench tpch] [--update q11,q21] <sql2verus binary> <schema.sql> [<inputs dir>] [q1 q2 ...]

--bench: the benchmark (default tpch): kits accelerators/<bench>/q<N>/verus_kit/<bench>_q<N>_vgen,


--engine: the engine whose spec is generated (default: the pipeline's, verus.yaml).  The default
engine's files live in crate/src; another engine's spec is crate/src/<engine>/gen_spec.rs, selected
by crate/src/lib_<engine>.rs (lib.rs with that one module re-pointed; build_verus_crate.sh takes it
as VERUS_CRATE_ROOT).  Everything else sql2verus generates must come out the same for every engine
(obligations, world, ffi, header, glue); a difference is an error.

--update: these kits' gen_spec.rs and gen_obligations.rs are rewritten too (a spec change approved
on purpose: the kernel then has to be re-proved).  A query with workloads/<bench>/<q>.nullable-params
(parameter numbers, comma separated) gets sql2verus --nullable-params.

<inputs dir> (default workloads/<bench>) holds the kernel inputs of each query as q<N>.inputs (the files the kits'
gen_obligations.rs were generated from).  Host glue (glue.cpp) is written per kit, not
generated.
"""
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent


def same(generated: pathlib.Path, committed: pathlib.Path) -> bool:
    return generated.read_text() == committed.read_text()


# the engine the kits run on, as the Verus pipeline's settings name it
DEFAULT_ENGINE = re.search(r"^engine:\s*(\S+)", (ROOT / "pipeline/config/verus.yaml").read_text(), re.M).group(1)
FFI_PREFIX = {"tpch": "tpch", "sec_edgar": "sec"}  # other benchmarks: the name without underscores
UPDATE_SPEC = bool(os.environ.get("REGEN_UPDATE_SPEC"))  # approved spec change only


def main() -> int:
    argv = sys.argv[1:]
    bench, engine = "tpch", DEFAULT_ENGINE
    while argv[:1] in (["--bench"], ["--engine"]):
        if argv[0] == "--bench":
            bench = argv[1]
        else:
            engine = argv[1]
        argv = argv[2:]
    own = engine != DEFAULT_ENGINE  # spec files under crate/src/<engine>/
    update: set[str] = set()
    if argv[:1] == ["--update"]:
        update = set(argv[1].split(","))
        argv = argv[2:]
    binary, schema = argv[0], pathlib.Path(argv[1])
    inputs = ROOT / "workloads" / bench
    if argv[2:3] and not re.fullmatch(r"q\d+", argv[2]):
        inputs = pathlib.Path(argv[2])
        argv = argv[:2] + argv[3:]
    qs = argv[2:] or [f"q{n}" for n in (range(1, 23) if bench == "tpch" else (1, 2, 3, 4, 6, 24))]
    bad = 0
    for q in qs:
        n = q[1:]
        name = f"{bench}_{q}_vgen"
        kit = ROOT / "accelerators" / bench / q / "verus_kit" / name
        src = kit / "crate" / "src"
        with tempfile.TemporaryDirectory(dir="/dev/shm") as t:
            t = pathlib.Path(t)
            has_inputs = (inputs / f"{q}.inputs").exists()  # none: every table whole
            if has_inputs:
                shutil.copy2(inputs / f"{q}.inputs", t / "q.inputs")  # the name gen_obligations.rs records
            cmd = [binary, "--engine", engine, "--schema", schema,
                   "--query", ROOT / f"workloads/{bench}/{q}.sql", "--out", t / "gen_spec.rs",
                   "--obligations", t / "gen_obligations.rs",
                   "--kernel-stub", t / "kernel_stub.rs", "--world", t / "gen_world.rs",
                   "--ffi", t / "gen_ffi.rs", "--ffi-header", t / f"{name}_verus.h",
                   "--ffi-prefix", f"{FFI_PREFIX.get(bench, bench.replace('_', ''))}q{n}vgen"]
            if has_inputs:
                cmd += ["--inputs", t / "q.inputs"]
            if not (kit / "glue.cpp").exists():  # a new kit: the derived host glue as its start
                cmd += ["--host-glue", t / "glue.cpp", "--query-id", n, "--accel-id", name, "--workload", bench]
            nullable = ROOT / f"workloads/{bench}/{q}.nullable-params"
            if nullable.exists():
                cmd += ["--nullable-params", nullable.read_text().strip()]
            r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
            if r.returncode != 0:
                print(f"{q}: sql2verus failed\n{r.stderr[-1500:]}")
                bad += 1
                continue
            if own:
                shared = {"gen_obligations.rs": src / "gen_obligations.rs", "gen_world.rs": src / "gen_world.rs",
                          "gen_ffi.rs": src / "gen_ffi.rs", f"{name}_verus.h": kit / f"{name}_verus.h"}
                off = [f for f, c in shared.items() if not c.exists() or not same(t / f, c)]
                if off:
                    print(f"{q}: {', '.join(off)} differ between engines {DEFAULT_ENGINE} and {engine}; not written")
                    bad += 1
                    continue
                spec = src / engine / "gen_spec.rs"
                spec.parent.mkdir(exist_ok=True)
                if (q in update or UPDATE_SPEC or not spec.exists()):
                    shutil.copy2(t / "gen_spec.rs", spec)
                elif not same(t / "gen_spec.rs", spec):
                    print(f"{q}: {engine}/gen_spec.rs differs from the committed one; not written")
                    bad += 1
                    continue
                lib = src / f"lib_{engine}.rs"
                lib.write_text((src / "lib.rs").read_text().replace(
                    "pub mod gen_spec;", f'#[path = "{engine}/gen_spec.rs"]\npub mod gen_spec;'))
                print(f"{q}: ok ({engine})")
                continue
            src.mkdir(parents=True, exist_ok=True)
            fresh = [f for f in ("gen_spec.rs", "gen_obligations.rs") if not (src / f).exists()]
            for f in fresh:  # a new kit: its first spec
                shutil.copy2(t / f, src / f)
            if not (src / "lib.rs").exists():
                mods = ("gen_spec", "gen_obligations", "kernel", "gen_world", "gen_ffi")
                (src / "lib.rs").write_text("#![allow(unused_imports)]\n\n" + "".join(f"pub mod {m};\n" for m in mods))
            if fresh and not (src / "kernel.rs").exists():
                shutil.copy2(t / "kernel_stub.rs", src / "kernel.rs")
            frozen = [f for f in ("gen_spec.rs", "gen_obligations.rs") if not same(t / f, src / f)]
            if frozen and (q in update or UPDATE_SPEC):  # approved spec change: write the regenerated spec
                for f in frozen:
                    shutil.copy2(t / f, src / f)
                frozen = []
            if frozen:
                print(f"{q}: {', '.join(frozen)} differ from the committed ones; not written")
                bad += 1
                continue
            if (t / "glue.cpp").exists():
                shutil.copy2(t / "glue.cpp", kit / "glue.cpp")
            shutil.copy2(t / "gen_world.rs", src / "gen_world.rs")
            shutil.copy2(t / "gen_ffi.rs", src / "gen_ffi.rs")
            shutil.copy2(t / f"{name}_verus.h", kit / f"{name}_verus.h")
            print(f"{q}: ok")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
