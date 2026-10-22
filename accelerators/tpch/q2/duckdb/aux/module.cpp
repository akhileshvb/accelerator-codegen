#include "tpch/q2/duckdb/aux/module.hpp"
#include "tpch/q2/calibration_writes.hpp"
#include "prims/row_pieces.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/parallel_sort.hpp"
#include "substrate/aux/core/partitioned_build.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
#include "tpch/q2/layout.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {

// Ops above which a commit is applied as one batch (untimed).
constexpr std::size_t kBatchOpThreshold = 1024;
// Parts per slice of the maintenance entry diff (~1500 us).
constexpr std::size_t kEntryGrain = 512;
// Entries per slice of the sorted maintenance diff (untimed).
constexpr std::size_t kSortGrain = 16384;
// Groups per slice of list edits (untimed).
constexpr std::size_t kGroupGrain = 4;
// Part slots per slice of the initial entry computation (~65000 us).
constexpr std::size_t kPartGrain = 65536;
// Candidate lists per slice of the initial list build (untimed): each list is sorted.
constexpr std::size_t kListGrain = 1;

// Entries per chunk of a candidate list when built, and the size that splits it.
constexpr std::size_t kChunkTarget = 256;
constexpr std::size_t kChunkMax = 512;
constexpr int64_t kMaxFlatKey = 1'000'000;

bool StringValue(const core::AuxValue &value, std::string &out) {
  if (value.is_null || !std::holds_alternative<std::string>(value.payload))
    return false;
  out = std::get<std::string>(value.payload);
  return true;
}

bool ReadPartsupp(const core::AuxRow &row, int64_t &partkey, int64_t &suppkey,
                  int64_t &supplycost_raw) {
  int32_t scale = 0;
  return core::IntCell(row, 0, partkey) && core::IntCell(row, 1, suppkey) &&
         core::DecimalCell(row, 2, supplycost_raw, scale);
}

template <typename T>
void EnsureFlatSize(std::vector<T> &vector, std::size_t required) {
  if (required > vector.size())
    vector.resize(required);
}

// Part and supplier keys index chunked copy-on-write arrays and are stored as
// int32 in offers and groups; nation and region keys index flat vectors. A key
// outside its range would be silently wrong, so it stops the build or the
// commit (the runtime then rebuilds or invalidates).
bool AcceptDenseKey(int64_t key) {
  if (key < 0 || key > std::numeric_limits<int32_t>::max())
    throw std::out_of_range("TPC-H Q2 key outside the int32 range: " +
                            std::to_string(key));
  return true;
}

bool AcceptFlatKey(int64_t key) {
  if (key < 0 || key > kMaxFlatKey)
    throw std::out_of_range("TPC-H Q2 nation/region key too large: " +
                            std::to_string(key));
  return true;
}

// Total order of candidate entries = final ORDER BY of Q2, plus suppkey as a
// last tie-break.  Supplier names are only consulted when the cheaper fields
// tie.  Callers keep the lists consistent with the supplier table the
// comparator reads (see Maintainer::ApplySupplier).
struct EntryLess {
  const TpchQ2State *st;
  bool operator()(const TpchQ2Entry &a, const TpchQ2Entry &b) const {
    if (a.acctbal_raw != b.acctbal_raw)
      return a.acctbal_raw > b.acctbal_raw;
    if (a.nation_rank != b.nation_rank)
      return a.nation_rank < b.nation_rank;
    if (a.suppkey != b.suppkey) {
      const auto *sa = st->FindSupplier(a.suppkey);
      const auto *sb = st->FindSupplier(b.suppkey);
      if (sa && sb) {
        const int c = sa->name.compare(sb->name);
        if (c != 0)
          return c < 0;
      }
    }
    if (a.partkey != b.partkey)
      return a.partkey < b.partkey;
    return a.suppkey < b.suppkey;
  }
};

struct GroupEntry {
  int64_t group = 0;
  TpchQ2Entry e;
};

bool GroupEntryEq(const GroupEntry &a, const GroupEntry &b) {
  return a.group == b.group && a.e.acctbal_raw == b.e.acctbal_raw &&
         a.e.partkey == b.e.partkey && a.e.suppkey == b.e.suppkey &&
         a.e.type_id == b.e.type_id && a.e.nation_rank == b.e.nation_rank;
}

// Canonical order used only to diff two small multisets.
bool GroupEntryCanon(const GroupEntry &a, const GroupEntry &b) {
  if (a.group != b.group)
    return a.group < b.group;
  if (a.e.partkey != b.e.partkey)
    return a.e.partkey < b.e.partkey;
  if (a.e.suppkey != b.e.suppkey)
    return a.e.suppkey < b.e.suppkey;
  if (a.e.acctbal_raw != b.e.acctbal_raw)
    return a.e.acctbal_raw < b.e.acctbal_raw;
  if (a.e.type_id != b.e.type_id)
    return a.e.type_id < b.e.type_id;
  return a.e.nation_rank < b.e.nation_rank;
}

bool NationValid(const TpchQ2State &st, int64_t nk) {
  return nk >= 0 && static_cast<std::size_t>(nk) < st.nation_names->size() &&
         static_cast<std::size_t>(nk) < st.nation_region->size() &&
         !(*st.nation_names)[static_cast<std::size_t>(nk)].empty();
}

// The candidate entries of one part: for every region, each live offer whose
// cost equals the minimum cost of the part's offers in that region (ties give
// several entries, duplicate partsupp rows give duplicate entries -- bag
// semantics).  Pure function of (part, offers, suppliers, nations).
void ComputeEntries(const TpchQ2State &st, int64_t partkey,
                    std::vector<GroupEntry> &out) {
  const auto *part = st.FindPart(partkey);
  if (!part)
    return;
  const auto *offs = st.FindOffers(partkey);
  if (!offs || offs->empty())
    return;
  struct Resolved {
    const TpchQ2SupplierInfo *supplier;
    int32_t region;
    int32_t rank;
    int64_t cost;
    int32_t suppkey;
  };
  thread_local std::vector<Resolved> scratch;
  scratch.clear();
  for (const auto &o : *offs) {
    const auto *s = st.FindSupplier(o.suppkey);
    if (!s || !NationValid(st, s->nationkey))
      continue;
    const auto nk = static_cast<std::size_t>(s->nationkey);
    scratch.push_back({s, (*st.nation_region)[nk], (*st.nation_rank)[nk],
                       o.cost_raw, o.suppkey});
  }
  for (std::size_t i = 0; i < scratch.size(); ++i) {
    const auto &r = scratch[i];
    bool is_min = true;
    for (std::size_t j = 0; j < scratch.size(); ++j) {
      if (scratch[j].region == r.region && scratch[j].cost < r.cost) {
        is_min = false;
        break;
      }
    }
    if (!is_min)
      continue;
    GroupEntry ge;
    ge.group = TpchQ2GroupKey(part->size, r.region);
    ge.e.acctbal_raw = r.supplier->acctbal_raw;
    ge.e.partkey = static_cast<int32_t>(partkey);
    ge.e.suppkey = r.suppkey;
    ge.e.type_id = part->type_id;
    ge.e.nation_rank = r.rank;
    out.push_back(ge);
  }
}

void BuildNationRank(TpchQ2State &st) {
  const auto &names = *st.nation_names;
  std::vector<std::string> sorted(names.begin(), names.end());
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  auto rank = std::make_shared<std::vector<int32_t>>(names.size(), 0);
  for (std::size_t i = 0; i < names.size(); ++i)
    (*rank)[i] = static_cast<int32_t>(
        std::lower_bound(sorted.begin(), sorted.end(), names[i]) -
        sorted.begin());
  st.nation_rank = std::move(rank);
}

void AddOfferRaw(TpchQ2State &st, int64_t partkey, int64_t suppkey,
                 int64_t cost) {
  TpchQ2MutOf(st.offers).Mutable(partkey)
      .push_back({cost, static_cast<int32_t>(suppkey)});
  TpchQ2MutOf(st.supp_parts).Mutable(suppkey)
      .push_back(static_cast<int32_t>(partkey));
}

// One partsupp row as scanned.
struct PartsuppRow {
  int64_t partkey = 0;
  int64_t suppkey = 0;
  int64_t cost_raw = 0;
};

// Scanned rows of the part and supplier tables; `type` / `index` of a part is the
// piece's own p_type numbering until the pieces are unified.
struct PartRow {
  int64_t key = 0;
  std::string mfgr;
  int32_t type = 0;
  int32_t size = 0;
};

using GroupAcc = std::unordered_map<int64_t, std::vector<TpchQ2Entry>>;

// The candidate entries of every group, one map per slice of part slots.
std::vector<GroupAcc> ComputeGroupEntries(core::AuxEngineServices &services,
                                          const TpchQ2State &st) {
  const std::size_t capacity = TpchQ2Capacity(*st.parts);
  std::vector<GroupAcc> slices(core::SliceCount(services, capacity, kPartGrain));
  core::ParallelSlices(
      services, capacity, kPartGrain,
      [&](std::size_t slice, std::size_t begin, std::size_t end) {
        std::vector<GroupEntry> tmp;
        for (std::size_t k = begin; k < end; ++k) {
          const auto *p = st.parts->Find(static_cast<int64_t>(k));
          if (!p || !p->valid)
            continue;
          tmp.clear();
          ComputeEntries(st, static_cast<int64_t>(k), tmp);
          for (const auto &ge : tmp)
            slices[slice][ge.group].push_back(ge.e);
        }
      });
  return slices;
}

// Builds the sorted candidate lists from scratch (Build / Decode): the slices'
// entries of each group are joined in slice order and sorted, groups in parallel.
void BuildGroups(core::AuxEngineServices &services, TpchQ2State &st) {
  auto slices = ComputeGroupEntries(services, st);
  std::vector<int64_t> keys;
  for (const auto &slice : slices)
    for (const auto &kv : slice)
      keys.push_back(kv.first);
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  std::vector<std::shared_ptr<TpchQ2GroupList>> lists(keys.size());
  const EntryLess less{&st};
  core::ParallelEach(services, keys.size(), kListGrain, [&](std::size_t i) {
    std::vector<TpchQ2Entry> v;
    for (auto &slice : slices) {
      const auto it = slice.find(keys[i]);
      if (it == slice.end())
        continue;
      v.insert(v.end(), it->second.begin(), it->second.end());
      std::vector<TpchQ2Entry>().swap(it->second);
    }
    std::sort(v.begin(), v.end(), less);
    auto list = std::make_shared<TpchQ2GroupList>();
    list->chunks.reserve(v.size() / kChunkTarget + 1);
    for (std::size_t at = 0; at < v.size(); at += kChunkTarget) {
      auto chunk = std::make_shared<TpchQ2Chunk>();
      const std::size_t end = std::min(v.size(), at + kChunkTarget);
      chunk->v.assign(v.begin() + static_cast<std::ptrdiff_t>(at),
                      v.begin() + static_cast<std::ptrdiff_t>(end));
      list->chunks.push_back(std::move(chunk));
    }
    lists[i] = std::move(list);
  });
  auto groups = std::make_shared<TpchQ2GroupMap>();
  groups->reserve(keys.size());
  for (std::size_t i = 0; i < keys.size(); ++i)
    (*groups)[keys[i]] = std::move(lists[i]);
  st.groups = std::move(groups);
}

void InitEmptyState(TpchQ2State &st) {
  st.suppliers = std::make_shared<const TpchQ2Paged<TpchQ2SupplierInfo, 6>>();
  st.parts = std::make_shared<const TpchQ2Paged<TpchQ2PartInfo, 10>>();
  st.offers =
      std::make_shared<const TpchQ2Paged<std::vector<TpchQ2Offer>, 9>>();
  st.supp_parts =
      std::make_shared<const TpchQ2Paged<std::vector<int32_t>, 6>>();
  st.types = std::make_shared<const TpchQ2TypeDict>();
  st.groups = std::make_shared<const TpchQ2GroupMap>();
}

// first chunk whose last element is not less than e
std::size_t ChunkIndex(const TpchQ2GroupList &gl, const EntryLess &less,
                       const TpchQ2Entry &e) {
  return static_cast<std::size_t>(
      std::partition_point(gl.chunks.begin(), gl.chunks.end(),
                           [&](const std::shared_ptr<const TpchQ2Chunk> &c) {
                             return less(c->v.back(), e);
                           }) -
      gl.chunks.begin());
}

void InsertIntoList(TpchQ2GroupList &gl, const EntryLess &less,
                    const TpchQ2Entry &e) {
  if (gl.chunks.empty()) {
    auto c = std::make_shared<TpchQ2Chunk>();
    c->v.push_back(e);
    gl.chunks.push_back(std::move(c));
    return;
  }
  std::size_t idx = ChunkIndex(gl, less, e);
  if (idx == gl.chunks.size())
    idx = gl.chunks.size() - 1;
  auto &chunk = TpchQ2MutOf(gl.chunks[idx]);
  const auto pos = std::lower_bound(chunk.v.begin(), chunk.v.end(), e, less);
  chunk.v.insert(pos, e);
  if (chunk.v.size() > kChunkMax) {
    auto upper = std::make_shared<TpchQ2Chunk>();
    const auto mid = chunk.v.begin() +
                     static_cast<std::ptrdiff_t>(chunk.v.size() / 2);
    upper->v.assign(mid, chunk.v.end());
    chunk.v.erase(mid, chunk.v.end());
    gl.chunks.insert(gl.chunks.begin() + static_cast<std::ptrdiff_t>(idx) + 1,
                     std::move(upper));
  }
}

// False when the list holds no such entry.
bool RemoveFromList(TpchQ2GroupList &gl, const EntryLess &less,
                    const TpchQ2Entry &e) {
  const std::size_t idx = ChunkIndex(gl, less, e);
  if (idx >= gl.chunks.size())
    return false;
  const auto &found = gl.chunks[idx]->v;
  const auto at = std::lower_bound(found.begin(), found.end(), e, less) - found.begin();
  if (static_cast<std::size_t>(at) == found.size() || less(e, found[at]) ||
      found[at].partkey != e.partkey || found[at].suppkey != e.suppkey)
    return false;
  auto &chunk = TpchQ2MutOf(gl.chunks[idx]);
  chunk.v.erase(chunk.v.begin() + at);
  if (chunk.v.empty())
    gl.chunks.erase(gl.chunks.begin() + static_cast<std::ptrdiff_t>(idx));
  return true;
}

// `before \ after` into `rem`, `after \ before` into `add` (multiset
// difference of two GroupEntryCanon-sorted vectors; both outputs stay sorted).
// An entry of a `moved` supplier never matches: its sort key changed, so it
// must leave the list and come back.
template <class Moved>
void DiffEntries(const std::vector<GroupEntry> &before,
                 const std::vector<GroupEntry> &after, const Moved &moved,
                 std::vector<GroupEntry> &rem, std::vector<GroupEntry> &add) {
  std::size_t i = 0, j = 0;
  while (i < before.size() || j < after.size()) {
    if (i == before.size()) {
      add.push_back(after[j++]);
    } else if (j == after.size()) {
      rem.push_back(before[i++]);
    } else if (GroupEntryEq(before[i], after[j])) {
      if (moved(before[i].e.suppkey)) {
        rem.push_back(before[i]);
        add.push_back(after[j]);
      }
      ++i;
      ++j;
    } else if (GroupEntryCanon(before[i], after[j])) {
      rem.push_back(before[i++]);
    } else {
      add.push_back(after[j++]);
    }
  }
}

// Applies captured changes to a private working copy of a published state.
// Every shared structure is cloned on first write (page / chunk granularity),
// so the published image is never touched.  ok == false means the working
// copy is inconsistent and the caller must fall back to a rebuild.
class Maintainer {
public:
  explicit Maintainer(const TpchQ2State &old) : st(old) {}

  TpchQ2State st;
  bool ok = true;

  void Apply(const TpchQ2Op &op) {
    if (!ok)
      return;
    switch (op.kind) {
    case TpchQ2Op::Kind::Partsupp:
      ApplyPartsupp(op.partsupp);
      break;
    case TpchQ2Op::Kind::SupplierSet:
      ApplySupplier(op.key, &op.supplier);
      break;
    case TpchQ2Op::Kind::SupplierErase:
      ApplySupplier(op.key, nullptr);
      break;
    case TpchQ2Op::Kind::PartSet:
      ApplyPart(op.key, &op.part);
      break;
    case TpchQ2Op::Kind::PartErase:
      ApplyPart(op.key, nullptr);
      break;
    }
  }

private:
  void InsertEntry(int64_t group, const TpchQ2Entry &e) {
    InsertIntoList(TpchQ2MutOf(TpchQ2MutOf(st.groups)[group]), EntryLess{&st}, e);
  }

  void RemoveEntry(int64_t group, const TpchQ2Entry &e) {
    const auto git = st.groups->find(group);
    if (git == st.groups->end() ||
        !RemoveFromList(TpchQ2MutOf(TpchQ2MutOf(st.groups)[group]), EntryLess{&st}, e))
      ok = false;
  }

  // Removes `before \ after`, inserts `after \ before` (multiset difference).
  void ApplyDiff(std::vector<GroupEntry> &before,
                 std::vector<GroupEntry> &after) {
    std::sort(before.begin(), before.end(), GroupEntryCanon);
    std::sort(after.begin(), after.end(), GroupEntryCanon);
    std::vector<GroupEntry> rem, add;
    DiffEntries(before, after, [](int32_t) { return false; }, rem, add);
    for (const auto &g : rem) {
      RemoveEntry(g.group, g.e);
      if (!ok)
        return;
    }
    for (const auto &g : add)
      InsertEntry(g.group, g.e);
  }

  // False (and !ok) when a deleted offer was never seen.
  bool EditOffers(const TpchQ2PartsuppDelta &d) {
    if (!AcceptDenseKey(d.partkey) || !AcceptDenseKey(d.suppkey))
      return false;
    if (d.sign > 0) {
      AddOfferRaw(st, d.partkey, d.suppkey, d.supplycost_raw);
      return true;
    }
    auto &offs = TpchQ2MutOf(st.offers).Mutable(d.partkey);
    auto it = std::find_if(offs.begin(), offs.end(), [&](const TpchQ2Offer &o) {
      return o.suppkey == d.suppkey && o.cost_raw == d.supplycost_raw;
    });
    if (it == offs.end()) {
      ok = false;
      return false;
    }
    offs.erase(it);
    auto &sp = TpchQ2MutOf(st.supp_parts).Mutable(d.suppkey);
    const auto sit = std::find(sp.begin(), sp.end(), static_cast<int32_t>(d.partkey));
    if (sit != sp.end())
      sp.erase(sit);
    return true;
  }

  void EditPart(int64_t key, const TpchQ2PartInput *in) {
    auto &slot = TpchQ2MutOf(st.parts).Mutable(key);
    if (!in) {
      slot = TpchQ2PartInfo{};
      return;
    }
    slot.mfgr = in->mfgr;
    slot.type_id = static_cast<int32_t>(InternType(in->type));
    slot.size = in->size;
    slot.valid = true;
  }

  void EditSupplier(int64_t key, const TpchQ2SupplierInfo *in) {
    auto &slot = TpchQ2MutOf(st.suppliers).Mutable(key);
    if (in) {
      slot = *in;
      slot.valid = true;
    } else {
      slot = TpchQ2SupplierInfo{};
    }
  }

  // Whether the candidate lists depend on the change: only the output-only
  // fields (address/phone/comment) can change without touching them.
  bool SupplierMovesEntries(int64_t key, const TpchQ2SupplierInfo *in) const {
    const auto *cur = st.FindSupplier(key);
    if (!cur || !in)
      return cur || in;
    return cur->name != in->name || cur->nationkey != in->nationkey ||
           cur->acctbal_raw != in->acctbal_raw;
  }

  // Parts the supplier has offers for.
  std::vector<int32_t> PartsOf(int64_t suppkey) const {
    std::vector<int32_t> parts;
    if (const auto *sp = st.supp_parts->Find(static_cast<std::size_t>(suppkey)))
      parts = *sp;
    std::sort(parts.begin(), parts.end());
    parts.erase(std::unique(parts.begin(), parts.end()), parts.end());
    return parts;
  }

  void ApplyPartsupp(const TpchQ2PartsuppDelta &d) {
    if (!AcceptDenseKey(d.partkey) || !AcceptDenseKey(d.suppkey))
      return;
    std::vector<GroupEntry> before, after;
    ComputeEntries(st, d.partkey, before);
    if (!EditOffers(d))
      return;
    ComputeEntries(st, d.partkey, after);
    ApplyDiff(before, after);
  }

  void ApplyPart(int64_t key, const TpchQ2PartInput *in) {
    if (!AcceptDenseKey(key))
      return;
    if (!st.FindPart(key) && !in)
      return;
    std::vector<GroupEntry> before, after;
    ComputeEntries(st, key, before);
    EditPart(key, in);
    ComputeEntries(st, key, after);
    ApplyDiff(before, after);
  }

  uint32_t InternType(const std::string &t) {
    const auto it = st.types->index.find(t);
    if (it != st.types->index.end())
      return it->second;
    return TpchQ2MutOf(st.types).Intern(t);
  }

  void ApplySupplier(int64_t key, const TpchQ2SupplierInfo *in) {
    if (!AcceptDenseKey(key))
      return;
    if (!st.FindSupplier(key) && !in)
      return;
    if (!SupplierMovesEntries(key, in)) {
      EditSupplier(key, in);
      return;
    }
    const auto parts = PartsOf(key);
    std::vector<GroupEntry> before, after;
    for (const auto p : parts)
      ComputeEntries(st, p, before);
    // 1. remove this supplier's entries while the table still holds the old
    //    sort key (keeps the list consistent with the comparator).
    std::vector<GroupEntry> rest;
    for (const auto &g : before) {
      if (g.e.suppkey == key) {
        RemoveEntry(g.group, g.e);
        if (!ok)
          return;
      } else {
        rest.push_back(g);
      }
    }
    // 2. change the table.
    EditSupplier(key, in);
    // 3. recompute and diff the remaining entries against the new truth.
    for (const auto p : parts)
      ComputeEntries(st, p, after);
    ApplyDiff(rest, after);
  }

  // What a batch of table edits touched: parts whose candidate entries may
  // differ, and suppliers whose entries' sort key moved. Both sorted, unique.
  struct BatchEdits {
    std::vector<int64_t> parts;
    std::vector<int64_t> moved_suppliers;
  };

  BatchEdits EditTables(const std::vector<TpchQ2Op> &ops) {
    BatchEdits edits;
    for (const auto &op : ops) {
      switch (op.kind) {
      case TpchQ2Op::Kind::Partsupp:
        if (EditOffers(op.partsupp))
          edits.parts.push_back(op.partsupp.partkey);
        break;
      case TpchQ2Op::Kind::SupplierSet:
      case TpchQ2Op::Kind::SupplierErase: {
        const auto *in =
            op.kind == TpchQ2Op::Kind::SupplierSet ? &op.supplier : nullptr;
        if (AcceptDenseKey(op.key) && !st.FindSupplier(op.key) && !in)
          break;
        if (SupplierMovesEntries(op.key, in)) {
          const auto parts = PartsOf(op.key);
          edits.parts.insert(edits.parts.end(), parts.begin(), parts.end());
          edits.moved_suppliers.push_back(op.key);
        }
        EditSupplier(op.key, in);
        break;
      }
      case TpchQ2Op::Kind::PartSet:
      case TpchQ2Op::Kind::PartErase: {
        const auto *in = op.kind == TpchQ2Op::Kind::PartSet ? &op.part : nullptr;
        if (AcceptDenseKey(op.key) && !st.FindPart(op.key) && !in)
          break;
        edits.parts.push_back(op.key);
        EditPart(op.key, in);
        break;
      }
      }
      if (!ok)
        break;
    }
    for (auto *keys : {&edits.parts, &edits.moved_suppliers}) {
      std::sort(keys->begin(), keys->end());
      keys->erase(std::unique(keys->begin(), keys->end()), keys->end());
    }
    return edits;
  }

  // One group's share of a batch: remove rem[rem_begin, rem_end), then insert
  // add[add_begin, add_end), on the group's own list.
  struct GroupEdit {
    int64_t group = 0;
    std::size_t rem_begin = 0, rem_end = 0, add_begin = 0, add_end = 0;
    TpchQ2GroupList *list = nullptr;
  };

  static std::vector<GroupEdit> SplitByGroup(const std::vector<GroupEntry> &rem,
                                             const std::vector<GroupEntry> &add) {
    std::vector<GroupEdit> edits;
    std::size_t r = 0, a = 0;
    while (r < rem.size() || a < add.size()) {
      GroupEdit edit;
      edit.group = a == add.size() || (r < rem.size() && rem[r].group <= add[a].group)
                       ? rem[r].group
                       : add[a].group;
      edit.rem_begin = r;
      edit.add_begin = a;
      while (r < rem.size() && rem[r].group == edit.group)
        ++r;
      while (a < add.size() && add[a].group == edit.group)
        ++a;
      edit.rem_end = r;
      edit.add_end = a;
      edits.push_back(edit);
    }
    return edits;
  }

public:
  // Applies many ops at once: the table edits in arrival order, then the
  // candidate lists of the affected parts, one group per task (groups own
  // disjoint lists). `published` is the state the lists were built against: it
  // orders the removals, the edited tables order the insertions.
  void ApplyBatch(core::AuxEngineServices &services,
                  const std::vector<TpchQ2Op> &ops, const TpchQ2State &published) {
    const auto edits = EditTables(ops);
    if (!ok)
      return;
    struct Entries {
      std::vector<GroupEntry> before, after;
    } entries;
    core::ParallelReduce(
        services, edits.parts.size(), kEntryGrain, entries,
        [&](Entries &into, std::size_t begin, std::size_t end) {
          for (std::size_t i = begin; i < end; ++i) {
            ComputeEntries(published, edits.parts[i], into.before);
            ComputeEntries(st, edits.parts[i], into.after);
          }
        },
        [](Entries &into, Entries &&from) {
          into.before.insert(into.before.end(), from.before.begin(), from.before.end());
          into.after.insert(into.after.end(), from.after.begin(), from.after.end());
        });
    core::ParallelStableSort(services, entries.before, GroupEntryCanon, kSortGrain);
    core::ParallelStableSort(services, entries.after, GroupEntryCanon, kSortGrain);
    std::vector<GroupEntry> rem, add;
    DiffEntries(entries.before, entries.after,
                [&](int32_t suppkey) {
                  return std::binary_search(edits.moved_suppliers.begin(),
                                            edits.moved_suppliers.end(), suppkey);
                },
                rem, add);

    auto group_edits = SplitByGroup(rem, add);
    auto &lists = TpchQ2MutOf(st.groups);
    for (auto &edit : group_edits)
      edit.list = &TpchQ2MutOf(lists[edit.group]);
    std::atomic<bool> consistent{true};
    const EntryLess removal_order{&published}, insertion_order{&st};
    core::ParallelEach(services, group_edits.size(), kGroupGrain, [&](std::size_t i) {
      const auto &edit = group_edits[i];
      for (std::size_t r = edit.rem_begin; r < edit.rem_end; ++r)
        if (!RemoveFromList(*edit.list, removal_order, rem[r].e)) {
          consistent = false;
          return;
        }
      for (std::size_t a = edit.add_begin; a < edit.add_end; ++a)
        InsertIntoList(*edit.list, insertion_order, add[a].e);
    });
    ok = consistent;
  }
};

// Partsupp: a row that cannot be parsed has a NULL join/aggregate column and
// contributes nothing (neither joins nor feeds MIN), so it is skipped.
void AccumulatePartsupp(const core::MutationBatch &mutation, TpchQ2TxnBuffer &buffer) {
  auto add = [&](const core::AuxRow &row, int8_t sign) {
    int64_t partkey = 0, suppkey = 0, cost = 0;
    if (!ReadPartsupp(row, partkey, suppkey, cost))
      return;
    TpchQ2Op op;
    op.kind = TpchQ2Op::Kind::Partsupp;
    op.partsupp = {partkey, suppkey, cost, sign};
    buffer.AddOp(std::move(op));
  };
  switch (mutation.kind) {
  case core::MutationKind::Insert:
    for (const auto &row : mutation.new_rows)
      add(row, 1);
    break;
  case core::MutationKind::Delete:
    for (const auto &row : mutation.old_rows)
      add(row, -1);
    break;
  case core::MutationKind::Update:
    for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
      int64_t pk0, sk0, c0, pk1, sk1, c1;
      const bool o = ReadPartsupp(mutation.old_rows[i], pk0, sk0, c0);
      const bool n = ReadPartsupp(mutation.new_rows[i], pk1, sk1, c1);
      if (o && n && pk0 == pk1 && sk0 == sk1 && c0 == c1)
        continue; // no-op update
      if (o)
        add(mutation.old_rows[i], -1);
      if (n)
        add(mutation.new_rows[i], 1);
    }
    break;
  }
}

// Inserts and updated-to rows go to set(row), deleted rows to erase(row); an
// update that changes the key erases the old row first. An unreadable key
// needs a rebuild.
template <class Set, class Erase>
void ForEachKeyedChange(const core::MutationBatch &mutation, TpchQ2TxnBuffer &buffer,
                        Set &&set, Erase &&erase) {
  switch (mutation.kind) {
  case core::MutationKind::Insert:
    for (const auto &row : mutation.new_rows)
      set(row);
    break;
  case core::MutationKind::Delete:
    for (const auto &row : mutation.old_rows)
      erase(row);
    break;
  case core::MutationKind::Update:
    for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
      int64_t old_key = 0, new_key = 0;
      if (!core::IntCell(mutation.old_rows[i], 0, old_key) ||
          !core::IntCell(mutation.new_rows[i], 0, new_key)) {
        buffer.MarkRebuild();
        return;
      }
      if (old_key != new_key)
        erase(mutation.old_rows[i]);
      set(mutation.new_rows[i]);
    }
    break;
  }
}

void AddEraseOp(TpchQ2TxnBuffer &buffer, const core::AuxRow &row, TpchQ2Op::Kind kind) {
  TpchQ2Op op;
  op.kind = kind;
  if (!core::IntCell(row, 0, op.key)) {
    buffer.MarkRebuild();
    return;
  }
  buffer.AddOp(std::move(op));
}

bool ParseSupplier(const core::AuxRow &row, int64_t &key, TpchQ2SupplierInfo &info) {
  int64_t nationkey = 0;
  int32_t scale = 0;
  if (!core::IntCell(row, 0, key) || !core::StringCell(row, 1, info.name) ||
      !core::StringCell(row, 2, info.address) || !core::IntCell(row, 3, nationkey) ||
      !core::StringCell(row, 4, info.phone) ||
      !core::DecimalCell(row, 5, info.acctbal_raw, scale) ||
      !core::StringCell(row, 6, info.comment))
    return false;
  if (nationkey < std::numeric_limits<int32_t>::min() ||
      nationkey > std::numeric_limits<int32_t>::max())
    return false;
  info.nationkey = static_cast<int32_t>(nationkey);
  info.valid = true;
  return true;
}

bool ParsePart(const core::AuxRow &row, TpchQ2Op &op) {
  int64_t size = 0;
  if (!core::IntCell(row, 0, op.key) || !core::StringCell(row, 1, op.part.mfgr) ||
      !core::StringCell(row, 2, op.part.type) || !core::IntCell(row, 3, size))
    return false;
  if (size < std::numeric_limits<int32_t>::min() ||
      size > std::numeric_limits<int32_t>::max())
    return false;
  op.part.key = op.key;
  op.part.size = static_cast<int32_t>(size);
  return true;
}

void AccumulateSuppliers(const core::MutationBatch &mutation, TpchQ2TxnBuffer &buffer) {
  ForEachKeyedChange(
      mutation, buffer,
      [&](const core::AuxRow &row) {
        TpchQ2Op op;
        op.kind = TpchQ2Op::Kind::SupplierSet;
        if (!ParseSupplier(row, op.key, op.supplier)) {
          buffer.MarkRebuild();
          return;
        }
        buffer.AddOp(std::move(op));
      },
      [&](const core::AuxRow &row) {
        AddEraseOp(buffer, row, TpchQ2Op::Kind::SupplierErase);
      });
}

void AccumulateParts(const core::MutationBatch &mutation, TpchQ2TxnBuffer &buffer) {
  ForEachKeyedChange(
      mutation, buffer,
      [&](const core::AuxRow &row) {
        TpchQ2Op op;
        op.kind = TpchQ2Op::Kind::PartSet;
        if (!ParsePart(row, op)) {
          buffer.MarkRebuild();
          return;
        }
        buffer.AddOp(std::move(op));
      },
      [&](const core::AuxRow &row) {
        AddEraseOp(buffer, row, TpchQ2Op::Kind::PartErase);
      });
}

struct PublishedState {
  std::shared_ptr<const TpchQ2State> state;
  core::Version version = 0;
};

// The published full image, or an empty state when it is absent or foreign.
PublishedState LoadPublished(core::AuxEngineServices &services) {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {core::TpchQ2Layout::kFull});
  for (const auto &image : snapshot.partitions) {
    if (image.partition != core::TpchQ2Layout::kFull)
      continue;
    const auto *typed = image.As<TpchQ2StateImage>();
    if (!typed || typed->kind != TpchQ2ImageKind::Full || !typed->state)
      return {};
    return {typed->state, image.version};
  }
  return {};
}

// Applies `ops` to a copy-on-write working copy of `published` (pages, chunks
// and lists are cloned only where an op writes; the published image is never
// mutated). Fills `outcome` and returns true when the cost model keeps the
// delta; false when the copy is inconsistent or a rebuild is chosen.
bool TryApplyOps(core::AuxEngineServices &services, const std::vector<TpchQ2Op> &ops,
                 const PublishedState &published, core::StagedMaintenanceOutcome &outcome) {
  Maintainer work(*published.state);
  if (core::SliceCount(services, ops.size(), kBatchOpThreshold) > 1) {
    work.ApplyBatch(services, ops, *published.state);
  } else {
    for (const auto &op : ops) {
      work.Apply(op);
      if (!work.ok)
        break;
    }
  }
  if (!work.ok)
    return false;
  const auto decision = services.ChooseMaintenance(
      core::MaintenanceRequest::IncrementalSingleState(
          ops.size(), services.ExpectedQueriesBeforeNextWrite()));
  if (decision.action != core::MaintenanceAction::ApplyDelta)
    return false;
  auto image = std::make_shared<TpchQ2StateImage>();
  image->kind = TpchQ2ImageKind::Full;
  image->state = std::make_shared<const TpchQ2State>(std::move(work.st));
  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.updates.push_back(core::StateImage::Make(
      core::TpchQ2Layout::kAccelerator, core::TpchQ2Layout::kFull, published.version,
      std::shared_ptr<const TpchQ2StateImage>(std::move(image))));
  return true;
}

} // namespace

bool TpchQ2MatchesLike(const std::string &text, const std::string &pattern) {
  // TPC-H Q2's parameter is a SQL LIKE pattern. Implement a faithful
  // recursive wildcard matcher ('%' and '_' are the only special characters).
  const std::size_t text_len = text.size();
  const std::size_t pattern_len = pattern.size();
  auto matches = [&](const auto &self, std::size_t ti, std::size_t pi) -> bool {
    while (pi < pattern_len) {
      const char pc = pattern[pi];
      if (pc == '%') {
        if (pi + 1 == pattern_len)
          return true;
        for (std::size_t start = ti; start <= text_len; ++start) {
          if (self(self, start, pi + 1))
            return true;
        }
        return false;
      }
      if (pc == '_') {
        if (ti >= text_len)
          return false;
        ++ti;
        ++pi;
        continue;
      }
      if (ti >= text_len || text[ti] != pc)
        return false;
      ++ti;
      ++pi;
    }
    return ti == text_len;
  };
  return matches(matches, 0, 0);
}

TpchQ2LikeAccelerator
TpchQ2BuildLikeAccelerator(const std::string &pattern) {
  TpchQ2LikeAccelerator acc;
  acc.pattern = pattern;
  acc.enabled = !pattern.empty();
  if (pattern.size() >= 2) {
    acc.prefix_wild = pattern.front() == '%';
    acc.suffix_wild = pattern.back() == '%';
  }
  return acc;
}

bool TpchQ2LikeAcceleratedMatch(const TpchQ2LikeAccelerator &acc,
                                const std::string &text) {
  if (!acc.enabled) {
    return true;
  }
  // Suffix-only wild (e.g., %BRASS): check text.ends_with(pat_body)
  if (acc.prefix_wild && !acc.suffix_wild) {
    const std::size_t pat_start = 1;
    if (pat_start < acc.pattern.size()) {
      std::string_view pat(&acc.pattern[pat_start], acc.pattern.size() - pat_start);
      if (pat.find('%') == std::string_view::npos &&
          pat.find('_') == std::string_view::npos) {
        return text.size() >= pat.size() &&
               text.compare(text.size() - pat.size(), pat.size(),
                            pat.data(), pat.size()) == 0;
      }
    }
  }
  // Fast path: pure substring (single % at both ends) — extremely common in
  // TPC-H type predicates.  Use std::string::find instead of recursive LIKE.
  if (acc.prefix_wild && acc.suffix_wild) {
    const std::size_t start = 1;
    const std::size_t end = acc.pattern.size() - 1;
    if (start < end) {
      const std::string substr = acc.pattern.substr(start, end - start);
      if (substr.find('%') == std::string::npos &&
          substr.find('_') == std::string::npos) {
        return text.find(substr) != std::string::npos;
      }
    }
  }
  // General path: fall back to full LIKE
  return TpchQ2MatchesLike(text, acc.pattern);
}

bool TpchQ2TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ2TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  ops.clear();
}
bool TpchQ2TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ2Op> &TpchQ2TxnBuffer::Ops() const { return ops; }
void TpchQ2TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
}
void TpchQ2TxnBuffer::AddOp(TpchQ2Op op) {
  pending = true;
  ops.push_back(std::move(op));
}

core::AcceleratorId TpchQ2Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ2Module::Dependencies() const {
  return {{config.part_table, {0, 1, 2, 3}, {0, 2, 4, 5}},
          {config.supplier_table, {0, 1, 2, 3, 4, 5, 6}, {0, 1, 2, 3, 4, 5, 6}},
          {config.partsupp_table, {0, 1, 2}, {0, 1, 3}},
          {config.nation_table, {0, 1, 2}, {0, 1, 2}},
          {config.region_table, {0, 1}, {0, 1}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ2Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ2TxnBuffer>();
}

void TpchQ2Module::Build(core::AuxEngineServices &services,
                         core::StateBuilder &builder) {
  auto state = BuildState(services);
  const auto typed_state =
      std::make_shared<const TpchQ2State>(std::move(state));
  builder.Write(core::StateImage::Make(
      core::TpchQ2Layout::kAccelerator, core::TpchQ2Layout::kFull, 0,
      std::make_shared<const TpchQ2StateImage>(TpchQ2StateImage{
          TpchQ2ImageKind::Full, typed_state})));
  builder.Finish();
}

core::InvalidationPlan
TpchQ2Module::InterpretMutation(const core::MutationBatch &mutation) const {
  // Partsupp / supplier / part changes are applied incrementally (page- and
  // chunk-level copy-on-write).  Nation/region changes are rare and alter the
  // region mapping and name ranks of every supplier: full rebuild.
  if (mutation.table == config.partsupp_table ||
      mutation.table == config.supplier_table ||
      mutation.table == config.part_table) {
    return {};
  }
  if (mutation.table == config.nation_table ||
      mutation.table == config.region_table) {
    return core::AllPartitionsPlan();
  }
  return {};
}

void TpchQ2Module::AccumulateMutation(const core::MutationBatch &mutation,
                                      core::AuxEngineServices &,
                                      core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ2TxnBuffer &>(base_buffer);
  const auto table = mutation.table;
  if (table == config.nation_table || table == config.region_table) {
    buffer.MarkRebuild();
    return;
  }
  if (table != config.partsupp_table && table != config.supplier_table &&
      table != config.part_table)
    return;

  if (mutation.kind == core::MutationKind::Update &&
      mutation.old_rows.size() != mutation.new_rows.size()) {
    buffer.MarkRebuild();
    return;
  }

  if (table == config.partsupp_table)
    AccumulatePartsupp(mutation, buffer);
  else if (table == config.supplier_table)
    AccumulateSuppliers(mutation, buffer);
  else
    AccumulateParts(mutation, buffer);
}

core::StagedMaintenanceOutcome
TpchQ2Module::StageMaintenance(core::AuxEngineServices &services,
                               const core::InvalidationPlan &,
                               core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ2TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;
  if (!buffer.HasPendingChanges())
    return outcome;
  if (!buffer.RequiresRebuild()) {
    if (buffer.Ops().empty()) {
      buffer.Clear();
      return outcome;
    }
    const auto published = LoadPublished(services);
    if (!published.state) {
      buffer.Clear();
      return outcome;
    }
    if (TryApplyOps(services, buffer.Ops(), published, outcome)) {
      buffer.Clear();
      return outcome;
    }
    // A rebuild chosen by the cost model or forced leaves the updates empty, so
    // PreCommit MaterializeSyncRebuild can run under an active snapshot.
  }

  // Full rebuild: nation/region mutations, unparsable rows, or an inconsistent
  // incremental application.
  core::FinishRebuildAll(
      outcome, services.ChooseMaintenance(core::MaintenanceRequest::RebuildOnly(
                   services.ExpectedQueriesBeforeNextWrite())));
  buffer.Clear();
  return outcome;
}

bool TpchQ2Module::CheckValidity(core::AuxEngineServices &services,
                                 const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ2Module::Execute(core::AuxEngineServices &services,
                           const core::QueryArguments &arguments,
                           core::AuxRowSink &sink) {
  if (!services.StateStore()
           .CheckValidity({Id(), {}}, services.CurrentVisibility())
           .IsValid()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  auto state = LoadState(services);
  if (!state) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  int32_t part_size = 0;
  std::string part_type_pattern;
  std::string region_name;
  if (arguments.values.size() != 3) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  if (arguments.values[0].is_null || arguments.values[1].is_null ||
      arguments.values[2].is_null) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  if (std::holds_alternative<int64_t>(arguments.values[0].payload)) {
    part_size = static_cast<int32_t>(std::get<int64_t>(arguments.values[0].payload));
  } else {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  if (!StringValue(arguments.values[1], part_type_pattern)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  if (!StringValue(arguments.values[2], region_name)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitResult(*state, part_size, part_type_pattern, region_name, sink);
}

TpchQ2State
TpchQ2Module::BuildState(core::AuxEngineServices &services) const {
  TpchQ2State state;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q2 requires a typed initial scan source");
  InitEmptyState(state);

  auto regions = std::make_shared<std::vector<std::string>>();
  auto nations = std::make_shared<std::vector<std::string>>();
  auto nation_regions = std::make_shared<std::vector<int32_t>>();
  TpchQ2TypeDict types;

  initial_scan->ScanRegionsSerial([&](const TpchQ2RegionInput &input) {
    AcceptFlatKey(input.key);
    EnsureFlatSize(*regions, static_cast<std::size_t>(input.key) + 1);
    (*regions)[static_cast<std::size_t>(input.key)] = input.name;
  });
  initial_scan->ScanNationsSerial([&](const TpchQ2NationInput &input) {
    AcceptFlatKey(input.key);
    if (input.regionkey < 0)
      return;
    EnsureFlatSize(*nations, static_cast<std::size_t>(input.key) + 1);
    EnsureFlatSize(*nation_regions, static_cast<std::size_t>(input.key) + 1);
    (*nations)[static_cast<std::size_t>(input.key)] = input.name;
    (*nation_regions)[static_cast<std::size_t>(input.key)] = input.regionkey;
  });
  state.region_names = std::move(regions);
  state.nation_names = std::move(nations);
  state.nation_region = std::move(nation_regions);
  BuildNationRank(state);

  const auto parallelism = core::Parallelism::OnPool(services);
  {
    using Rows = core::InternedPieces<PartRow>;
    auto scanned = core::PartitionedBuild<Rows>(
        services, [] { return Rows{}; },
        [&](std::size_t parts, auto &&sink) {
          initial_scan->ScanTable(services, "part",
                                  {scan::IntColumn("p_partkey"), scan::TextColumn("p_mfgr"),
                                   scan::TextColumn("p_type"), scan::IntColumn("p_size")},
                                  parts, sink);
        },
        [](Rows &into, const auto &row) {
          AcceptDenseKey(row.Int(0));
          into.Add({row.Int(0), row.Text(1), into.Intern(row.Text(2)),
                    static_cast<int32_t>(row.Int(3))});
        },
        Rows::Merge);
    scanned.Unify(
        parallelism, [&](const std::string &type) { return static_cast<int32_t>(types.Intern(type)); },
        [](PartRow &row, const std::vector<int32_t> &table) { row.type = table[row.type]; });
    core::FillChunks(
        parallelism, TpchQ2MutOf(state.parts), scanned.pieces,
        [](const PartRow &row) { return row.key; },
        [](TpchQ2PartInfo &slot, PartRow &row) {
          slot.mfgr = std::move(row.mfgr);
          slot.type_id = row.type;
          slot.size = row.size;
          slot.valid = true;
        });
  }
  state.types = std::make_shared<const TpchQ2TypeDict>(std::move(types));
  {
    using Rows = core::RowPieces<TpchQ2SupplierInput>;
    auto scanned = core::PartitionedBuild<Rows>(
        services, [] { return Rows{}; },
        [&](std::size_t parts, auto &&sink) {
          initial_scan->ScanTable(
              services, "supplier",
              {scan::IntColumn("s_suppkey"), scan::TextColumn("s_name"),
               scan::TextColumn("s_address"), scan::IntColumn("s_nationkey"),
               scan::TextColumn("s_phone"), scan::DecimalColumn("s_acctbal", 2),
               scan::TextColumn("s_comment")},
              parts, sink);
        },
        [](Rows &into, const auto &row) {
          AcceptDenseKey(row.Int(0));
          into.Add({row.Int(0), row.Text(1), row.Text(2), static_cast<int32_t>(row.Int(3)),
                    row.Text(4), row.Decimal(5), row.Text(6)});
        },
        Rows::Merge);
    core::FillChunks(
        parallelism, TpchQ2MutOf(state.suppliers), scanned.pieces,
        [](const TpchQ2SupplierInput &input) { return input.key; },
        [](TpchQ2SupplierInfo &slot, TpchQ2SupplierInput &input) {
          slot.name = std::move(input.name);
          slot.address = std::move(input.address);
          slot.nationkey = input.nationkey;
          slot.phone = std::move(input.phone);
          slot.acctbal_raw = input.acctbal_raw;
          slot.comment = std::move(input.comment);
          slot.valid = true;
        });
  }

  // Partsupp rows are kept as pieces in scan order and appended to the per-part
  // offer lists and per-supplier part lists chunk by chunk, in parallel.
  using scan::DecimalColumn;
  using scan::IntColumn;
  using Rows = core::RowPieces<PartsuppRow>;
  auto partsupps = core::PartitionedBuild<Rows>(
      services, [] { return Rows{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanTable(services, "partsupp",
                                {IntColumn("ps_partkey"), IntColumn("ps_suppkey"),
                                 DecimalColumn("ps_supplycost", 2)},
                                parts, sink);
      },
      [](Rows &into, const auto &row) {
        AcceptDenseKey(row.Int(1));
        AcceptDenseKey(row.Int(0));
        into.Add({row.Int(0), row.Int(1), row.Decimal(2)});
      },
      Rows::Merge);
  decltype(partsupps.pieces) by_supplier(partsupps.pieces.size());
  parallelism.For(by_supplier.size(), [&](std::size_t p) { by_supplier[p] = partsupps.pieces[p]; });
  TpchQ2Paged<std::vector<TpchQ2Offer>, 9> offers;
  core::FillChunks(
      parallelism, offers, partsupps.pieces, [](const PartsuppRow &row) { return row.partkey; },
      [](std::vector<TpchQ2Offer> &list, const PartsuppRow &row) {
        list.push_back({row.cost_raw, static_cast<int32_t>(row.suppkey)});
      });
  TpchQ2Paged<std::vector<int32_t>, 6> supp_parts;
  core::FillChunks(
      parallelism, supp_parts, by_supplier, [](const PartsuppRow &row) { return row.suppkey; },
      [](std::vector<int32_t> &list, const PartsuppRow &row) {
        list.push_back(static_cast<int32_t>(row.partkey));
      });
  state.offers = std::make_shared<const TpchQ2Paged<std::vector<TpchQ2Offer>, 9>>(std::move(offers));
  state.supp_parts =
      std::make_shared<const TpchQ2Paged<std::vector<int32_t>, 6>>(std::move(supp_parts));
  BuildGroups(services, state);
  return state;
}

std::shared_ptr<const TpchQ2State>
TpchQ2Module::LoadState(core::AuxEngineServices &services) const {
  return LoadPublished(services).state;
}

void TpchQ2Module::EmitResult(const TpchQ2State &state, int32_t part_size,
                              const std::string &part_type_pattern,
                              const std::string &region_name,
                              core::AuxRowSink &sink) const {
  TpchQ2FinalizeAndEmit(state, part_size, part_type_pattern, region_name, sink);
}

std::vector<core::CalibrationWrite> TpchQ2Module::CalibrationWrites() const {
  return TpchQ2CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
