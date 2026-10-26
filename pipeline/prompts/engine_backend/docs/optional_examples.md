# Placement patterns (slot S6; engine-agnostic)

These describe **how** a backend can host the runtime. They are not
requirements to clone any particular existing engine.

## `in_process`

- The backend's host owns one `RuntimeHost` per database; builds and applies run
  on pool threads and the writer's thread (`EngineWorkOnHostThread() == false`).
- Capture calls `RuntimeHost::OnCapture` directly; `PreCommit` / `Commit` /
  `Abort` come from the engine's commit callbacks.
- Reads call `AcceleratorRuntime::Read` on the reader's thread; no
  serialization anywhere.
- The engine's own thread count is the budget's engine setting
  (`ResolveThreadBudget`).

## `shipped_worker`

- The engine runs a process per connection, so the runtime lives in one worker
  per accelerator (a background worker hosting a `RuntimeHost`).
- Writers ship each commit to the worker as a `wire.hpp`-encoded
  `CommittedChange` (`OfferCommit` / `AnnounceCommit`); the worker applies in
  sequence order on its engine thread (`Pump`, `EngineWorkOnHostThread() == true`).
- Readers send a read request (epoch, arguments) and get rows back; the worker's
  **read threads never call the engine** (own rings over shared memory, no
  engine headers in those files, signals blocked).
- A commit that cannot be delivered becomes a gap; the worker rebuilds. A
  restarted worker rebuilds on demand. The thread budget is a token pool
  shared by all of a database's workers (`TokenSource`).

Choose from the workload `placement_candidates`. Prefer the placement that
matches this engine's process model; document the choice in `backend_analysis.json`.
