# Tools

You run under Codex: native tools to read, search, edit and run shell commands, plus the pipeline's MCP tools on server `accel_pipeline` (call them by name; they are tools, not resources).

- **Edit only the files your stage owns.** Pipeline files are restored after every turn.
- **Write your own small tests and scripts** whenever they answer a question faster than a pipeline tool: an oracle test, a micro-benchmark, a timer, a parser for a result file.
- **`quick_check` is the inner loop:** build, the query's tests and the frozen suite on the small subset, with write cost, build time and thread scaling; `sanitizer="thread"` for races. Run it after every edit and fix what it reports first.
- **Heavy checks run once, at stage exit, started by the orchestrator.** You do not need to run them yourself. If you want an early answer, start one with `request_gate(<gate>, background=True)` after `quick_check` passes, keep working, and read it once with `gate_status` when you need it. Never poll, and never run the full-size binaries from the shell instead.
- **Stuck on one failure for ~10 minutes, or about to declare part of the plan unbuildable:** `ask_advisor(question)`.
- **Obstacle outside your files:** `report_blocker(summary, evidence)` and stop.
- **A tool result ending with a budget notice is real:** leave the tree building with the last passing version in place, and finish the step you are on.
