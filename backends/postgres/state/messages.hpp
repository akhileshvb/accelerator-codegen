// GENERATED BACKEND
//
// Messages between a database's backends and its shard workers, encoded with
// the substrate wire format. Pure C++ (no Postgres headers): the worker's
// non-Postgres threads decode them.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "substrate/aux/runtime/columnar_batch.hpp"
#include "substrate/aux/runtime/engine_slots.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {

// One committed (or, after its epoch was taken, aborted) transaction as
// accelerator `accelerator` sees it: its captured batches on the
// accelerator's tables in statement order (aborted subtransactions removed).
struct CommitMessage {
  uint32_t accelerator = 0;
  runtime::Epoch epoch = 0;
  bool empty = false;       // aborted after its epoch was taken
  bool invalidates = false; // e.g. TRUNCATE: rebuild
  std::vector<runtime::ColumnarBatchRef> batches;
};

void EncodeCommit(const CommitMessage &message, std::string &out);
// Throws std::runtime_error on malformed input.
CommitMessage DecodeCommit(std::string_view in);

// A read of the shard's accelerator at the reader's epoch. The reply goes to
// client slot `slot`.
struct ReadRequest {
  uint32_t slot = 0;
  // The reader gives up at this steady-clock time (nanoseconds since its
  // epoch, shared by all processes of the machine).
  int64_t deadline_ns = 0;
  runtime::Epoch epoch = 0;
  // How long the worker may wait for commits, a build or a catch-up first.
  uint64_t max_wait_us = 0;
  core::QueryArguments arguments;
};

// The reply: the rows, or that the reader must answer from SQL.
struct ReadResponse {
  bool served = false;
  std::vector<core::AuxRow> rows;
};

void EncodeRequest(const ReadRequest &request, std::string &out);
ReadRequest DecodeRequest(std::string_view in);
void EncodeResponse(const ReadResponse &response, std::string &out);
ReadResponse DecodeResponse(std::string_view in);

} // namespace accelerator_codegen::aux_state::engine::postgres
