// GENERATED BACKEND — txn + mutations + scan + runtime layer bridge header
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_types.hpp"
#include "accelerator_codegen/stateful/core/aux_registry.hpp"
#include "accelerator_codegen/stateful/core/aux_txn_buffer.hpp"
#include "accelerator_codegen/stateful/postgres/postgres_state_store.hpp"
#include "accelerator_codegen/stateful/postgres/postgres_aux_backend.hpp"

namespace accelerator_codegen::aux_state::runtime::postgres {

using PgStore = engine::postgres::PostgresStateStore;
using PgBackend = engine::postgres::PostgresAuxBackend;

void InitTxnBridge();
void ShutdownTxnBridge();

core::AuxTransactionCoordinator &GetCoordinator();
PgStore &GetStateStore();
PgBackend &GetAuxBackend();

// txn-layer test helpers
std::string TxnTestCaptureMutation(int64_t table_id, int kind_int,
                                    int64_t count_value,
                                    const std::string &label);
std::string TxnTestReadState();
std::string TxnTestCheckEpoch();
std::string TxnTestResetAll();

// mutations-layer: trigger capture recording
void RecordCapturedMutation(const core::MutationBatch &mutation);
std::string MutationsTestLastCapture();
std::string MutationsTestDrainCaptures();
std::string MutationsTestCaptureCount();

// scan-layer: native table scan init and UDF bridge
void InitScanBridge();
std::string ScanRows(const std::string &table_name,
                     const std::string &columns_csv,
                     const std::string &filter_col,
                     const std::string &filter_val);

// runtime-layer: Toy Runtime bridge
void InitRuntimeBridge(const std::string &table_name);
void ShutdownRuntimeBridge();
std::string RuntimeBuild();
std::string RuntimeExecute();
std::string RuntimeReadState();

// visibility-layer: uncommitted write isolation test helpers
void InitVisibilityBridge();
std::string VisTestCaptureMutation(int64_t table_id, int kind_int,
                                     int64_t count_value,
                                     const std::string &label);
std::string VisTestReadState();
std::string VisTestCheckEpoch();
std::string VisTestResetAll();

} // namespace accelerator_codegen::aux_state::runtime::postgres
