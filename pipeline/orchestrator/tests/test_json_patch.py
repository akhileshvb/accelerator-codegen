from __future__ import annotations

import pytest

from accel_pipeline.json_patch import PatchError, apply


DOC = {"design": {"storage": [{"component": "a", "primitive": "X"}, {"component": "b", "primitive": "X"}]}}


def test_replace_add_remove_on_nested_lists_and_keys():
    out = apply(DOC, [
        {"op": "replace", "path": "/design/storage/1/primitive", "value": "CowMap"},
        {"op": "add", "path": "/design/storage/-", "value": {"component": "c", "primitive": "CowPagedArray"}},
        {"op": "add", "path": "/design/note", "value": "grouped"},
        {"op": "remove", "path": "/design/storage/0"},
    ])
    assert [s["primitive"] for s in out["design"]["storage"]] == ["CowMap", "CowPagedArray"]
    assert out["design"]["note"] == "grouped" and DOC["design"]["storage"][1]["primitive"] == "X"  # input untouched


def test_escaped_tokens_and_errors():
    assert apply({"a/b": {"~k": 1}}, [{"op": "replace", "path": "/a~1b/~0k", "value": 2}]) == {"a/b": {"~k": 2}}
    for op in ({"op": "replace", "path": "/design/missing", "value": 1},
               {"op": "remove", "path": "/design/storage/5"},
               {"op": "move", "path": "/design"},
               {"op": "add", "path": "design", "value": 1}):
        with pytest.raises(PatchError):
            apply(DOC, [op])
