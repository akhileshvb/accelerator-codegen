from __future__ import annotations

TOOL_SCHEMAS: dict[str, dict] = {
    "read_file": {
        "type": "object",
        "properties": {
            "path": {"type": "string", "description": "Repo-relative or absolute path under the repo root."},
            "offset": {
                "type": "integer",
                "description": "0-based starting line number. Omit or 0 to read from the start.",
                "default": 0,
            },
            "limit": {
                "type": "integer",
                "description": "Maximum number of lines to return. 0 means read the whole file.",
                "default": 0,
            },
            "numbered": {
                "type": "boolean",
                "description": "Prefix each line with its 1-based line number.",
                "default": False,
            },
        },
        "required": ["path"],
    },
    "write_file": {
        "type": "object",
        "properties": {
            "path": {"type": "string", "description": "Repo-relative path in an allowlisted write directory."},
            "content": {"type": "string", "description": "Full file contents to write."},
        },
        "required": ["path", "content"],
    },
    "apply_patch": {
        "type": "object",
        "properties": {
            "path": {"type": "string", "description": "Repo-relative path in an allowlisted write directory."},
            "old_string": {"type": "string", "description": "Exact text to replace (must match exactly once)."},
            "new_string": {"type": "string", "description": "Replacement text."},
        },
        "required": ["path", "old_string", "new_string"],
    },
    "insert_lines": {
        "type": "object",
        "properties": {
            "path": {"type": "string", "description": "Repo-relative path in an allowlisted write directory."},
            "after_line": {
                "type": "integer",
                "description": "Insert after this 1-based line number. Use 0 to insert at file start.",
            },
            "content": {"type": "string", "description": "Text to insert. A trailing newline is added if missing."},
        },
        "required": ["path", "after_line", "content"],
    },
    "delete_file": {
        "type": "object",
        "properties": {
            "path": {"type": "string", "description": "Repo-relative path in an allowlisted write directory."},
        },
        "required": ["path"],
    },
    "list_dir": {
        "type": "object",
        "properties": {
            "path": {"type": "string", "description": "Repo-relative directory path.", "default": "."},
            "glob": {"type": "string", "description": "Optional glob filter for entry names.", "default": "*"},
        },
        "required": [],
    },
    "glob_files": {
        "type": "object",
        "properties": {
            "pattern": {
                "type": "string",
                "description": "Glob pattern relative to path, e.g. '**/*.cpp'.",
            },
            "path": {"type": "string", "description": "Repo-relative root to search from.", "default": "."},
            "limit": {"type": "integer", "description": "Maximum matches to return.", "default": 100},
        },
        "required": ["pattern"],
    },
    "file_info": {
        "type": "object",
        "properties": {
            "path": {"type": "string", "description": "Repo-relative or absolute path under the repo root."},
        },
        "required": ["path"],
    },
    "search_repo": {
        "type": "object",
        "properties": {
            "pattern": {"type": "string", "description": "Regex or literal pattern for ripgrep."},
            "glob": {"type": "string", "description": "Optional file glob filter.", "default": "*"},
            "path": {"type": "string", "description": "Repo-relative directory to search.", "default": "."},
            "max_matches": {"type": "integer", "description": "Maximum matches to return.", "default": 50},
        },
        "required": ["pattern"],
    },
    "grep_context": {
        "type": "object",
        "properties": {
            "path": {"type": "string", "description": "Repo-relative file to search."},
            "pattern": {"type": "string", "description": "Regex pattern to match per line."},
            "context": {
                "type": "integer",
                "description": "Lines of context before and after each match.",
                "default": 3,
            },
            "max_matches": {
                "type": "integer",
                "description": "Maximum match lines to return.",
                "default": 12,
            },
            "case_insensitive": {
                "type": "boolean",
                "description": "Case-insensitive regex matching.",
                "default": False,
            },
        },
        "required": ["path", "pattern"],
    },
    "read_reference_files": {
        "type": "object",
        "properties": {
            "paths": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Up to 8 repo-relative file paths to read in one call.",
            },
            "limit_per_file": {
                "type": "integer",
                "description": "Max lines per file (numbered).",
                "default": 400,
            },
        },
        "required": ["paths"],
    },
    "run_command": {
        "type": "object",
        "properties": {
            "cmd": {"type": "string", "description": "Allowlisted build/test shell command."},
            "cwd": {"type": "string", "description": "Optional repo-relative working directory.", "default": ""},
            "timeout": {"type": "integer", "description": "Timeout in seconds.", "default": 600},
        },
        "required": ["cmd"],
    },
    "end_ab_trial": {
        "type": "object",
        "properties": {
            "trial_id": {"type": "string", "description": "Trial id passed to begin_ab_trial."},
            "keep": {
                "type": "boolean",
                "description": "True to keep the modified kernel; false to restore the checkpoint.",
            },
        },
        "required": ["trial_id", "keep"],
    },
    "begin_ab_trial": {
        "type": "object",
        "properties": {
            "trial_id": {"type": "string", "description": "Short unique id for this trial, e.g. dict_flat_adsh."},
            "hypothesis": {
                "type": "string",
                "description": "What this trial tests and which profile counter it targets.",
            },
        },
        "required": ["trial_id", "hypothesis"],
    },
    "run_quick_benchmark": {
        "type": "object",
        "properties": {
            "include_profile": {
                "type": "boolean",
                "description": "For base kernels: include profile counters. Ignored for aux workloads (aux quick sweep always returns workload metrics).",
                "default": True,
            },
        },
        "required": [],
    },
    "request_gate": {
        "type": "object",
        "properties": {
            "gate_name": {
                "type": "string",
                "description": "One of: correctness, baseline, sf10, profile, fairness, scaffold.",
            },
        },
        "required": ["gate_name"],
    },
    "request_structural_reanalysis": {
        "type": "object",
        "properties": {
            "bottleneck_type": {
                "type": "string",
                "enum": sorted(
                    [
                        "residual_read_path",
                        "scan_for_scan",
                        "wrong_partition_grain",
                        "maintenance_infeasible",
                        "insufficient_state_expressiveness",
                        "other",
                    ]
                ),
            },
            "evidence": {
                "type": "string",
                "description": "Concrete gate output or measurements (>= 40 chars).",
            },
            "proposed_direction": {
                "type": "string",
                "description": "Structural design change needed (>= 20 chars).",
            },
            "current_candidate_id": {
                "type": "string",
                "description": "Selected candidate id from analysis.json.",
            },
            "alternate_candidate_ids": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Optional viable alternates to reconsider.",
            },
            "cost_model_insufficient_reason": {
                "type": "string",
                "description": "Why cost-model tuning cannot fix the bottleneck.",
            },
        },
        "required": [
            "bottleneck_type",
            "evidence",
            "proposed_direction",
            "current_candidate_id",
        ],
    },
}
