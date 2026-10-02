## Ground rules (every step)

* Work only inside the workspace (`{{workspace}}`, your current directory).
  Read `README.md` first. Do not read files outside it: no other accelerators,
  no other kernels, no repository sources. Everything you need is here, and
  runs that copy outside work are flagged by the audit.
* Build, verify, test and measure only through the MCP tools of the
  `accel_pipeline` server (`verus_*`); they run in the pipeline's build tree.
  Do not run cargo, cmake, verus or the engine yourself.
* Frozen files (spec/, frozen/, crate/src/gen_*.rs, and tests/ once frozen)
  are fixed: never edit them. If one seems wrong, say so in your final message.
* Lessons worth keeping for future queries go in the playbook with
  `playbook_append` (with evidence: numbers, what you tried).
* End with a short final message; when the step asks for a JSON block, put it
  last, in a ```json fence.
