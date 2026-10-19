# Slot gate adapters

One file per engine, sourced by `../run_slot_gates.sh` (which provides `ROOT`
and `BUILD_DIR`). It defines

    slot_adapter_run <scenario> <seconds>

for the scenarios `epoch_stress`, `hits_under_writes`, `read_after_commit`,
`txn_edges`, `restart`, `own_writes`, `ddl_events`, `differential` and exits 0 (pass), 1 (fail) or 77 (not
applicable to this engine: the gate is skipped). Output is the gate's log.

Prefer the generic harness (`eval/harness/slot_gates.hpp`): implement
`slot_gates::Engine` for the engine and call `slot_gates::Run`
(`eval/tests/test_slot_gates_duckdb.cpp` is the reference). An adapter that
maps a scenario to an engine's existing test driver must say, in its header,
which part of the scenario the driver does not cover.
