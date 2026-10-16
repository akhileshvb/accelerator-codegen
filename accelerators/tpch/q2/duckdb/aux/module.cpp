#include "tpch/q2/duckdb/aux/module.hpp"
#include "tpch/q2/calibration_writes.hpp"
#include "substrate/aux/core/byte_stream.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
#include "tpch/q2/layout.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {

// ── Custom profiling (no-op unless ACCEL_AUX_CUSTOM_PROFILE is set) ─────────
struct CustomTimer {
  using Clock = std::chrono::high_resolution_clock;
  Clock::time_point start;
  bool active;
  explicit CustomTimer(bool enabled) : start(Clock::now()), active(enabled) {}
  double us() const {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - start)
            .count()) /
           1000.0;
  }
};

static bool CustomProfilingEnabled() {
  static const bool enabled = []() {
    const char *v = std::getenv("ACCEL_AUX_CUSTOM_PROFILE");
    return v && v[0] != '\0';
  }();
  return enabled;
}

static void EmitCustomProfile(const char *phase, double us_val, int64_t rows,
                              int64_t groups, int64_t keys) {
  if (!CustomProfilingEnabled())
    return;
  fprintf(stderr,
          "ACCEL_CUSTOM_PROFILE {"
          "\"phase\":\"%s\","
          "\"us\":%.3f,"
          "\"rows\":%ld,"
          "\"groups\":%ld,"
          "\"keys\":%ld"
          "}\n",
          phase, us_val, (long)rows, (long)groups, (long)keys);
}


using core::ByteReader;
using core::ByteWriter;

constexpr uint32_t kVersion = 2;
constexpr uint32_t kStateMagic = 0x5132534d; // Q2SM

bool StringValue(const core::AuxValue &value, std::string &out) {
  if (value.is_null || !std::holds_alternative<std::string>(value.payload))
    return false;
  out = std::get<std::string>(value.payload);
  return true;
}

bool IntCell(const core::AuxRow &row, std::size_t index, int64_t &out) {
  if (index >= row.values.size() || row.values[index].is_null)
    return false;
  const auto &cell = row.values[index];
  if (std::holds_alternative<int64_t>(cell.payload)) {
    out = std::get<int64_t>(cell.payload);
    return true;
  }
  if (std::holds_alternative<uint64_t>(cell.payload)) {
    const auto value = std::get<uint64_t>(cell.payload);
    if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
      return false;
    out = static_cast<int64_t>(value);
    return true;
  }
  return false;
}

bool RawDecimalCell(const core::AuxRow &row, std::size_t index, int64_t &raw,
                    int32_t &scale) {
  if (index >= row.values.size() || row.values[index].is_null)
    return false;
  const auto &cell = row.values[index];
  if (std::holds_alternative<core::ScaledDecimal>(cell.payload)) {
    const auto decimal = std::get<core::ScaledDecimal>(cell.payload);
    raw = decimal.value;
    scale = decimal.scale;
    return true;
  }
  return false;
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

bool ReadPartsupp(const core::AuxRow &row, int64_t &partkey, int64_t &suppkey,
                  int64_t &supplycost_raw) {
  int32_t scale = 0;
  return IntCell(row, 0, partkey) && IntCell(row, 1, suppkey) &&
         RawDecimalCell(row, 2, supplycost_raw, scale);
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

constexpr int64_t kMaxFlatKey = 1'000'000;

bool AcceptFlatKey(int64_t key) {
  if (key < 0 || key > kMaxFlatKey)
    throw std::out_of_range("TPC-H Q2 nation/region key too large: " +
                            std::to_string(key));
  return true;
}


bool StrCell(const core::AuxRow &row, std::size_t index, std::string &out) {
  if (index >= row.values.size())
    return false;
  return StringValue(row.values[index], out);
}

constexpr std::size_t kChunkTarget = 256;
constexpr std::size_t kChunkMax = 512;

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

// Partsupp rows of one scan partition, by part and by supplier.
struct OffersPartial {
  TpchQ2Paged<std::vector<TpchQ2Offer>, 9> offers;
  TpchQ2Paged<std::vector<int32_t>, 6> supp_parts;
  int64_t rows = 0;
};

// Appends every list of `from` to the list of the same key in `into`.
template <class Paged> void AppendLists(Paged &into, const Paged &from) {
  for (std::size_t ci = 0; ci < from.NumChunks(); ++ci) {
    const auto *chunk = from.ChunkAt(ci);
    if (!chunk)
      continue;
    const int64_t base = Paged::ChunkBase(ci);
    for (std::size_t i = 0; i < chunk->size(); ++i) {
      const auto &list = (*chunk)[i];
      if (list.empty())
        continue;
      auto &target = into.Mutable(base + static_cast<int64_t>(i));
      target.insert(target.end(), list.begin(), list.end());
    }
  }
}

using GroupAcc = std::unordered_map<int64_t, std::vector<TpchQ2Entry>>;

// Builds the sorted candidate lists from scratch (Build / Decode).
void BuildGroups(core::AuxEngineServices &services, TpchQ2State &st) {
  // ComputeEntries costs about a microsecond per part slot.
  constexpr std::size_t kPartGrain = 1 << 16;
  GroupAcc acc;
  core::ParallelReduce(
      services, TpchQ2Capacity(*st.parts), kPartGrain, acc,
      [&](GroupAcc &into, std::size_t begin, std::size_t end) {
        std::vector<GroupEntry> tmp;
        for (std::size_t k = begin; k < end; ++k) {
          const auto *p = st.parts->Find(static_cast<int64_t>(k));
          if (!p || !p->valid)
            continue;
          tmp.clear();
          ComputeEntries(st, static_cast<int64_t>(k), tmp);
          for (const auto &ge : tmp)
            into[ge.group].push_back(ge.e);
        }
      },
      [](GroupAcc &into, GroupAcc &&from) {
        for (auto &kv : from) {
          auto &v = into[kv.first];
          v.insert(v.end(), kv.second.begin(), kv.second.end());
        }
      });
  std::vector<GroupAcc::value_type *> items;
  items.reserve(acc.size());
  for (auto &kv : acc)
    items.push_back(&kv);
  std::vector<std::shared_ptr<TpchQ2GroupList>> lists(items.size());
  const EntryLess less{&st};
  services.ParallelFor(items.size(), [&](std::size_t i) {
    auto &v = items[i]->second;
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
    std::vector<TpchQ2Entry>().swap(v);
  });
  auto groups = std::make_shared<TpchQ2GroupMap>();
  groups->reserve(acc.size());
  for (std::size_t i = 0; i < items.size(); ++i)
    (*groups)[items[i]->first] = std::move(lists[i]);
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
  // ---- sorted list primitives -------------------------------------------
  static std::size_t ChunkIndex(const TpchQ2GroupList &gl, const EntryLess &less,
                                const TpchQ2Entry &e) {
    // first chunk whose last element is not less than e
    return static_cast<std::size_t>(
        std::partition_point(gl.chunks.begin(), gl.chunks.end(),
                             [&](const std::shared_ptr<const TpchQ2Chunk> &c) {
                               return less(c->v.back(), e);
                             }) -
        gl.chunks.begin());
  }

  void InsertEntry(int64_t group, const TpchQ2Entry &e) {
    const EntryLess less{&st};
    auto &map = TpchQ2MutOf(st.groups);
    auto &slot = map[group];
    auto &gl = TpchQ2MutOf(slot);
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

  void RemoveEntry(int64_t group, const TpchQ2Entry &e) {
    const EntryLess less{&st};
    const auto git = st.groups->find(group);
    if (git == st.groups->end()) {
      ok = false;
      return;
    }
    const std::size_t idx = ChunkIndex(*git->second, less, e);
    if (idx >= git->second->chunks.size()) {
      ok = false;
      return;
    }
    {
      const auto &cv = git->second->chunks[idx]->v;
      const auto pos = std::lower_bound(cv.begin(), cv.end(), e, less);
      if (pos == cv.end() || less(e, *pos) || pos->partkey != e.partkey ||
          pos->suppkey != e.suppkey) {
        ok = false;
        return;
      }
    }
    auto &map = TpchQ2MutOf(st.groups);
    auto &gl = TpchQ2MutOf(map[group]);
    auto &chunk = TpchQ2MutOf(gl.chunks[idx]);
    const auto pos = std::lower_bound(chunk.v.begin(), chunk.v.end(), e, less);
    chunk.v.erase(pos);
    if (chunk.v.empty())
      gl.chunks.erase(gl.chunks.begin() + static_cast<std::ptrdiff_t>(idx));
  }

  // Removes `before \ after`, inserts `after \ before` (multiset difference).
  void ApplyDiff(std::vector<GroupEntry> &before,
                 std::vector<GroupEntry> &after) {
    std::sort(before.begin(), before.end(), GroupEntryCanon);
    std::sort(after.begin(), after.end(), GroupEntryCanon);
    std::vector<GroupEntry> rem, add;
    std::size_t i = 0, j = 0;
    while (i < before.size() || j < after.size()) {
      if (i == before.size()) {
        add.push_back(after[j++]);
      } else if (j == after.size()) {
        rem.push_back(before[i++]);
      } else if (GroupEntryEq(before[i], after[j])) {
        ++i;
        ++j;
      } else if (GroupEntryCanon(before[i], after[j])) {
        rem.push_back(before[i++]);
      } else {
        add.push_back(after[j++]);
      }
    }
    for (const auto &g : rem) {
      RemoveEntry(g.group, g.e);
      if (!ok)
        return;
    }
    for (const auto &g : add)
      InsertEntry(g.group, g.e);
  }

  // ---- operations ---------------------------------------------------------
  void ApplyPartsupp(const TpchQ2PartsuppDelta &d) {
    if (!AcceptDenseKey(d.partkey) || !AcceptDenseKey(d.suppkey))
      return;
    std::vector<GroupEntry> before, after;
    ComputeEntries(st, d.partkey, before);
    if (d.sign > 0) {
      AddOfferRaw(st, d.partkey, d.suppkey, d.supplycost_raw);
    } else {
      auto &offs =
          TpchQ2MutOf(st.offers).Mutable(d.partkey);
      auto it = std::find_if(offs.begin(), offs.end(), [&](const TpchQ2Offer &o) {
        return o.suppkey == d.suppkey && o.cost_raw == d.supplycost_raw;
      });
      if (it == offs.end()) {
        ok = false; // delete of an offer we never saw
        return;
      }
      offs.erase(it);
      auto &sp =
          TpchQ2MutOf(st.supp_parts).Mutable(d.suppkey);
      const auto sit = std::find(sp.begin(), sp.end(),
                                 static_cast<int32_t>(d.partkey));
      if (sit != sp.end())
        sp.erase(sit);
    }
    ComputeEntries(st, d.partkey, after);
    ApplyDiff(before, after);
  }

  void ApplyPart(int64_t key, const TpchQ2PartInput *in) {
    if (!AcceptDenseKey(key))
      return;
    const auto *cur = st.FindPart(key);
    if (!cur && !in)
      return;
    std::vector<GroupEntry> before, after;
    ComputeEntries(st, key, before);
    auto &slot = TpchQ2MutOf(st.parts).Mutable(key);
    if (in) {
      slot.mfgr = in->mfgr;
      slot.type_id = static_cast<int32_t>(InternType(in->type));
      slot.size = in->size;
      slot.valid = true;
    } else {
      slot = TpchQ2PartInfo{};
    }
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
    const auto *cur = st.FindSupplier(key);
    if (!cur && !in)
      return;
    // Fields that only matter for output (address/phone/comment) need no
    // list work at all.
    if (cur && in && cur->name == in->name && cur->nationkey == in->nationkey &&
        cur->acctbal_raw == in->acctbal_raw) {
      auto &slot = TpchQ2MutOf(st.suppliers).Mutable(key);
      slot = *in;
      slot.valid = true;
      return;
    }
    // Affected parts = parts this supplier has offers for.
    std::vector<int32_t> parts;
    if (const auto *sp = st.supp_parts->Find(static_cast<std::size_t>(key)))
      parts = *sp;
    std::sort(parts.begin(), parts.end());
    parts.erase(std::unique(parts.begin(), parts.end()), parts.end());

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
    auto &slot = TpchQ2MutOf(st.suppliers).Mutable(key);
    if (in) {
      slot = *in;
      slot.valid = true;
    } else {
      slot = TpchQ2SupplierInfo{};
    }
    // 3. recompute and diff the remaining entries against the new truth.
    for (const auto p : parts)
      ComputeEntries(st, p, after);
    ApplyDiff(rest, after);
  }
};

} // namespace

std::vector<uint8_t> EncodeTpchQ2State(const TpchQ2State &state) {
  ByteWriter writer;
  const auto &regions = *state.region_names;
  core::WriteImageHeader(writer, kStateMagic, kVersion,
                         static_cast<uint64_t>(regions.size()));
  writer.U32(static_cast<uint32_t>(regions.size()));
  for (std::size_t key = 0; key < regions.size(); ++key) {
    if (regions[key].empty())
      continue;
    writer.I32(static_cast<int32_t>(key));
    writer.String(regions[key]);
  }

  const auto &nations = *state.nation_names;
  const auto &nation_regions = *state.nation_region;
  writer.U32(static_cast<uint32_t>(nations.size()));
  for (std::size_t key = 0; key < nations.size(); ++key) {
    if (nations[key].empty())
      continue;
    writer.I32(static_cast<int32_t>(key));
    writer.String(nations[key]);
    writer.I32(nation_regions[key]);
  }

  // Suppliers / parts: valid slots only, preceded by their count.
  {
    const std::size_t cap = TpchQ2Capacity(*state.suppliers);
    uint32_t valid = 0;
    for (std::size_t key = 0; key < cap; ++key) {
      const auto *s = state.suppliers->Find(static_cast<int64_t>(key));
      valid += s && s->valid ? 1u : 0u;
    }
    writer.U32(valid);
    for (std::size_t key = 0; key < cap; ++key) {
      const auto *sp = state.suppliers->Find(static_cast<int64_t>(key));
      if (!sp || !sp->valid)
        continue;
      const auto &supplier = *sp;
      writer.I64(static_cast<int64_t>(key));
      writer.String(supplier.name);
      writer.String(supplier.address);
      writer.I32(supplier.nationkey);
      writer.String(supplier.phone);
      writer.I64(supplier.acctbal_raw);
      writer.String(supplier.comment);
    }
  }
  {
    const std::size_t cap = TpchQ2Capacity(*state.parts);
    uint32_t valid = 0;
    for (std::size_t key = 0; key < cap; ++key)
      {
      const auto *p = state.parts->Find(static_cast<int64_t>(key));
      valid += p && p->valid ? 1u : 0u;
    }
    writer.U32(valid);
    for (std::size_t key = 0; key < cap; ++key) {
      const auto *pp = state.parts->Find(static_cast<int64_t>(key));
      if (!pp || !pp->valid)
        continue;
      const auto &part = *pp;
      writer.I64(static_cast<int64_t>(key));
      writer.String(part.mfgr);
      writer.String(state.types->names[static_cast<std::size_t>(part.type_id)]);
      writer.I32(part.size);
    }
  }

  // Partsupp offers (the candidate lists and supplier->parts index are
  // derived and rebuilt on decode).
  {
    const std::size_t cap = TpchQ2Capacity(*state.offers);
    uint32_t total = 0;
    for (std::size_t key = 0; key < cap; ++key) {
      const auto *o = state.offers->Find(static_cast<int64_t>(key));
      total += o ? static_cast<uint32_t>(o->size()) : 0u;
    }
    writer.U32(total);
    for (std::size_t key = 0; key < cap; ++key) {
      const auto *list = state.offers->Find(static_cast<int64_t>(key));
      if (!list)
        continue;
      for (const auto &o : *list) {
        writer.I64(static_cast<int64_t>(key));
        writer.I64(o.suppkey);
        writer.I64(o.cost_raw);
      }
    }
  }
  return std::move(writer.bytes);
}

bool DecodeTpchQ2State(core::AuxEngineServices &services,
                       const std::vector<uint8_t> &bytes, TpchQ2State &state) {
  ByteReader reader(bytes);
  uint64_t count = 0;
  if (!core::ReadImageHeader(reader, kStateMagic, kVersion, count))
    return false;

  state = TpchQ2State{};
  InitEmptyState(state);
  auto regions = std::make_shared<std::vector<std::string>>();
  auto nations = std::make_shared<std::vector<std::string>>();
  auto nation_regions = std::make_shared<std::vector<int32_t>>();
  TpchQ2TypeDict types;

  uint32_t region_count = 0;
  if (!reader.U32(region_count))
    return false;
  for (uint32_t i = 0; i < region_count; ++i) {
    int32_t key = 0;
    std::string name;
    if (!reader.I32(key) || !reader.String(name))
      return false;
    if (key < 0)
      return false;
    EnsureFlatSize(*regions, static_cast<std::size_t>(key) + 1);
    (*regions)[static_cast<std::size_t>(key)] = std::move(name);
  }

  uint32_t nation_count = 0;
  if (!reader.U32(nation_count))
    return false;
  for (uint32_t i = 0; i < nation_count; ++i) {
    int32_t key = 0, region = 0;
    std::string name;
    if (!reader.I32(key) || !reader.String(name) || !reader.I32(region))
      return false;
    if (key < 0 || region < 0)
      return false;
    EnsureFlatSize(*nations, static_cast<std::size_t>(key) + 1);
    EnsureFlatSize(*nation_regions, static_cast<std::size_t>(key) + 1);
    (*nations)[static_cast<std::size_t>(key)] = std::move(name);
    (*nation_regions)[static_cast<std::size_t>(key)] = region;
  }
  state.region_names = std::move(regions);
  state.nation_names = std::move(nations);
  state.nation_region = std::move(nation_regions);
  BuildNationRank(state);

  uint32_t supplier_count = 0;
  if (!reader.U32(supplier_count))
    return false;
  {
    auto &paged = TpchQ2MutOf(state.suppliers);
    for (uint32_t i = 0; i < supplier_count; ++i) {
      int64_t key = 0;
      TpchQ2SupplierInfo supplier;
      if (!reader.I64(key) || !reader.String(supplier.name) ||
          !reader.String(supplier.address) || !reader.I32(supplier.nationkey) ||
          !reader.String(supplier.phone) || !reader.I64(supplier.acctbal_raw) ||
          !reader.String(supplier.comment))
        return false;
      if (!AcceptDenseKey(key))
        return false;
      supplier.valid = true;
      paged.Mutable(key) = std::move(supplier);
    }
  }

  uint32_t part_count = 0;
  if (!reader.U32(part_count))
    return false;
  {
    auto &paged = TpchQ2MutOf(state.parts);
    for (uint32_t i = 0; i < part_count; ++i) {
      int64_t key = 0;
      TpchQ2PartInfo part;
      std::string type;
      int32_t size = 0;
      if (!reader.I64(key) || !reader.String(part.mfgr) ||
          !reader.String(type) || !reader.I32(size))
        return false;
      if (!AcceptDenseKey(key))
        return false;
      part.type_id = static_cast<int32_t>(types.Intern(type));
      part.size = size;
      part.valid = true;
      paged.Mutable(key) = std::move(part);
    }
  }
  state.types = std::make_shared<const TpchQ2TypeDict>(std::move(types));

  uint32_t offer_count = 0;
  if (!reader.U32(offer_count))
    return false;
  for (uint32_t i = 0; i < offer_count; ++i) {
    int64_t partkey = 0, suppkey = 0, cost = 0;
    if (!reader.I64(partkey) || !reader.I64(suppkey) || !reader.I64(cost))
      return false;
    if (!AcceptDenseKey(partkey) || !AcceptDenseKey(suppkey))
      return false;
    AddOfferRaw(state, partkey, suppkey, cost);
  }
  BuildGroups(services, state);
  return true;
}

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
    return All();
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

  // Insert -> new_rows, Delete -> old_rows, Update -> paired rows.
  if (mutation.kind == core::MutationKind::Update &&
      mutation.old_rows.size() != mutation.new_rows.size()) {
    buffer.MarkRebuild();
    return;
  }

  if (table == config.partsupp_table) {
    // A row that cannot be parsed has a NULL join/aggregate column and
    // contributes nothing (neither joins nor feeds MIN), so it is skipped.
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
    return;
  }

  if (table == config.supplier_table) {
    auto parse = [&](const core::AuxRow &row, int64_t &key,
                     TpchQ2SupplierInfo &info) {
      int64_t nationkey = 0;
      int32_t scale = 0;
      if (!IntCell(row, 0, key) || !StrCell(row, 1, info.name) ||
          !StrCell(row, 2, info.address) || !IntCell(row, 3, nationkey) ||
          !StrCell(row, 4, info.phone) ||
          !RawDecimalCell(row, 5, info.acctbal_raw, scale) ||
          !StrCell(row, 6, info.comment))
        return false;
      if (nationkey < std::numeric_limits<int32_t>::min() ||
          nationkey > std::numeric_limits<int32_t>::max())
        return false;
      info.nationkey = static_cast<int32_t>(nationkey);
      info.valid = true;
      return true;
    };
    auto set = [&](const core::AuxRow &row) {
      TpchQ2Op op;
      op.kind = TpchQ2Op::Kind::SupplierSet;
      if (!parse(row, op.key, op.supplier)) {
        buffer.MarkRebuild();
        return;
      }
      buffer.AddOp(std::move(op));
    };
    auto erase = [&](const core::AuxRow &row) {
      TpchQ2Op op;
      op.kind = TpchQ2Op::Kind::SupplierErase;
      if (!IntCell(row, 0, op.key)) {
        buffer.MarkRebuild();
        return;
      }
      buffer.AddOp(std::move(op));
    };
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
        if (!IntCell(mutation.old_rows[i], 0, old_key) ||
            !IntCell(mutation.new_rows[i], 0, new_key)) {
          buffer.MarkRebuild();
          return;
        }
        if (old_key != new_key)
          erase(mutation.old_rows[i]);
        set(mutation.new_rows[i]);
      }
      break;
    }
    return;
  }

  // part table
  auto parse = [&](const core::AuxRow &row, TpchQ2Op &op) {
    int64_t size = 0;
    if (!IntCell(row, 0, op.key) || !StrCell(row, 1, op.part.mfgr) ||
        !StrCell(row, 2, op.part.type) || !IntCell(row, 3, size))
      return false;
    if (size < std::numeric_limits<int32_t>::min() ||
        size > std::numeric_limits<int32_t>::max())
      return false;
    op.part.key = op.key;
    op.part.size = static_cast<int32_t>(size);
    return true;
  };
  auto set = [&](const core::AuxRow &row) {
    TpchQ2Op op;
    op.kind = TpchQ2Op::Kind::PartSet;
    if (!parse(row, op)) {
      buffer.MarkRebuild();
      return;
    }
    buffer.AddOp(std::move(op));
  };
  auto erase = [&](const core::AuxRow &row) {
    TpchQ2Op op;
    op.kind = TpchQ2Op::Kind::PartErase;
    if (!IntCell(row, 0, op.key)) {
      buffer.MarkRebuild();
      return;
    }
    buffer.AddOp(std::move(op));
  };
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
      if (!IntCell(mutation.old_rows[i], 0, old_key) ||
          !IntCell(mutation.new_rows[i], 0, new_key)) {
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

core::StagedMaintenanceOutcome
TpchQ2Module::StageMaintenance(core::AuxEngineServices &services,
                               const core::InvalidationPlan &,
                               core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ2TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;
  if (!buffer.HasPendingChanges()) {
    return outcome;
  }
  if (!buffer.RequiresRebuild()) {
    const auto &ops = buffer.Ops();
    if (ops.empty()) {
      buffer.Clear();
      return outcome;
    }
    const auto visibility = services.CurrentVisibility();
    const auto snapshot = services.StateStore().ReadPartitions(
        visibility, {core::TpchQ2Layout::kFull});
    std::shared_ptr<const TpchQ2State> old_state;
    core::Version old_version = 0;
    for (const auto &image : snapshot.partitions) {
      if (image.partition != core::TpchQ2Layout::kFull)
        continue;
      const auto *typed = image.As<TpchQ2StateImage>();
      if (!typed || typed->kind != TpchQ2ImageKind::Full || !typed->state) {
        buffer.Clear();
        return outcome;
      }
      old_state = typed->state;
      old_version = image.version;
      break;
    }
    if (!old_state) {
      buffer.Clear();
      return outcome;
    }

    // Copy-on-write working copy: a shallow copy of the state's shared_ptr
    // members.  Pages / chunks / lists are cloned only where an op writes
    // (the published image, kept alive by old_state, is never mutated).
    bool profiling = CustomProfilingEnabled();
    std::unique_ptr<CustomTimer> apply_timer;
    if (profiling)
      apply_timer = std::make_unique<CustomTimer>(true);
    auto work = std::make_unique<Maintainer>(*old_state);
    for (const auto &op : ops) {
      work->Apply(op);
      if (!work->ok)
        break;
    }
    if (profiling)
      EmitCustomProfile("write_apply", apply_timer->us(),
                        (int64_t)ops.size(), 0, 0);

    if (work->ok) {
      const auto decision = services.ChooseMaintenance(
          {static_cast<uint64_t>(ops.size()), 1, true, false, true, false,
           false, core::MaintenanceAction::NoOp,
           services.ExpectedQueriesBeforeNextWrite()});
      if (decision.action == core::MaintenanceAction::ApplyDelta) {
        auto image = std::make_shared<TpchQ2StateImage>();
        image->kind = TpchQ2ImageKind::Full;
        image->state = std::make_shared<const TpchQ2State>(std::move(work->st));
        const std::shared_ptr<const TpchQ2StateImage> const_image =
            std::move(image);
        outcome.action = core::MaintenanceAction::ApplyDelta;
        outcome.updates.push_back(core::StateImage::Make(
            core::TpchQ2Layout::kAccelerator, core::TpchQ2Layout::kFull,
            old_version, const_image));
        buffer.Clear();
        return outcome;
      }
      // Forced / cost-model rebuild: fall through with empty updates so
      // PreCommit MaterializeSyncRebuild can run under an active snapshot.
    }
  }

  // Full rebuild path (nation/region mutations, unparsable rows, or an
  // inconsistent incremental application).
  const auto decision = services.ChooseMaintenance(
      {0, 0, false, false, true, true, false,
       core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  outcome.action = decision.action == core::MaintenanceAction::ApplyDelta
                       ? core::MaintenanceAction::RebuildAll
                       : decision.action;
  if (outcome.action == core::MaintenanceAction::RebuildPartition) {
    outcome.action = core::MaintenanceAction::RebuildAll;
  }
  outcome.invalidations = All();
  outcome.pending_rebuild = outcome.invalidations;
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
  bool profiling = CustomProfilingEnabled();
  std::unique_ptr<CustomTimer> emit_timer;
  if (profiling) {
    emit_timer = std::make_unique<CustomTimer>(true);
  }
  EmitResult(*state, part_size, part_type_pattern, region_name, sink);
  if (profiling) {
    double dt = emit_timer->us();
    int64_t part_count = (int64_t)(state->parts ? TpchQ2Capacity(*state->parts) : 0);
    EmitCustomProfile("execute_emit", dt, part_count, 0, 0);
  }
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

  initial_scan->ScanRegions([&](const TpchQ2RegionInput &input) {
    AcceptFlatKey(input.key);
    EnsureFlatSize(*regions, static_cast<std::size_t>(input.key) + 1);
    (*regions)[static_cast<std::size_t>(input.key)] = input.name;
  });
  initial_scan->ScanNations([&](const TpchQ2NationInput &input) {
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

  {
    auto &paged = TpchQ2MutOf(state.parts);
    initial_scan->ScanParts([&](const TpchQ2PartInput &input) {
      if (!AcceptDenseKey(input.key))
        return;
      auto &slot = paged.Mutable(input.key);
      slot.mfgr = input.mfgr;
      slot.type_id = static_cast<int32_t>(types.Intern(input.type));
      slot.size = input.size;
      slot.valid = true;
    });
  }
  state.types = std::make_shared<const TpchQ2TypeDict>(std::move(types));
  {
    auto &paged = TpchQ2MutOf(state.suppliers);
    initial_scan->ScanSuppliers([&](const TpchQ2SupplierInput &input) {
      if (!AcceptDenseKey(input.key))
        return;
      auto &slot = paged.Mutable(input.key);
      slot.name = input.name;
      slot.address = input.address;
      slot.nationkey = input.nationkey;
      slot.phone = input.phone;
      slot.acctbal_raw = input.acctbal_raw;
      slot.comment = input.comment;
      slot.valid = true;
    });
  }

  // The state is intentionally not a final result cache: it stores the raw
  // per-part offers plus per-(size, region) candidate lists in output order.
  bool profiling = CustomProfilingEnabled();
  std::unique_ptr<CustomTimer> partsupp_timer;
  if (profiling) {
    partsupp_timer = std::make_unique<CustomTimer>(true);
  }
  // Partsupp rows go into per-partition offer lists, appended in partition
  // order (the order of the serial scan).
  using scan::DecimalColumn;
  using scan::IntColumn;
  auto offers = core::PartitionedBuild<OffersPartial>(
      services, [] { return OffersPartial{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanTable(services, "partsupp",
                                {IntColumn("ps_partkey"), IntColumn("ps_suppkey"),
                                 DecimalColumn("ps_supplycost", 2)},
                                parts, sink);
      },
      [](OffersPartial &p, const auto &row) {
        const int64_t partkey = row.Int(0), suppkey = row.Int(1);
        AcceptDenseKey(suppkey);
        AcceptDenseKey(partkey);
        p.offers.Mutable(partkey).push_back({row.Decimal(2), static_cast<int32_t>(suppkey)});
        p.supp_parts.Mutable(suppkey).push_back(static_cast<int32_t>(partkey));
        ++p.rows;
      },
      [](OffersPartial &into, OffersPartial &&from) {
        AppendLists(into.offers, from.offers);
        AppendLists(into.supp_parts, from.supp_parts);
        into.rows += from.rows;
      });
  const int64_t partsupp_rows = offers.rows;
  state.offers = std::make_shared<const TpchQ2Paged<std::vector<TpchQ2Offer>, 9>>(
      std::move(offers.offers));
  state.supp_parts = std::make_shared<const TpchQ2Paged<std::vector<int32_t>, 6>>(
      std::move(offers.supp_parts));
  BuildGroups(services, state);
  if (profiling) {
    EmitCustomProfile("build_partsupp_scan", partsupp_timer->us(),
                      partsupp_rows, (int64_t)state.groups->size(), 0);
  }
  return state;
}

std::shared_ptr<const TpchQ2State>
TpchQ2Module::LoadState(core::AuxEngineServices &services) const {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {core::TpchQ2Layout::kFull});
  for (const auto &image : snapshot.partitions) {
    if (image.partition != core::TpchQ2Layout::kFull)
      continue;
    const auto *typed = image.As<TpchQ2StateImage>();
    if (!typed || typed->kind != TpchQ2ImageKind::Full || !typed->state)
      return nullptr;
    return typed->state;
  }
  return nullptr;
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
