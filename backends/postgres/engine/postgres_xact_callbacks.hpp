// GENERATED BACKEND - PostgreSQL transaction lifecycle of captured writes
//
// Statement captures are staged per transaction (a frame per subtransaction:
// commit merges into the parent, abort drops). At PRE_COMMIT the transaction
// takes a commit epoch of every active accelerator that depends on a table it
// wrote; once it is visible (COMMIT) one CommitMessage per accelerator is
// shipped to its shard worker; an abort after the epochs were taken ships
// `empty` commits so the sequence stays dense. Nothing uncommitted is shipped.
#pragma once

#include <memory>

#include "substrate/aux/core/aux_types.hpp"
#include "substrate/aux/runtime/columnar_batch.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {

// Idempotent; the capture trigger calls it on every statement.
void RegisterPostgresXactCallbacks();

// The current (sub)transaction wrote `table` (reads of accelerators over it
// answer from SQL; also true of tables no accelerator is active on yet).
void NoteWrite(core::TableId table);
// Stages the captured rows of one statement (implies NoteWrite).
void StageBatch(runtime::ColumnarBatchRef batch);
// A write that is not shipped as rows (TRUNCATE: the accelerators rebuild).
void StageInvalidation(core::TableId table);

} // namespace accelerator_codegen::aux_state::engine::postgres
