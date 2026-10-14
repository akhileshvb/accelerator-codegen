#pragma once

#include "duckdb.hpp"
#include "sec_edgar/q4/common/aux/module.hpp"

namespace accelerator_codegen::aux_state::query {

// DuckDB-backed initial scan for Q4.  The full build streams pre and num
// straight from table storage into q4::Builder; structural maintenance reads
// only the rows of the dirty keys (ScanDirty).
class SecEdgarQ4DuckDbScan : public SecEdgarQ4InitialScan {
public:
  explicit SecEdgarQ4DuckDbScan(::duckdb::Connection &connection)
      : connection(connection) {}

  void ScanSub(const std::function<void(const SecEdgarQ4SubInput &)> &) const override;
  void ScanTag(const std::function<void(const SecEdgarQ4TagInput &)> &) const override;
  void ScanPre(const std::function<void(const SecEdgarQ4PreInput &)> &) const override;
  void ScanNum(const std::function<void(const SecEdgarQ4NumInput &)> &) const override;
  void ScanBridge(const SecEdgarQ4BridgeFilter &,
                  const std::function<void(const SecEdgarQ4BridgeRow &)> &) const override;
  void ScanDirty(const SecEdgarQ4BridgeFilter &, SecEdgarQ4DirtyRows &) const override;
  // sub / tag through SQL, then ONE native storage scan of pre and of num
  // (dictionary-coded strings are mapped to ids once per dictionary entry).
  bool BuildFull(q4::Builder &) const override;

  ::duckdb::Connection &connection;
};

} // namespace accelerator_codegen::aux_state::query
