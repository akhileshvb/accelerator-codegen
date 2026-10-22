#!/usr/bin/env python3
"""Leakage gate: KEEP files (manifest.yaml) must not depend on GENERATED material.

Scans every KEEP text file for references to generated material and reports them by kept file
and by kind:

    path     a generated file or directory: backends/<engine>/..., accelerators/<bench>/..., a primitives
             header, a generated eval or script file
    include  a C++ #include that resolves to a generated header
    cmake    a CMake option or interface target named after a concrete engine
    target   a build target defined only by generated code (or by CMake over generated sources)
    symbol   a class, function, macro or namespace that only generated headers define

and, separately, engine-specific code in kept files (the pipelines must be engine-generic; engine facts
belong in pipeline/config/workloads/engine_*.yaml, engine code in a generated backend):

    engine         a concrete engine name or engine-only identifier (duckdb, postgres, libpq, pg_*, mysqld, ...)
    engine_branch  code that branches on the engine (`engine == "postgres"`, `case "mysql":`)

The generated set is indexed from a checkout in which it exists (--generated-from, default --repo),
so the gate is meaningful in the full tree, not in the clean tree itself. Accepted references are
listed with a reason in leakage_allowlist.yaml.

    check_leakage.py [--repo R] [--generated-from G] [--markdown FILE] [--json] [--baseline FILE | --update-baseline FILE]

With --baseline the exit status is 1 when a (file, kind) count rose above the baseline: leakage may
shrink as the pipelines are decoupled, never grow.
"""
from __future__ import annotations

import argparse
import fnmatch
import json
import re
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))
from manifest import GENERATED, KEEP, Manifest, tracked_files  # noqa: E402

TEXT_SUFFIXES = {".py", ".sh", ".md", ".yaml", ".yml", ".json", ".toml", ".cmake", ".txt", ".cpp", ".hpp", ".h",
                 ".tpl", ".awk", ".sql", ".supp", ".rs", ".inputs"}
CPP_HEADERS = {".hpp", ".h"}
KINDS = ("path", "include", "cmake", "target", "symbol")
ENGINE_KINDS = ("engine", "engine_branch")
ENGINE_PROFILES = "pipeline/config/workloads/engine_*.yaml"
ENGINE_ALIASES = {
    "postgres": [r"postgresql", r"libpq", r"psql", r"pgdata", r"pg", r"pg_\w+", r"\w+_pg", r"pgxs", r"pg_ctl"],
    "mysql": [r"mysqld", r"mariadb", r"mysql_\w+", r"\w+_mysql"],
    "duckdb": [r"duckdb_\w+", r"\w+_duckdb"],
}
GROUPS = {
    "a": "prompts, schemas, templates and configs naming generated symbols or paths",
    "b": "gates and orchestrator hardcoding generated names",
    "c": "scaffolding CMake and eval",
    "d": "scripts",
    "e": "other (tests, docs, workloads)",
}

INCLUDE_RE = re.compile(r'#\s*include\s*[<"]([^>"]+)[>"]')
TOKEN_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
CMAKE_CALL_RE = re.compile(
    r"\b(add_executable|add_library|accel_add_duckdb_exe|accel_add_parallel_tpch_exe|"
    r"accel_add_shared_interface_test|accel_add_backend_exe)\s*\(([^)]*)\)", re.S)
COMMENT_RE = re.compile(r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\\n])*\"", re.S)
TYPE_DEFINITION_RES = (  # forward declarations (`struct X;`) name somebody else's type: not matched
    re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)", re.M),
    re.compile(r"\b(?:class|struct|union|enum(?:\s+class)?|namespace)\s+([A-Za-z_]\w*)\s*(?:final\s*)?[:{]"),
    re.compile(r"\busing\s+([A-Za-z_]\w*)\s*="),
)
FUNCTION_RE = re.compile(r"^[ \t]*(?:[\w:<>,*&]+[ \t]+)+[*&]*([A-Za-z_]\w*)[ \t]*\(", re.M)
NOT_SYMBOLS = {"return", "throw", "delete", "new", "else", "case", "sizeof", "typedef", "operator", "static_assert",
               "defined", "include", "define", "ifdef", "ifndef", "endif", "pragma"}


@dataclass(frozen=True)
class Finding:
    file: str
    line: int
    kind: str
    token: str
    text: str


def group_of(path: str) -> str:
    if path.startswith("pipeline/orchestrator/tests/") or path.startswith(("docs/", "workloads/")):
        return "e"
    if path.startswith(("pipeline/prompts/", "pipeline/schemas/", "pipeline/templates/", "pipeline/catalog/",
                        "pipeline/config/")):
        return "a"
    if path.startswith(("pipeline/gates/", "pipeline/orchestrator/", "pipeline/codex/")):
        return "b"
    if path == "CMakeLists.txt" or path.startswith(("cmake/", "eval/")):
        return "c"
    if path.startswith("scripts/"):
        return "d"
    return "e"


def _is_text(path: str) -> bool:
    return Path(path).suffix in TEXT_SUFFIXES or Path(path).name in {"CMakeLists.txt", ".gitignore", ".env.example"}


def _read(root: Path, rel: str) -> str:
    try:
        return (root / rel).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def _template_regex(name: str) -> str:
    """A CMake target name with ${var} parts as a regex."""
    return r"\b" + re.sub(r"\\\$\\\{[^}]*\\\}", r"\\d+", re.escape(name)) + r"\b"


class GeneratedIndex:
    """What the generated set defines: paths, engines, benches, targets, symbols."""

    def __init__(self, root: Path, manifest: Manifest, skeleton: set[str]):
        self.skeleton = skeleton
        files = tracked_files(root)
        self.paths = {f for f in files if manifest.classify(f) == GENERATED and f not in skeleton}
        self.keep = [f for f in files if manifest.classify(f) == KEEP]
        self.engines = sorted({p.split("/")[1] for p in self.paths if p.startswith("backends/")})
        self.benches = sorted({p.split("/")[1] for p in self.paths
                               if p.startswith("accelerators/") and p.split("/")[1] not in {"prims", "prooflib"}
                               and p.count("/") > 1})
        self.engine_re, self.branch_re = self._engine_regexes()
        self.path_re = self._path_regex()
        self.basenames = self._basenames()
        self.targets = self._targets(root)
        self.symbols = self._symbols(root)

    def _engine_regexes(self) -> tuple[re.Pattern[str] | None, re.Pattern[str] | None]:
        names = sorted(set(self.engines) | set(ENGINE_ALIASES), key=len, reverse=True)
        names += [a for e in self.engines for a in ENGINE_ALIASES.get(e, [])]
        names += [a for e in ENGINE_ALIASES if e not in self.engines for a in ENGINE_ALIASES[e]]
        alt = "|".join(names)
        engine = re.compile(r"(?i)(?<![A-Za-z0-9])(?:%s)(?![A-Za-z0-9])" % alt)
        engines = "|".join(sorted(set(self.engines) | set(ENGINE_ALIASES)))
        branch = re.compile(r"(?i)\bengine\w*\s*(?:==|!=|\bin\b|\bis\b)\s*[\"'(\[{]?\s*[\"']?(?:%s)\b|"
                            r"\b(?:if|elif|case)\b[^\n]*[\"'](?:%s)[\"']" % (engines, engines))
        return engine, branch

    def _path_regex(self) -> re.Pattern[str] | None:
        alts = [r"backends/(?:%s)\b[\w./-]*" % "|".join(self.engines)] if self.engines else []
        if self.benches:
            alts.append(r"accelerators/(?:%s)\b[\w./-]*" % "|".join(self.benches))
        alts.append(r"accelerators/(?:prims|prooflib)/[\w-]+(?:/[\w-]+)*\.\w+")
        alts += [re.escape(p) for p in sorted(self.paths, key=len, reverse=True)
                 if not p.startswith(("backends/", "accelerators/"))]
        return re.compile("|".join(alts)) if alts else None

    def _basenames(self) -> re.Pattern[str] | None:
        """Names of generated prims headers and script/eval files, matched without their directory."""
        keep_names = {Path(f).name for f in self.keep}
        names = {Path(p).name for p in self.paths
                 if p.startswith(("accelerators/prims/", "scripts/", "eval/")) and Path(p).suffix
                 and not p.endswith(("README.md",))}
        names = {n for n in names if len(n) >= 8 and n not in keep_names and ("_" in n)}
        return re.compile(r"(?<![\w./-])(?:%s)\b" % "|".join(re.escape(n) for n in sorted(names, key=len, reverse=True))) \
            if names else None

    def _targets(self, root: Path) -> list[tuple[str, re.Pattern[str]]]:
        """(name, regex) of build targets defined by generated CMake or by KEEP CMake over generated sources."""
        names: set[str] = set()
        for rel in self.paths:
            if rel.endswith(".cmake"):
                for func, args in CMAKE_CALL_RE.findall(_read(root, rel)):
                    parts = args.split()
                    if func == "accel_add_backend_exe" and len(parts) > 1:
                        names.add(parts[1])
                    elif parts:
                        names.add(parts[0])
        for rel in self.keep:
            if rel == "CMakeLists.txt" or rel.startswith("cmake/"):
                for func, args in CMAKE_CALL_RE.findall(_read(root, rel)):
                    parts = args.split()
                    sources = [re.sub(r"\$\{[^}]*\}", "*", a) for a in parts[1:] if "/" in a]
                    if parts and any(fnmatch.filter(self.paths, s) for s in sources):
                        names.add(parts[0])
        names = {n for n in names if len(n) >= 6 and re.fullmatch(r"[\w${}]+", n) and not n.startswith("$")}
        return [(n, re.compile(_template_regex(n))) for n in sorted(names)]

    def _symbols(self, root: Path) -> set[str]:
        defined: set[str] = set()
        for rel in self.paths:
            if Path(rel).suffix in CPP_HEADERS:
                text = COMMENT_RE.sub(" ", _read(root, rel))
                for regex in TYPE_DEFINITION_RES:
                    defined.update(s for s in regex.findall(text) if _is_distinctive(s))
                defined.update(s for s in FUNCTION_RE.findall(text) if _is_distinctive(s, strict=True))
        substrate_tokens: set[str] = set()
        for rel in self.keep:
            if rel.startswith("substrate/") and Path(rel).suffix in CPP_HEADERS | {".cpp"}:
                substrate_tokens.update(TOKEN_RE.findall(_read(root, rel)))
        return {s for s in defined if s not in substrate_tokens and s not in NOT_SYMBOLS}


def _is_distinctive(name: str, strict: bool = False) -> bool:
    """Snake- or CamelCase and long enough: plain words are not symbols worth chasing.

    Function names are stricter (three words), as methods like `ToString` are not specific to anything."""
    words = name.strip("_").count("_") + 1 + len(re.findall(r"[a-z0-9][A-Z]", name))
    return len(name) >= 7 and words >= (3 if strict else 2)


class Allowlist:
    def __init__(self, path: Path):
        entries = yaml.safe_load(path.read_text(encoding="utf-8")) if path.is_file() else None
        self.entries = [(e["file"], e.get("kind"), re.compile(e["match"]), e["reason"]) for e in (entries or [])]

    def allows(self, f: Finding) -> bool:
        return any(fnmatch.fnmatch(f.file, glob) and kind in (None, f.kind) and rx.search(f.token)
                   for glob, kind, rx, _ in self.entries)


def scan_file(rel: str, text: str, index: GeneratedIndex) -> list[Finding]:
    is_cmake = rel.endswith(".cmake") or Path(rel).name == "CMakeLists.txt"
    defining_cmake = rel == "CMakeLists.txt" or rel.startswith("cmake/")
    engine_alt = "|".join(index.engines)
    cmake_re = re.compile(r"(?i)\b(?:ACCELERATOR_CODEGEN_BUILD_|accel_backend_)(?:%s)\b|ACCEL_DEFAULT_BACKEND\s+(?:%s)\b"
                          % (engine_alt, engine_alt)) if engine_alt and is_cmake else None
    engine_scope = not fnmatch.fnmatch(rel, ENGINE_PROFILES)
    findings: list[Finding] = []
    for number, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()[:200]

        def add(kind: str, token: str) -> None:
            findings.append(Finding(rel, number, kind, token, stripped))

        include = INCLUDE_RE.search(line)
        include_hit = False
        if include:
            target = include.group(1)
            candidates = {target, f"accelerators/{target}"}
            if any(c in index.paths for c in candidates) or ("/" in target and any(p.endswith("/" + target) for p in index.paths)):
                add("include", target)
                include_hit = True
        if not include_hit:
            if index.path_re:
                for m in index.path_re.finditer(line):
                    if m.group(0) not in index.skeleton:
                        add("path", m.group(0).rstrip(".,;:)"))
            if index.basenames:
                for m in index.basenames.finditer(line):
                    add("path", m.group(0))
        if cmake_re:
            for m in cmake_re.finditer(line):
                add("cmake", m.group(0))
        if not defining_cmake:
            for name, regex in index.targets:
                if regex.search(line):
                    add("target", name)
        if engine_scope:
            if index.branch_re.search(line):
                add("engine_branch", index.branch_re.search(line).group(0))
            m = index.engine_re.search(line)
            if m:
                add("engine", m.group(0).lower())
        if not include_hit:
            for m in TOKEN_RE.finditer(line):
                if m.group(0) in index.symbols:
                    add("symbol", m.group(0))
    return findings


def run(repo: Path, generated_from: Path | None = None) -> tuple[list[Finding], int, GeneratedIndex]:
    manifest = Manifest.load(repo / "pipeline/clean_tree/manifest.yaml")
    skeleton_root = repo / manifest.skeleton
    skeleton = {p.relative_to(skeleton_root).as_posix() for p in skeleton_root.rglob("*") if p.is_file()} \
        if skeleton_root.is_dir() else set()
    gen_root = generated_from or repo
    index = GeneratedIndex(gen_root, manifest, skeleton)
    if not index.paths:
        raise SystemExit(f"no GENERATED files in {gen_root}: point --generated-from at a full checkout")
    allow = Allowlist(repo / "pipeline/clean_tree/leakage_allowlist.yaml")
    findings, allowed = [], 0
    for rel in tracked_files(repo):
        if manifest.classify(rel) != KEEP or not _is_text(rel) or rel.startswith("pipeline/clean_tree/"):
            continue
        for f in scan_file(rel, _read(repo, rel), index):
            if allow.allows(f):
                allowed += 1
            else:
                findings.append(f)
    return findings, allowed, index


def counts(findings: list[Finding]) -> dict[str, dict[str, int]]:
    out: dict[str, Counter] = defaultdict(Counter)
    for f in findings:
        out[f.file][f.kind] += 1
    return {file: dict(c) for file, c in sorted(out.items())}


def regressions(findings: list[Finding], baseline: dict[str, dict[str, int]]) -> list[str]:
    bad = []
    for file, kinds in counts(findings).items():
        for kind, n in kinds.items():
            if n > baseline.get(file, {}).get(kind, 0):
                bad.append(f"{file}: {n} {kind} references (baseline {baseline.get(file, {}).get(kind, 0)})")
    return bad


def render_markdown(findings: list[Finding], allowed: int, index: GeneratedIndex, top: int = 8) -> str:
    engine_findings = [f for f in findings if f.kind in ENGINE_KINDS]
    findings = [f for f in findings if f.kind not in ENGINE_KINDS]
    by_group: dict[str, list[Finding]] = defaultdict(list)
    for f in findings:
        by_group[group_of(f.file)].append(f)
    lines = ["# Clean-tree leakage report", "",
             f"{len(findings)} references from {len({f.file for f in findings})} kept files to generated material "
             f"({allowed} allowlisted). Generated index: {len(index.paths)} files, {len(index.symbols)} symbols, "
             f"{len(index.targets)} targets, engines {index.engines}, benches {index.benches}.", "",
             "| group | files | references | " + " | ".join(KINDS) + " |", "|---|---|---|" + "---|" * len(KINDS)]
    for g, title in GROUPS.items():
        fs = by_group.get(g, [])
        k = Counter(f.kind for f in fs)
        lines.append(f"| ({g}) {title} | {len({f.file for f in fs})} | {len(fs)} | "
                     + " | ".join(str(k.get(kind, 0)) for kind in KINDS) + " |")
    for g, title in GROUPS.items():
        fs = by_group.get(g, [])
        if not fs:
            continue
        lines += ["", f"## ({g}) {title}", ""]
        per_file = Counter(f.file for f in fs)
        for file, n in per_file.most_common():
            kinds = Counter(f.kind for f in fs if f.file == file)
            lines.append(f"- `{file}`: {n} ({', '.join(f'{k} {v}' for k, v in kinds.most_common())})")
            seen: set[tuple[str, str]] = set()
            for f in sorted((x for x in fs if x.file == file), key=lambda x: x.line):
                if (f.kind, f.token) in seen or len(seen) >= top:
                    continue
                seen.add((f.kind, f.token))
                lines.append(f"    - `{file}:{f.line}` {f.kind} `{f.token}`")
    lines += ["", "## Engine-specific code in kept files", "",
              f"{len(engine_findings)} lines in {len({f.file for f in engine_findings})} files "
              "(outside the engine profiles); `branch` counts lines that branch on the engine.", ""]
    per_file: dict[str, Counter] = defaultdict(Counter)
    for f in engine_findings:
        per_file[f.file][f.kind] += 1
    for file, c in sorted(per_file.items(), key=lambda kv: -sum(kv[1].values())):
        lines.append(f"- `{file}`: {c['engine']} names, {c['engine_branch']} branch")
    return "\n".join(lines) + "\n"


def render_text(findings: list[Finding], allowed: int) -> str:
    lines = [f"{f.file}:{f.line}: [{f.kind}] {f.token}" for f in sorted(findings, key=lambda x: (x.file, x.line))]
    per_kind = Counter(f.kind for f in findings)
    lines.append(f"{len(findings)} leakage references ({', '.join(f'{k} {v}' for k, v in per_kind.most_common())}); "
                 f"{allowed} allowlisted")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    ap.add_argument("--generated-from", type=Path)
    ap.add_argument("--markdown", type=Path)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--baseline", type=Path)
    ap.add_argument("--update-baseline", type=Path)
    args = ap.parse_args()
    findings, allowed, index = run(args.repo, args.generated_from)
    if args.markdown:
        args.markdown.write_text(render_markdown(findings, allowed, index), encoding="utf-8")
    if args.update_baseline:
        args.update_baseline.write_text(json.dumps(counts(findings), indent=1, sort_keys=True) + "\n", encoding="utf-8")
    if args.json:
        print(json.dumps([f.__dict__ for f in findings], indent=1))
    else:
        print(render_text(findings, allowed))
    if args.baseline:
        bad = regressions(findings, json.loads(args.baseline.read_text(encoding="utf-8")))
        for line in bad:
            print("REGRESSION", line, file=sys.stderr)
        return 1 if bad else 0
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
