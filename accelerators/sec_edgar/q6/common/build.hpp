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
#include <string_view>

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

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace accelerator_codegen::aux_state::query
