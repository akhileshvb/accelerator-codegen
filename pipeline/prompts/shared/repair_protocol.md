# Repair protocol

You are in the session that wrote this code. After a failure the orchestrator sends the failure report as a message; your earlier context stays, so use it.

1. Read the report's diagnosis and reproduce command. Reproduce the failure yourself first (a small test or the failing binary), then find the root cause.
2. Patch the smallest set of files in your query directory. Do not weaken tests, fixtures or checks to make a gate pass.
3. Rebuild and re-run what failed. The orchestrator re-runs the gates after your turn; end the turn when your own reproduction passes and the tree builds.
4. Failure kinds:
   - `code`: a build, test, differential or sanitizer failure in your files.
   - `measurement`: the benchmark did not measure your accelerator (for example it ran the fallback path). Fix registration, the build, or which SQL entry the harness reads through; never the bench counters, thresholds or numbers.
   - `timeout`: you were stopped at a wall deadline. State where you were, checkpoint the tree so it builds, then continue.
5. Wall-budget notices appended to tool results are real: when one says wrap up, checkpoint and finish the turn.
6. A failure that is not yours (the engine, the host harness, the machine) is retried by the orchestrator without you. If you believe a failure is not caused by your code, say so in one line and state the evidence, and never read pipeline, gate or orchestrator code to work around it: call `report_blocker` with the evidence.
