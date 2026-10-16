#pragma once

#include "sec_edgar/q6/duckdb/aux/module.hpp"

namespace duckdb {
class Connection;
}

namespace accelerator_codegen::aux_state::query {

// DuckDB initial-scan source for Q6. The build reads pre and num straight from
// table storage as columnar batches (dictionary vectors stay dictionary
// encoded); sub and the targeted per-adsh lookups go through SQL.
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
  bool ScanPreBatches(
      const std::function<void(const SecEdgarQ6PreBatch &)> &consumer) override;
  bool ScanNumBatches(
      const std::function<void(const SecEdgarQ6NumBatch &)> &consumer) override;
  bool ScanNumPartitioned(const core::AuxEngineServices &services,
                          SecEdgarQ6Builder &builder) override;
  bool LoadAdshSnapshots(const std::vector<std::string> &adshs,
                         std::vector<SecEdgarQ6AdshSnapshot> &out) override;

private:
  ::duckdb::Connection &connection;
};

} // namespace accelerator_codegen::aux_state::query
