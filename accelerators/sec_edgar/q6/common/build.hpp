#pragma once

// Bulk (initial) build of the Q6 state in one pass per base table:
//   1. sub rows        -> cohort (adsh -> name, fy)
//   2. pre batches     -> bridge runs + a flat (adsh, tag, version) -> hit table
//   3. num batches     -> per (hit, uom) double-double sums and counts
//   4. Finish          -> per group sums (hit x run entries), groups, top caches
// String columns are resolved to ids once per dictionary entry when the scan
// hands out dictionary-encoded batches.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "prims/flat_hash_map.hpp"

#include "sec_edgar/q6/common/module.hpp"

namespace accelerator_codegen::aux_state::query {

class SecEdgarQ6Builder {
public:
  SecEdgarQ6Builder();
  ~SecEdgarQ6Builder();
  SecEdgarQ6Builder(const SecEdgarQ6Builder &) = delete;
  SecEdgarQ6Builder &operator=(const SecEdgarQ6Builder &) = delete;

  // Rows with a NULL fy are not passed. Duplicate adsh keep the first row.
  void AddCohort(std::string_view adsh, int32_t fy, std::string_view name,
                 bool name_is_null);
  // Every pre row (rows of adsh outside the cohort / with NULL keys are skipped).
  void AddPreBatch(const SecEdgarQ6PreBatch &batch);
  void FinishPre(const core::AuxEngineServices &services);
  // Every num row (rows that cannot contribute are skipped).
  void AddNumBatch(const SecEdgarQ6NumBatch &batch);
  std::shared_ptr<SecEdgarQ6State> Finish();

  // Partitioned num phase (core::PartitionedBuild, after FinishPre): one
  // partial per partition holds double-double sums per (hit, unit of measure);
  // the partials are merged in partition order, so with a fixed partition
  // count the sums do not depend on the thread count.
  struct NumPartial {
    struct Sum {
      uint32_t hit, uom; // uom: index into `uoms`
      double hi = 0.0, lo = 0.0;
      int64_t cnt = 0;
    };
    std::vector<std::string> uoms;
    core::FlatHashMap<uint32_t> index; // (hit << 32 | uom) -> index into sums
    std::vector<Sum> sums;

    void Add(uint32_t hit, std::string_view uom, double hi, double lo, int64_t cnt);
    void Merge(NumPartial &&from);
  };
  // Reads the cohort, bridge and hit tables only.
  void AddNum(NumPartial &partial, std::string_view adsh, std::string_view tag,
              std::string_view version, std::string_view uom, double value) const;
  void MergeNum(NumPartial &&partial);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Adds a row of a partitioned num scan (columns: adsh, tag, version, uom, value;
// all nullable). `Row` is the engine's build scan row (IsNull, TextView, Double).
template <class Row>
void AddNumRow(const SecEdgarQ6Builder &builder, SecEdgarQ6Builder::NumPartial &partial, const Row &row) {
  for (int column = 0; column < 5; ++column) {
    if (row.IsNull(column)) return; // cannot join or contribute
  }
  builder.AddNum(partial, row.TextView(0), row.TextView(1), row.TextView(2), row.TextView(3), row.Double(4));
}

} // namespace accelerator_codegen::aux_state::query
