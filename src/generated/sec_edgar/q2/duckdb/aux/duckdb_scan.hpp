#pragma once

#include "duckdb.hpp"
#include "sec_edgar/q2/duckdb/aux/module.hpp"

namespace accelerator_codegen::aux_state::query {

// DuckDB implementation of the Q2 data-access interface.  Every method is a
// single columnar SQL statement or a native table scan on the caller's
// connection (so it sees the caller's open transaction, including uncommitted
// writes during PreCommit).
class SecEdgarQ2DuckdbScan : public SecEdgarQ2InitialScan {
public:
  explicit SecEdgarQ2DuckdbScan(::duckdb::Connection &connection_p)
      : connection(connection_p) {}

  void ScanNum(
      const std::function<void(const SecEdgarQ2NumInput &)> &consumer) override;
  void ScanSub(
      const std::function<void(const SecEdgarQ2SubInput &)> &consumer) override;
  void ScanUomRowCounts(
      const std::function<void(const std::string &, uint64_t)> &consumer) override;
  void ScanNumValueCounts(
      const std::vector<std::string> &uoms,
      const std::function<void(const SecEdgarQ2ValueCountRow &)> &consumer) override;
  // Native storage scan of num (no SQL, no GROUP BY): dictionary-compressed
  // string columns are mapped to ids once per dictionary entry.
  bool ScanNumIds(SecEdgarQ2KeyMap &keys,
                  const std::function<void(const SecEdgarQ2IdBatch &)> &batch) override;
  void LookupNumValueCounts(
      const std::string &uom,
      const std::vector<std::pair<std::string, std::string>> &groups,
      const std::function<void(const SecEdgarQ2ValueCountRow &)> &consumer) override;
  void LookupSub(
      const std::vector<std::string> &adshs,
      const std::function<void(const SecEdgarQ2SubInput &)> &consumer) override;
  void ExecuteDirect(
      int64_t fy, const std::string &uom,
      const std::function<void(const SecEdgarQ2DirectRow &)> &consumer) override;

private:
  ::duckdb::Connection &connection;
};

} // namespace accelerator_codegen::aux_state::query
