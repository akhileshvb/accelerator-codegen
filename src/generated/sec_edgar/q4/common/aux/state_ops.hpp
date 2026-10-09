#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "accelerator_codegen/stateful/core/flat_hash_map.hpp"
#include "sec_edgar/q4/common/aux/module.hpp"
#include "sec_edgar/q4/common/aux/state.hpp"

namespace accelerator_codegen::aux_state::query::q4 {

// Copy-on-write writer: starts from a published state (or an empty one) and
// produces the next version.  Every container it touches is cloned lazily and
// only as far as the touched shard / chunk, so a write costs O(touched).
class Writer {
public:
  // base == nullptr starts from an empty state (full build).
  explicit Writer(const SecEdgarQ4State *base);

  // One num row appears (sign=+1) / disappears (sign=-1).  false => the state
  // is inconsistent with the delta (caller rebuilds from the base tables).
  bool ApplyNum(std::string_view adsh, std::string_view tag, std::string_view version,
                std::string_view uom, double value, int sign);

  // Structural maintenance: drop every record the dirty filings / tags / keys
  // can touch (finds them by one pass over the record maps), then AddRow the
  // restricted re-read.  false => invariant broke, caller rebuilds.
  bool RemoveDirty(const std::vector<std::string> &adsh,
                   const std::vector<std::pair<std::string, std::string>> &tag_versions,
                   const std::vector<SecEdgarQ4BridgeFilter::Key> &keys);
  bool AddRow(const SecEdgarQ4BridgeRow &row);

  bool bad() const { return bad_; }
  // Finishes the batch.  The returned state shares every untouched chunk.
  SecEdgarQ4State Finish();

private:
  using Shard = SecEdgarQ4GroupShard;

  SecEdgarQ4Cohort &MutCohort();
  SecEdgarQ4StrDict &MutTvDict();
  std::vector<uint32_t> &MutTvTl();
  SecEdgarQ4StrDict &MutTlabels();
  SecEdgarQ4Sud &MutSud();
  const SecEdgarQ4Cohort &Coh() const { return mut_cohort_ ? *mut_cohort_ : *cohort_; }
  Shard &MutShard(uint32_t gid);
  const Shard &ConstShard(uint32_t gid) const;
  uint32_t GroupFor(uint32_t sic_id, uint32_t tl, uint32_t stmt, uint32_t uom);
  // group += mult * (hi + lo); rows joined rows for the cik.
  bool Contribute(uint32_t gid, uint32_t cik_id, double hi, double lo, double mult,
                  int64_t rows, bool removing);
  uint32_t TvFind(std::string_view tag, std::string_view version);

  std::shared_ptr<const SecEdgarQ4Cohort> cohort_;
  std::shared_ptr<const SecEdgarQ4StrDict> tv_dict_;
  std::shared_ptr<const std::vector<uint32_t>> tv_tl_;
  std::shared_ptr<const SecEdgarQ4Sud> sud_;
  std::shared_ptr<const SecEdgarQ4Labels> labels_;
  std::shared_ptr<const SecEdgarQ4Groups> base_groups_;
  std::shared_ptr<SecEdgarQ4Cohort> mut_cohort_;
  std::shared_ptr<SecEdgarQ4StrDict> mut_tv_dict_;
  std::shared_ptr<std::vector<uint32_t>> mut_tv_tl_;
  std::shared_ptr<SecEdgarQ4Sud> mut_sud_;
  std::shared_ptr<SecEdgarQ4Labels> mut_labels_;

  SecEdgarQ4PreMap pre_;
  SecEdgarQ4NumMap num_;
  SecEdgarQ4PairMap pairs_;
  SecEdgarQ4GroupIndex gindex_;
  std::vector<std::shared_ptr<Shard>> own_;
  std::vector<std::shared_ptr<const Shard>> shards_;
  uint32_t group_count_ = 0;
  uint64_t generation_ = 0;
  bool bad_ = false;
  std::string scratch_;
};

// Resolves the 4 parameters; ids are kQ4None when the value is unknown.
std::shared_ptr<const SecEdgarQ4Result>
ComputeResult(const SecEdgarQ4Groups &groups, const SecEdgarQ4Params &params);

// Cached ComputeResult (per published version).
std::shared_ptr<const SecEdgarQ4Result>
CachedResult(const SecEdgarQ4Groups &groups, const SecEdgarQ4Params &params);

// Candidate filter used by ComputeResult (exposed for tests/benchmarks):
// appends the ids of groups with distinct >= 2 matching the parameters.
void CollectCandidates(const SecEdgarQ4Groups &groups, int32_t lo, int32_t hi, uint32_t su,
                       std::vector<uint32_t> &out, bool allow_simd = true);

} // namespace accelerator_codegen::aux_state::query::q4
