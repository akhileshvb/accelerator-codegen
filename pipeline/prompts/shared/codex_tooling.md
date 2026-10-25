# Tools

You run under Codex: native tools to read, search, edit and run shell commands, plus the pipeline's MCP tools on server `accel_pipeline` (call them by name; they are tools, not resources).

- **Edit only the files your stage owns.** Pipeline files are restored after every turn.
- **Write your own small tests and scripts** whenever they answer a question faster than a pipeline tool: an oracle test, a micro-benchmark, a timer, a parser for a result file.
- **`quick_check` is the inner loop (10-30 s):** build, the query's tests, your `targets` (the oracle test) and the frozen suite on the join-closed subset at N and 1 threads. It reports build time and thread speedup with a full-scale projection, read time against the engine's SQL, each write's maintenance action and projected cost, and perf counters; a rebuild-like write or a serial or slow build fails it. `sanitizer="thread"` runs on a tiny database in seconds. Run it after every edit and fix what it reports first.
- **`run_profile`** (about 20 s): hot spots by symbol of the read, write and build paths; it fails when perf cannot run.
- **Heavy checks run once, at stage exit, started by the orchestrator.** You do not need to run them yourself. If you want an early answer, start one with `request_gate(<gate>, background=True)` after `quick_check` passes, keep working, and read it once with `gate_status` when you need it. Never poll, and never run the full-size binaries from the shell instead.
- **Stuck on one failure for ~10 minutes, or about to declare part of the plan unbuildable:** `ask_advisor(question)`.
- **Obstacle outside your files:** work around it in your files. When you cannot, `report_blocker(summary, evidence)` and stop: the orchestrator re-checks and comes back to you to adapt.
- **A tool result ending with a budget notice is real:** leave the tree building with the last passing version in place, and finish the step you are on.
