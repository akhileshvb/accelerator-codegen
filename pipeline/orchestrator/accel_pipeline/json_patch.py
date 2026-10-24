"""Small edits to a JSON document: the add / replace / remove operations of RFC 6902 with RFC 6901 paths.

The analysis agent revises analysis.json with a few targeted operations instead of regenerating the
whole document."""

from __future__ import annotations

import copy
from typing import Any


class PatchError(ValueError):
    pass


def _tokens(path: str) -> list[str]:
    if path == "":
        return []
    if not path.startswith("/"):
        raise PatchError(f"path {path!r} must start with '/'")
    return [t.replace("~1", "/").replace("~0", "~") for t in path[1:].split("/")]


def _index(container: list, token: str, *, append: bool) -> int:
    if append and token == "-":
        return len(container)
    if not token.isdigit():
        raise PatchError(f"list index {token!r} is not a number")
    i = int(token)
    if i > len(container) or (i == len(container) and not append):
        raise PatchError(f"list index {i} out of range (length {len(container)})")
    return i


def _parent(doc: Any, tokens: list[str], path: str) -> Any:
    node = doc
    for token in tokens[:-1]:
        if isinstance(node, dict) and token in node:
            node = node[token]
        elif isinstance(node, list):
            node = node[_index(node, token, append=False)]
        else:
            raise PatchError(f"{path}: no {token!r}")
    return node


def apply(doc: Any, ops: list[dict[str, Any]]) -> Any:
    """A patched copy of `doc`; raises PatchError naming the first operation that cannot apply."""
    out = copy.deepcopy(doc)
    for n, op in enumerate(ops):
        kind, path = op.get("op"), op.get("path")
        if kind not in ("add", "replace", "remove") or not isinstance(path, str):
            raise PatchError(f"operation {n}: need op add|replace|remove and a string path, got {op}")
        tokens = _tokens(path)
        if not tokens:
            if kind == "remove":
                raise PatchError(f"operation {n}: cannot remove the whole document")
            out = copy.deepcopy(op["value"])
            continue
        parent, last = _parent(out, tokens, path), tokens[-1]
        if isinstance(parent, dict):
            if kind != "add" and last not in parent:
                raise PatchError(f"operation {n}: {path} does not exist")
            if kind == "remove":
                del parent[last]
            else:
                parent[last] = copy.deepcopy(op["value"])
        elif isinstance(parent, list):
            i = _index(parent, last, append=kind == "add")
            if kind == "add":
                parent.insert(i, copy.deepcopy(op["value"]))
            elif kind == "replace":
                parent[i] = copy.deepcopy(op["value"])
            else:
                del parent[i]
        else:
            raise PatchError(f"operation {n}: {path} has no container to edit")
    return out
