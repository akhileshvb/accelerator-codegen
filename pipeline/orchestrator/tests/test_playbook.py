from __future__ import annotations

import json
from pathlib import Path

from accel_pipeline.playbook import (
    PLAYBOOK_PROPOSAL_JSON,
    PLAYBOOK_TECHNIQUES_JSON,
    merge_playbook_proposal,
)


def test_merge_playbook_proposal_json(tmp_path: Path):
    repo = tmp_path / "repo"
    art = repo / "pipeline" / "artifacts" / "sec_edgar" / "q1"
    art.mkdir(parents=True)
    workload = {"artifact_namespace": "sec_edgar"}

    proposal = {
        "iteration": 2,
        "summary": "Dict-index adsh helped consume.",
        "techniques": [
            {
                "technique_id": "dict_index_adsh",
                "hypothesis": "track adsh by dictionary child index",
                "kept": True,
                "speedup_delta": 0.15,
                "profile_deltas": {"pre_consume_us": -12000},
            }
        ],
    }
    (art / PLAYBOOK_PROPOSAL_JSON).write_text(json.dumps(proposal), encoding="utf-8")

    assert merge_playbook_proposal(repo, 1, workload=workload) is True

    store = json.loads((art / PLAYBOOK_TECHNIQUES_JSON).read_text(encoding="utf-8"))
    assert store["techniques"][0]["technique_id"] == "dict_index_adsh"

    shared = repo / "pipeline" / "artifacts" / "shared" / "optimization_playbook.md"
    text = shared.read_text(encoding="utf-8")
    assert "dict_index_adsh" in text
    assert "Dict-index adsh helped consume." in text
