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
    "run_command": {
        "type": "object",
        "properties": {
            "cmd": {"type": "string", "description": "Allowlisted build/test shell command."},
            "cwd": {"type": "string", "description": "Optional repo-relative working directory.", "default": ""},
            "timeout": {"type": "integer", "description": "Timeout in seconds.", "default": 600},
        },
        "required": ["cmd"],
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
}
