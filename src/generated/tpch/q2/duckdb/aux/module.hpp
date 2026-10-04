#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"

namespace accelerator_codegen::aux_state::query {

struct TpchQ2Config {
  core::TableId part_table = 0;
  core::TableId supplier_table = 0;
  core::TableId partsupp_table = 0;
  core::TableId nation_table = 0;
  core::TableId region_table = 0;
};

struct TpchQ2PartInput {
  int64_t key = 0;
  std::string mfgr;
  std::string type;
  int32_t size = 0;
};

struct TpchQ2SupplierInput {
  int64_t key = 0;
  std::string name;
  std::string address;
  int32_t nationkey = 0;
  std::string phone;
  int64_t acctbal_raw = 0;
  std::string comment;
};

struct TpchQ2PartsuppInput {
  int64_t partkey = 0;
  int64_t suppkey = 0;
  int64_t supplycost_raw = 0;
};

struct TpchQ2NationInput {
  int32_t key = 0;
  std::string name;
  int32_t regionkey = 0;
};

struct TpchQ2RegionInput {
  int32_t key = 0;
  std::string name;
};

class TpchQ2InitialScan {
public:
  virtual ~TpchQ2InitialScan() = default;
  virtual void ScanParts(
      const std::function<void(const TpchQ2PartInput &)> &) = 0;
  virtual void ScanSuppliers(
      const std::function<void(const TpchQ2SupplierInput &)> &) = 0;
  virtual void ScanPartsupps(
      const std::function<void(const TpchQ2PartsuppInput &)> &) = 0;
  virtual void ScanNations(
      const std::function<void(const TpchQ2NationInput &)> &) = 0;
  virtual void ScanRegions(
      const std::function<void(const TpchQ2RegionInput &)> &) = 0;
};

struct TpchQ2SupplierInfo {
  std::string name;
  std::string address;
  int32_t nationkey = 0;
  std::string phone;
  int64_t acctbal_raw = 0;
  std::string comment;
  bool valid = false;
};

struct TpchQ2PartInfo {
  std::string mfgr;
  int32_t type_id = 0; // index into TpchQ2State::types
  int32_t size = 0;
  bool valid = false;
};

// One partsupp row of a part (the offer of one supplier).  Offers are stored
// per part independent of whether the supplier / part row exists yet; the
// derived candidate lists only count offers whose supplier joins.
struct TpchQ2Offer {
  int64_t cost_raw = 0;
  int32_t suppkey = 0;
};

// Interned p_type strings.  Immutable once published; a write that introduces
// a new type copies the (tiny) dictionary.
struct TpchQ2TypeDict {
  std::vector<std::string> names;
  std::unordered_map<std::string, uint32_t> index;
  uint32_t Intern(const std::string &s) {
    const auto it = index.find(s);
    if (it != index.end())
      return it->second;
    const auto id = static_cast<uint32_t>(names.size());
    names.push_back(s);
    index.emplace(s, id);
    return id;
  }
};

// Copy-on-write paged dense vector.  Pages are shared between state versions
// through shared_ptr; Mut() clones a page only if it is still shared with a
// published version (use_count() != 1).  Mut() must only be used on a
// working (unpublished) copy that is held by a single thread; published
// images are never mutated.
template <typename T, unsigned kBits> class TpchQ2Paged {
public:
  static constexpr std::size_t kPage = std::size_t{1} << kBits;
  using Page = std::vector<T>;

  std::size_t Capacity() const { return pages_.size() << kBits; }

  const T *Find(std::size_t i) const {
    const std::size_t p = i >> kBits;
    if (p >= pages_.size())
      return nullptr;
    return &(*pages_[p])[i & (kPage - 1)];
  }

  T &Mut(std::size_t i) {
    const std::size_t p = i >> kBits;
    while (pages_.size() <= p)
      pages_.push_back(std::make_shared<const Page>(kPage));
    if (pages_[p].use_count() != 1)
      pages_[p] = std::make_shared<const Page>(*pages_[p]);
    return const_cast<Page &>(*pages_[p])[i & (kPage - 1)];
  }

private:
  std::vector<std::shared_ptr<const Page>> pages_;
};

// Clone-if-shared helper for shared_ptr<const X> members of a working state.
template <typename X> X &TpchQ2MutOf(std::shared_ptr<const X> &p) {
  if (!p)
    p = std::make_shared<const X>();
  else if (p.use_count() != 1)
    p = std::make_shared<const X>(*p);
  return const_cast<X &>(*p);
}

// One candidate output row: the (part, min-cost supplier) pair of one region.
struct TpchQ2Entry {
  int64_t acctbal_raw = 0;
  int32_t partkey = 0;
  int32_t suppkey = 0;
  int32_t type_id = 0;
  int32_t nation_rank = 0; // rank of n_name among nation names
};

struct TpchQ2Chunk {
  std::vector<TpchQ2Entry> v; // sorted, never empty inside a list
};

// Candidates of one (p_size, region) in final output order
// (s_acctbal desc, n_name, s_name, p_partkey, s_suppkey).
struct TpchQ2GroupList {
  std::vector<std::shared_ptr<const TpchQ2Chunk>> chunks;
};

using TpchQ2GroupMap =
    std::unordered_map<int64_t, std::shared_ptr<const TpchQ2GroupList>>;

inline int64_t TpchQ2GroupKey(int32_t size, int32_t region_key) {
  return (static_cast<int64_t>(static_cast<uint32_t>(size)) << 32) |
         static_cast<int64_t>(static_cast<uint32_t>(region_key));
}

struct TpchQ2State {
  // Small immutable dimensions.
  std::shared_ptr<const std::vector<std::string>> region_names;
  std::shared_ptr<const std::vector<std::string>> nation_names;
  std::shared_ptr<const std::vector<int32_t>> nation_region;
  std::shared_ptr<const std::vector<int32_t>> nation_rank;

  // Large dimensions / facts, copy-on-write at page granularity.
  std::shared_ptr<const TpchQ2Paged<TpchQ2SupplierInfo, 6>> suppliers;
  std::shared_ptr<const TpchQ2Paged<TpchQ2PartInfo, 10>> parts;
  std::shared_ptr<const TpchQ2Paged<std::vector<TpchQ2Offer>, 9>> offers;
  // supplier -> partkeys it offers (one element per partsupp row).
  std::shared_ptr<const TpchQ2Paged<std::vector<int32_t>, 6>> supp_parts;
  std::shared_ptr<const TpchQ2TypeDict> types;

  // Derived: per (size, region) sorted candidate lists, chunked COW.
  std::shared_ptr<const TpchQ2GroupMap> groups;

  const TpchQ2SupplierInfo *FindSupplier(int64_t suppkey) const {
    if (!suppliers || suppkey < 0)
      return nullptr;
    const auto *s = suppliers->Find(static_cast<std::size_t>(suppkey));
    return s && s->valid ? s : nullptr;
  }

  const TpchQ2PartInfo *FindPart(int64_t partkey) const {
    if (!parts || partkey < 0)
      return nullptr;
    const auto *p = parts->Find(static_cast<std::size_t>(partkey));
    return p && p->valid ? p : nullptr;
  }

  const std::vector<TpchQ2Offer> *FindOffers(int64_t partkey) const {
    if (!offers || partkey < 0)
      return nullptr;
    return offers->Find(static_cast<std::size_t>(partkey));
  }

  const TpchQ2GroupList *FindGroup(int32_t size, int32_t region_key) const {
    if (!groups)
      return nullptr;
    const auto it = groups->find(TpchQ2GroupKey(size, region_key));
    return it == groups->end() ? nullptr : it->second.get();
  }
};

// Pre-parsed LIKE pattern accelerator for fast type matching.
struct TpchQ2LikeAccelerator {
  bool enabled = false;
  bool prefix_wild = false;
  bool suffix_wild = false;
  std::string pattern;
};

TpchQ2LikeAccelerator TpchQ2BuildLikeAccelerator(const std::string &pattern);
bool TpchQ2LikeAcceleratedMatch(const TpchQ2LikeAccelerator &acc,
                                const std::string &text);

enum class TpchQ2ImageKind : uint8_t { Full };

struct TpchQ2StateImage {
  TpchQ2ImageKind kind = TpchQ2ImageKind::Full;
  std::shared_ptr<const TpchQ2State> state;
};

struct TpchQ2PartsuppDelta {
  int64_t partkey = 0;
  int64_t suppkey = 0;
  int64_t supplycost_raw = 0;
  int8_t sign = 0;
};

// One captured change, applied in arrival order at StageMaintenance.
struct TpchQ2Op {
  enum class Kind : uint8_t {
    Partsupp,
    SupplierSet,
    SupplierErase,
    PartSet,
    PartErase
  };
  Kind kind = Kind::Partsupp;
  TpchQ2PartsuppDelta partsupp;
  int64_t key = 0; // supplier / part key
  TpchQ2SupplierInfo supplier;
  TpchQ2PartInput part;
};

std::vector<uint8_t> EncodeTpchQ2State(const TpchQ2State &);
bool DecodeTpchQ2State(const std::vector<uint8_t> &, TpchQ2State &);

bool TpchQ2MatchesLike(const std::string &text, const std::string &pattern);

void TpchQ2FinalizeAndEmit(const TpchQ2State &state, int32_t part_size,
                           const std::string &part_type_pattern,
                           const std::string &region_name,
                           core::AuxRowSink &sink);

constexpr std::size_t kTpchQ2ResultLimit = 100;

class TpchQ2TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ2Op> &Ops() const;
  void MarkRebuild();
  void AddOp(TpchQ2Op op);

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ2Op> ops;
};

class TpchQ2Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = "tpch_q2";

  explicit TpchQ2Module(TpchQ2Config config,
                        TpchQ2InitialScan *initial_scan = nullptr)
      : config(config), initial_scan(initial_scan) {}

  core::AcceleratorId Id() const override;
  std::vector<core::TableDependency> Dependencies() const override;
  std::unique_ptr<core::AuxTxnBuffer> CreateTxnBuffer() const override;
  void Build(core::AuxEngineServices &, core::StateBuilder &) override;
  core::InvalidationPlan
  InterpretMutation(const core::MutationBatch &) const override;
  void AccumulateMutation(const core::MutationBatch &, core::AuxEngineServices &,
                          core::AuxTxnBuffer &) override;
  core::StagedMaintenanceOutcome
  StageMaintenance(core::AuxEngineServices &, const core::InvalidationPlan &,
                   core::AuxTxnBuffer &) override;
  bool CheckValidity(core::AuxEngineServices &,
                     const core::QueryArguments &) const override;
  void Execute(core::AuxEngineServices &, const core::QueryArguments &,
               core::AuxRowSink &) override;
  void ExecuteFallback(core::AuxEngineServices &, const core::QueryArguments &,
                       core::AuxRowSink &) override;
  std::vector<core::CalibrationWrite> CalibrationWrites() const override;

private:
  TpchQ2State BuildState(core::AuxEngineServices &) const;
  std::shared_ptr<const TpchQ2State>
  LoadState(core::AuxEngineServices &) const;
  void EmitResult(const TpchQ2State &, int32_t part_size,
                  const std::string &part_type_pattern,
                  const std::string &region_name, core::AuxRowSink &) const;

  TpchQ2Config config;
  TpchQ2InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
