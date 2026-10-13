#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "accelerator_codegen/stateful/core/flat_hash_map.hpp"
#include "sec_edgar/q4/common/aux/module.hpp"
#include "sec_edgar/q4/common/aux/state.hpp"

namespace accelerator_codegen::aux_state::query::q4 {

// Copy-on-write writer: starts from a published state and produces the next
// version.  Every container it touches is cloned lazily and only as far as the
// touched shard, so a write costs O(touched).  Any `false` return means the
// state is inconsistent with the delta (the caller rebuilds from the base
// tables); the writer must then be discarded.
class Writer {
public:
  explicit Writer(const SecEdgarQ4State &base);

  // Structural maintenance (sub / tag / pre writes): removes the old
  // contribution of every key the dirty filings / tags / keys can touch and
  // adds the current one, from `rows` (the current base rows of those keys) and
  // the transaction's own num deltas.  Returns the affected keys
  // ("adsh\0tag\0version") in `keys_out`: their num deltas are already applied.
  bool ApplyStructural(const SecEdgarQ4DirtyRows &rows, const SecEdgarQ4TxnBuffer &buffer,
                       std::unordered_set<std::string> &keys_out);

  // All num deltas of the buffer (except those of `skip` keys), netted per
  // (adsh, tag, version, uom).
  bool ApplyNumDeltas(const SecEdgarQ4TxnBuffer &buffer,
                      const std::unordered_set<std::string> *skip);

  bool StructureChanged() const {
    return mut_cohort_ || mut_tdict_ || mut_tags_ || mut_pre_ || mut_pre_over_ || mut_labels_;
  }
  SecEdgarQ4State Finish();

private:
  using Shard = SecEdgarQ4GroupShard;

  const SecEdgarQ4Cohort &Coh() const { return mut_cohort_ ? *mut_cohort_ : *base_.cohort; }
  const SecEdgarQ4TagDicts &TD() const { return mut_tdict_ ? *mut_tdict_ : *bridge_->tdict; }
  const SecEdgarQ4TagMap &TM() const { return mut_tags_ ? *mut_tags_ : *bridge_->tags; }
  const SecEdgarQ4PreMap &Pre() const { return mut_pre_ ? *mut_pre_ : *bridge_->pre; }
  const SecEdgarQ4PreOverflowMap &PreOver() const {
    return mut_pre_over_ ? *mut_pre_over_ : *bridge_->pre_over;
  }
  const SecEdgarQ4Sud &Sud() const { return mut_sud_ ? *mut_sud_ : *groups_->sud; }
  const SecEdgarQ4Labels &Labels() const { return mut_labels_ ? *mut_labels_ : *base_.labels; }
  const SecEdgarQ4GroupIndex &GIndex() const { return mut_gindex_ ? *mut_gindex_ : *groups_->gindex; }
  const SecEdgarQ4PairMap &Pairs() const { return mut_pairs_ ? *mut_pairs_ : *groups_->pairs; }
  const core::FlatHashMap<uint32_t> &SuSlot() const {
    return mut_su_slot_ ? *mut_su_slot_ : *groups_->su_slot;
  }

  SecEdgarQ4Cohort &MutCohort();
  SecEdgarQ4TagDicts &MutTD();
  SecEdgarQ4TagMap &MutTM();
  SecEdgarQ4PreMap &MutPre();
  SecEdgarQ4PreOverflowMap &MutPreOver();
  SecEdgarQ4Sud &MutSud();
  SecEdgarQ4Labels &MutLabels();
  SecEdgarQ4GroupIndex &MutGIndex();
  SecEdgarQ4PairMap &MutPairs();
  SecEdgarQ4SuGroups &MutSu(uint32_t slot);
  SecEdgarQ4ShardChunk &MutChunk(SecEdgarQ4SuGroups &sg, uint32_t shard_index);
  Shard &MutShard(uint64_t gid);

  // Existing group id, or a new one when `create`; kNoGroup otherwise.
  static constexpr uint64_t kNoGroup = ~uint64_t{0};
  uint64_t GroupFor(uint32_t sic_id, uint32_t tl, uint32_t stmt, uint32_t uom, bool create);
  uint64_t NewGroup(int64_t key, uint32_t sic_id, uint32_t tl, uint32_t stmt, uint32_t uom);
  // group += mult * (hi + lo), count += rows; rows joined rows for the cik.
  bool Contribute(uint64_t gid, uint32_t cik_id, double hi, double lo, double mult, int64_t rows);
  bool ErasePre(int64_t pk);
  bool PutPre(int64_t pk, uint32_t tl, const std::vector<std::pair<uint32_t, uint32_t>> &stmts);
  uint32_t EnsureAdsh(std::string_view adsh);

  SecEdgarQ4State base_;
  const SecEdgarQ4Bridge *bridge_;
  const SecEdgarQ4Groups *groups_;

  std::shared_ptr<SecEdgarQ4Cohort> mut_cohort_;
  std::shared_ptr<SecEdgarQ4TagDicts> mut_tdict_;
  std::shared_ptr<SecEdgarQ4TagMap> mut_tags_;
  std::shared_ptr<SecEdgarQ4PreMap> mut_pre_;
  std::shared_ptr<SecEdgarQ4PreOverflowMap> mut_pre_over_;
  std::shared_ptr<SecEdgarQ4Sud> mut_sud_;
  std::shared_ptr<SecEdgarQ4Labels> mut_labels_;
  std::shared_ptr<SecEdgarQ4GroupIndex> mut_gindex_;
  std::shared_ptr<SecEdgarQ4PairMap> mut_pairs_;
  std::shared_ptr<core::FlatHashMap<uint32_t>> mut_su_slot_;
  std::vector<std::shared_ptr<SecEdgarQ4SuGroups>> slots_;
  std::vector<uint8_t> own_slot_;
};

// Full build from the base tables (single pass over pre and num).
//
// Input order: AddSub*, AddTag*, then BeginPre + AddPre* (ids), then AddNum*
// (ids), then Finish.  Alternatively AddBridgeRow* (pre-joined rows, used by
// engines that run the join in the database) after AddSub/AddTag or alone.
class Builder {
public:
  Builder();
  ~Builder();

  void AddSub(std::string_view adsh, bool cik_null, int32_t cik, int32_t sic); // sic IS NOT NULL
  void AddTag(std::string_view tag, std::string_view version, bool tlabel_null,
              std::string_view tlabel); // abstract = 0
  void ReserveTags(size_t rows);
  // A batch of tag rows with abstract = 0 (label_null[i] != 0: tlabel IS NULL).
  void AddTags(const std::string_view *tag, const std::string_view *ver,
               const std::string_view *label, const uint8_t *label_null, size_t n);

  // String -> id resolution for the id-based inputs (kQ4None: cannot join).
  uint32_t AdshId(const char *p, size_t n) const;
  uint32_t TagId(const char *p, size_t n) const;
  uint32_t VerId(const char *p, size_t n) const;
  void AdshIds(const std::string_view *s, size_t n, uint32_t *out) const;
  void TagIds(const std::string_view *s, size_t n, uint32_t *out) const;
  void VerIds(const std::string_view *s, size_t n, uint32_t *out) const;
  uint32_t StmtId(const char *p, size_t n); // inserts
  uint32_t UomId(const char *p, size_t n);  // inserts

  void BeginPre(size_t expected_rows);
  // One pre row per index (stmt IS NOT NULL); rows with a kQ4None id are skipped.
  void AddPre(const uint32_t *a, const uint32_t *t, const uint32_t *v, const uint32_t *s, size_t n);
  // One num row per index (value IS NOT NULL); rows with a kQ4None id are skipped.
  void AddNum(const uint32_t *a, const uint32_t *t, const uint32_t *v, const uint32_t *u,
              const double *x, size_t n);
  void AddBridgeRow(const SecEdgarQ4BridgeRow &row);

  SecEdgarQ4State Finish();

  struct Impl;

private:
  std::unique_ptr<Impl> impl_;
};

std::shared_ptr<const SecEdgarQ4Result>
ComputeResult(const SecEdgarQ4Groups &groups, const SecEdgarQ4Params &params);

// Cached ComputeResult (per published version).
std::shared_ptr<const SecEdgarQ4Result>
CachedResult(const SecEdgarQ4Groups &groups, const SecEdgarQ4Params &params);

} // namespace accelerator_codegen::aux_state::query::q4
