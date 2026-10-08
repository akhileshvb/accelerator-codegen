#pragma once

#include "duckdb.hpp"
#include "sec_edgar/q4/duckdb/aux/module.hpp"

namespace accelerator_codegen::aux_state::query {

// DuckDB-backed initial scan for Q4.  ScanBridge pushes the pre/sub/tag/num
// join down into the database (one streamed result of ~65K rows on SF3)
// instead of materializing the 39M-row num table row by row.
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

  ::duckdb::Connection &connection;
};

} // namespace accelerator_codegen::aux_state::query
