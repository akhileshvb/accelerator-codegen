#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "accelerator_codegen/stateful/core/flat_hash_map.hpp"
#include "sec_edgar/q4/common/aux/module.hpp"
#include "sec_edgar/q4/common/aux/state.hpp"

namespace accelerator_codegen::aux_state::query::q4 {

// Mutable, flat working representation used to build a state from scratch and
// to apply structural (sub/tag/pre) maintenance.  Not on the num hot path.
struct Flat {
  struct Group {
    int32_t sic = 0;
    uint32_t tl = kQ4None;
    double hi = 0.0, lo = 0.0;
    int64_t count = 0;
    uint32_t distinct = 0;
  };

  SecEdgarQ4Cohort cohort;
  core::FlatHashMap<uint32_t> cik_index;
  SecEdgarQ4StrDict tv;
  std::vector<uint32_t> tv_tl;
  SecEdgarQ4StrDict tlabels;
  std::vector<SecEdgarQ4Entry> entries;
  core::FlatHashMap<uint32_t> entry_index; // (adsh id << 32 | tv id) -> entry
  std::vector<Group> groups;
  core::FlatHashMap<uint32_t> group_index; // (sic << 32 | tlabel id) -> group
  core::FlatHashMap<int64_t> pairs;        // Q4PairKey(group, cik id) -> rows
  bool bad = false; // an invariant broke: caller must rebuild from scratch

  uint32_t GroupFor(int32_t sic, uint32_t tl);
  void PairAdd(uint32_t group, uint32_t cik_id, int64_t delta);
  void Contribute(const SecEdgarQ4Entry &e, int sign);
  void AddRow(const SecEdgarQ4BridgeRow &row);
  void RemoveEntry(uint32_t idx);
};

void StateToFlat(const SecEdgarQ4State &state, Flat &flat);
SecEdgarQ4State FlatToState(Flat &flat, uint64_t generation);

// Copy-on-write application of num deltas on top of a published state.  Only
// the shards that a delta touches are cloned.
class CowApplier {
public:
  explicit CowApplier(const SecEdgarQ4State &base);
  // false => state is inconsistent with the delta (caller rebuilds).
  bool Apply(std::string_view adsh, std::string_view tag,
             std::string_view version, double value, int sign);
  // Finishes the batch.  The returned state shares every untouched chunk.
  SecEdgarQ4State Finish();

private:
  SecEdgarQ4State base_;
  const SecEdgarQ4Cohort &cohort;
  const SecEdgarQ4Bridge &bridge;
  const SecEdgarQ4Groups &groups;
  // Private copy-on-write views: the first write to a chunk/shard clones it.
  SecEdgarQ4EntryArray entries;
  SecEdgarQ4PairMap pairs;
  std::vector<std::shared_ptr<SecEdgarQ4GroupShard>> new_g;
  std::string scratch;
};

std::shared_ptr<const SecEdgarQ4Result>
ComputeResult(const SecEdgarQ4Groups &groups);

// Candidate filter used by ComputeResult (exposed for tests/benchmarks):
// appends the ids of groups with distinct >= 2 and count > 0.
void CollectCandidates(const SecEdgarQ4Groups &groups,
                       std::vector<uint32_t> &out, bool allow_simd = true);

} // namespace accelerator_codegen::aux_state::query::q4
