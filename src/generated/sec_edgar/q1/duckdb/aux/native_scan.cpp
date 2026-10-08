#include "sec_edgar/q1/duckdb/aux/module.hpp"

#include "accelerator_codegen/runtime/duckdb/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/runtime/native_scan.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

namespace accelerator_codegen::aux_state::query {

bool ScanSecEdgarQ1PreNative(
    ::duckdb::Connection &connection,
    const std::function<void(::duckdb::DataChunk &)> &consumer) {
  if (!connection.context) {
    return false;
  }
  auto &context = *connection.context;
  NativeScanSpec spec;
  spec.table_name = "pre";
  auto column = [](const char *name, ::duckdb::LogicalType type,
                   ::duckdb::PhysicalType physical) {
    return NativeColumnSpec{name, name, std::move(type), physical};
  };
  spec.projected_columns.push_back(
      column("stmt", ::duckdb::LogicalType::VARCHAR, ::duckdb::PhysicalType::VARCHAR));
  spec.projected_columns.push_back(
      column("rfile", ::duckdb::LogicalType::VARCHAR, ::duckdb::PhysicalType::VARCHAR));
  spec.projected_columns.push_back(
      column("adsh", ::duckdb::LogicalType::VARCHAR, ::duckdb::PhysicalType::VARCHAR));
  spec.projected_columns.push_back(
      column("line", ::duckdb::LogicalType::INTEGER, ::duckdb::PhysicalType::INT32));
  const bool started = !context.transaction.HasActiveTransaction();
  if (started) {
    context.transaction.BeginTransaction();
  }
  try {
    ScanNativeBaseTable(context, spec, consumer);
    if (started) {
      context.transaction.Commit();
    }
  } catch (...) {
    if (started && context.transaction.HasActiveTransaction()) {
      context.transaction.Rollback(nullptr);
    }
    throw;
  }
  return true;
}

} // namespace accelerator_codegen::aux_state::query
