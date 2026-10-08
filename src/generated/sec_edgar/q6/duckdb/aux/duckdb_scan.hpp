#pragma once

#include "sec_edgar/q6/duckdb/aux/module.hpp"

namespace duckdb {
class Connection;
}

namespace accelerator_codegen::aux_state::query {

// DuckDB initial-scan source for Q6. Streams projected/pre-filtered column
// chunks (no per-cell Value materialization) and offers the engine-side
// group-aggregate fast path used by Build().
class SecEdgarQ6DuckdbScan : public SecEdgarQ6InitialScan {
public:
  explicit SecEdgarQ6DuckdbScan(::duckdb::Connection &connection)
      : connection(connection) {}

  void ScanNum(
      const std::function<void(const SecEdgarQ6NumInput &)> &consumer) override;
  void ScanSub(
      const std::function<void(const SecEdgarQ6SubInput &)> &consumer) override;
  void ScanPre(
      const std::function<void(const SecEdgarQ6PreInput &)> &consumer) override;
  bool ScanGroupAggregates(
      const std::function<void(const SecEdgarQ6GroupAggregateInput &)> &consumer)
      override;
  bool LoadAdshSnapshots(const std::vector<std::string> &adshs,
                         std::vector<SecEdgarQ6AdshSnapshot> &out) override;

private:
  ::duckdb::Connection &connection;
};

} // namespace accelerator_codegen::aux_state::query
