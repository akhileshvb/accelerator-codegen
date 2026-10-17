#pragma once
// Stages one commit's row ops over the published Q16 pages: only the pages an
// op touches are copied, published pages are never mutated, and the modified
// pages come out as the commit's state images.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "tpch/q16/duckdb/aux/module.hpp"

namespace accelerator_codegen::aux_state::query {

// Page geometry, group codes and name ranks shared by the build, the read path
// and the stager.
namespace q16_pages {

using Layout = TpchQ16Layout;

constexpr uint16_t kNoBrand = 0xFFFF;
// Distinct type names a type id (16 bits) can hold.
constexpr std::size_t kTypeCap = 0xFFFF;
constexpr std::size_t kPartPageSize = std::size_t{1} << Layout::kPartShift;
constexpr std::size_t kPsPageSize = std::size_t{1} << Layout::kPsShift;
constexpr std::size_t kSuppPageSize = std::size_t{1} << Layout::kSuppShift;

inline bool SizeInRange(int32_t size) {
  return size >= Layout::kMinSize && size <= Layout::kMaxSize;
}

inline uint32_t GroupCode(int32_t size, uint32_t brand, uint32_t type) {
  return (static_cast<uint32_t>(size) << 24) | (brand << 16) | type;
}
inline int32_t CodeSize(uint32_t code) { return static_cast<int32_t>(code >> 24); }
inline uint32_t CodeBrand(uint32_t code) { return (code >> 16) & 0xFFu; }
inline uint16_t CodeType(uint32_t code) { return static_cast<uint16_t>(code & 0xFFFFu); }

inline void CheckKey(int64_t key, const char *what) {
  if (!Layout::KeyInRange(key))
    throw std::runtime_error(std::string("TPC-H Q16: ") + what +
                             " outside the supported key range");
}

template <class T>
core::StateImage MakeImage(core::PartitionId partition, std::shared_ptr<const T> payload) {
  return core::StateImage::Make<T>(Layout::kAccelerator, partition, 0, std::move(payload));
}

// Id of `name`, assigned in arrival order; throws past `cap` distinct names.
inline uint16_t InternName(std::vector<std::string> &names,
                           std::unordered_map<std::string, uint16_t> &index,
                           const std::string &name, std::size_t cap) {
  const auto it = index.find(name);
  if (it != index.end())
    return it->second;
  if (names.size() >= cap)
    throw std::runtime_error("TPC-H Q16: too many distinct brands/types");
  const auto id = static_cast<uint16_t>(names.size());
  names.push_back(name);
  index.emplace(name, id);
  return id;
}

inline void ComputeRanks(const std::vector<std::string> &names, std::vector<uint16_t> &rank,
                         std::vector<uint16_t> &by_rank) {
  std::vector<uint16_t> order(names.size());
  for (std::size_t i = 0; i < order.size(); ++i)
    order[i] = static_cast<uint16_t>(i);
  std::sort(order.begin(), order.end(), [&](uint16_t a, uint16_t b) { return names[a] < names[b]; });
  rank.assign(names.size(), 0);
  for (std::size_t i = 0; i < order.size(); ++i)
    rank[order[i]] = static_cast<uint16_t>(i);
  by_rank = std::move(order);
}

inline void RecomputeRanks(TpchQ16Meta &meta) {
  ComputeRanks(meta.brands, meta.brand_rank, meta.brand_by_rank);
  ComputeRanks(meta.types, meta.type_rank, meta.type_by_rank);
}

// ORDER BY supplier_cnt DESC, p_brand, p_type, p_size. New brands/types keep
// the relative order of the existing ones, so stored lists stay sorted.
struct RowLess {
  const TpchQ16Meta &meta;
  bool operator()(const Q16PrecomputedRow &a, const Q16PrecomputedRow &b) const {
    if (a.count != b.count)
      return a.count > b.count;
    if (a.brand_id != b.brand_id)
      return meta.brand_rank[a.brand_id] < meta.brand_rank[b.brand_id];
    if (a.type_id != b.type_id)
      return meta.type_rank[a.type_id] < meta.type_rank[b.type_id];
    return a.size < b.size;
  }
};

} // namespace q16_pages

class TpchQ16Stager {
public:
  explicit TpchQ16Stager(core::AuxEngineServices &services)
      : services(services), visibility(services.CurrentVisibility()) {}
  bool Open();
  void Apply(const TpchQ16Op &op);

  // Applies a large commit: the page edits in arrival order, logging the group
  // membership changes they imply; then the groups and the suppliers'
  // memberships are updated page by page on the pool (every page has one
  // owner). False: the pages are inconsistent with the ops (rebuild).
  bool ApplyBatch(const std::vector<TpchQ16Op> &ops);

  // Re-sorts the per-size lists for every group whose count changed and
  // appends all modified pages to `out`.
  void Finish(std::vector<core::StateImage> &out);

private:
  template <class T> struct PageSet {
    struct Entry {
      std::shared_ptr<const T> published;
      std::shared_ptr<T> mutated;
    };
    std::unordered_map<core::PartitionId, Entry> entries;
  };

  // One more (delta > 0) or fewer partsupp row of `suppkey` in a group.
  struct MemberEvent {
    uint32_t code;
    int64_t suppkey;
    int32_t delta;
  };
  // A supplier that entered (+1) or left (-1) a group.
  struct Membership {
    int64_t suppkey;
    uint32_t code;
    int8_t delta;
  };

  template <class T> std::shared_ptr<const T> Fetch(core::PartitionId id);
  template <class T> typename PageSet<T>::Entry &EntryOf(PageSet<T> &set, core::PartitionId id);
  template <class T> const T *Peek(PageSet<T> &set, core::PartitionId id);
  template <class T, class Init>
  T &Mut(PageSet<T> &set, core::PartitionId id, Init init);
  template <class T> void Collect(PageSet<T> &set, std::vector<core::StateImage> &out);

  const TpchQ16Meta &Meta() const { return meta_mut ? *meta_mut : *meta_pub; }
  TpchQ16Meta &MutableMeta();
  uint16_t Brand(const std::string &name);
  uint16_t Type(const std::string &name);

  // Pages
  TpchQ16PartInfo InfoOf(int64_t partkey);
  void SetInfo(int64_t partkey, const TpchQ16PartInfo &info);
  bool Eligible(int64_t suppkey);
  TpchQ16SupplierPage &MutSupp(int64_t suppkey);
  TpchQ16PartsuppPage &MutPs(int64_t partkey);
  const TpchQ16GroupMembers *PeekGroup(uint32_t code);
  TpchQ16GroupMembers *FindGroup(uint32_t code, bool create);
  void Touch(uint32_t code, const TpchQ16GroupMembers &gm);
  // One more (delta > 0) or one fewer partsupp row of `suppkey` in the group.
  void GroupAdd(uint32_t code, int64_t suppkey, int delta);
  static bool InGroup(const TpchQ16PartInfo &info);
  static uint32_t CodeOf(const TpchQ16PartInfo &info);

  // Ops
  void RemovePart(int64_t partkey, const TpchQ16PartInfo &info);
  void PartInsert(const TpchQ16Op &op);
  void PartDelete(int64_t partkey);
  void PartsuppInsert(int64_t partkey, int64_t suppkey);
  void PartsuppDelete(int64_t partkey, int64_t suppkey);
  void SupplierSet(int64_t suppkey, bool eligible);

  // Batch: the work of a large commit, page by page
  // Opens every page of `ids` (store reads, serial) and copies the published
  // ones for writing (parallel).
  template <class T, class Init>
  void MutAll(PageSet<T> &set, const std::vector<core::PartitionId> &ids, Init init);
  // Page lookups for pool tasks: the pages were opened before they started.
  template <class T> T &OpenedPage(PageSet<T> &set, core::PartitionId id);
  bool EligibleIn(const std::shared_ptr<const TpchQ16SupplierPage> &page, int64_t suppkey) const;
  bool EligiblePublished(int64_t suppkey) const;
  bool EligibleNow(int64_t suppkey) const;
  // A supplier whose eligibility changed moves the eligible count of every
  // group it was in before the commit (the groups it gained come with events).
  void AddFlipEvents();
  // Merges the net events of one group (sorted by supplier) into its members;
  // false when a ref count would go negative.
  bool MergeGroup(TpchQ16GroupMembers &gm, uint32_t code, const MemberEvent *first,
                  const MemberEvent *last, std::vector<Membership> &changes) const;
  // Applies the logged events to the groups, one task per group page, and
  // returns the membership changes in group order.
  bool ApplyGroupEvents(std::vector<Membership> &changes);
  // Records the membership changes in the suppliers' group lists, one task
  // per supplier page.
  bool ApplySupplierMemberships(std::vector<Membership> changes);
  // Re-sorts the per-size lists of the groups whose count changed; sizes are
  // independent lists, one task each.
  void ResortLists();

  core::AuxEngineServices &services;
  core::VisibilityToken visibility;
  std::shared_ptr<const TpchQ16Meta> meta_pub;
  std::shared_ptr<TpchQ16Meta> meta_mut;
  PageSet<TpchQ16SizeList> lists;
  PageSet<TpchQ16GroupPage> groups;
  PageSet<TpchQ16PartPage> parts;
  PageSet<TpchQ16PartsuppPage> ps;
  PageSet<TpchQ16SupplierPage> supp;
  // group code -> eligible count before this commit first changed it
  std::unordered_map<uint32_t, int32_t> touched;
  // Batch mode: group changes are logged instead of applied.
  bool batching = false;
  std::vector<MemberEvent> events;
  std::vector<int64_t> flipped; // suppliers whose eligibility a SupplierSet op changed
};

} // namespace accelerator_codegen::aux_state::query
