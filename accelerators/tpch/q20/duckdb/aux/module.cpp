#include "tpch/q20/duckdb/aux/module.hpp"
#include "tpch/q20/calibration_writes.hpp"
#include "prims/sql_date.hpp"
#include "prims/key_range_sort.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/parallel_sort.hpp"
#include "substrate/aux/core/partitioned_build.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <set>
#include <unordered_set>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

// Base pairs per read slice (~3000 us): a cache-missing series lookup is ~100 ns.
constexpr std::size_t kPairGrain = 32768;
// Partsupp rows per build slice (~1300 us): a merge step is ~20 ns.
constexpr std::size_t kPartsuppGrain = 65536;
// Series per maintenance slice (~16000 us): rebuilding one is ~1 us, so RF-sized commits stay
// inline.
constexpr std::size_t kSeriesGrain = 16384;
// Lineitem deltas per slice when keying and sorting them by pair (~1000 us).
constexpr std::size_t kSeriesSortGrain = 32768;
// Rows per sorted slice of the build (~5000 us): a stable sort of 64K small rows.
constexpr std::size_t kSortGrain = 65536;

using core::DateCell;
using core::IntCell;
using core::StringCell;

// (partkey, suppkey) packed 32/32: exact (no collisions) for every key pair
// PairKeyFits accepts -- TPC-H stays far inside it at any scale factor.
inline bool PairKeyFits(int64_t partkey, int64_t suppkey) {
  return partkey >= 0 && partkey <= INT64_C(0xFFFFFFFF) && suppkey >= 0 &&
         suppkey <= std::numeric_limits<int32_t>::max();
}
inline uint64_t MakePairKey(int64_t partkey, int64_t suppkey) {
  return (static_cast<uint64_t>(partkey) << 32) |
         static_cast<uint64_t>(static_cast<uint32_t>(suppkey));
}
inline void RequirePairKey(int64_t partkey, int64_t suppkey) {
  if (!PairKeyFits(partkey, suppkey))
    throw std::runtime_error("TPC-H Q20 aux: (partkey, suppkey) out of range");
}

std::shared_ptr<const TpchQ20StateImage>
MakeStateImage(const std::shared_ptr<const TpchQ20State> &state) {
  auto image = std::make_shared<TpchQ20StateImage>();
  image->kind = TpchQ20ImageKind::Full;
  image->state = state;
  return image;
}

const TpchQ20StateImage *AsQ20Image(const core::StateImage &image) {
  const auto *payload = image.As<TpchQ20StateImage>();
  return payload && payload->kind == TpchQ20ImageKind::Full && payload->state
             ? payload
             : nullptr;
}

bool AvailQtyGreaterThanHalfSum(int64_t availqty, int64_t sum_qty_raw) {
#if defined(__SIZEOF_INT128__)
  return static_cast<__int128_t>(availqty) * 200 >
         static_cast<__int128_t>(sum_qty_raw);
#else
  return availqty > sum_qty_raw / 200;
#endif
}

// SQL LIKE ('%': any run, '_': one byte), iterative: on a mismatch it
// resumes after the last '%' one byte further on, so it is never exponential.
bool SqlLikeAt(const char *text, std::size_t text_len, const char *pattern,
               std::size_t pattern_len, std::size_t text_pos,
               std::size_t pattern_pos) {
  std::size_t star = std::string::npos, star_text = 0;
  while (text_pos < text_len) {
    if (pattern_pos < pattern_len && pattern[pattern_pos] == '%') {
      star = pattern_pos++;
      star_text = text_pos;
    } else if (pattern_pos < pattern_len &&
               (pattern[pattern_pos] == '_' ||
                pattern[pattern_pos] == text[text_pos])) {
      ++text_pos;
      ++pattern_pos;
    } else if (star != std::string::npos) {
      pattern_pos = star + 1;
      text_pos = ++star_text;
    } else {
      return false;
    }
  }
  while (pattern_pos < pattern_len && pattern[pattern_pos] == '%')
    ++pattern_pos;
  return pattern_pos == pattern_len;
}

bool SqlLike(const std::string &text, const std::string &pattern) {
  return SqlLikeAt(text.data(), text.size(), pattern.data(), pattern.size(), 0,
                   0);
}

bool ReadLineitem(const core::AuxRow &row, int32_t &shipdate_days,
                  int64_t &partkey, int64_t &suppkey, int64_t &quantity_raw) {
  if (!DateCell(row, 0, shipdate_days) || !IntCell(row, 1, partkey) ||
      !IntCell(row, 2, suppkey))
    return false;
  return core::DecimalCellAtScale(row, 3, 2, quantity_raw);
}

bool ReadPart(const core::AuxRow &row, int64_t &partkey, std::string &name) {
  return IntCell(row, 0, partkey) && StringCell(row, 1, name);
}

bool ReadPartsupp(const core::AuxRow &row, int64_t &partkey, int64_t &suppkey,
                  int64_t &availqty) {
  if (!IntCell(row, 0, partkey) || !IntCell(row, 1, suppkey))
    return false;
  if (row.values.size() > 2)
    return IntCell(row, 2, availqty);
  return false;
}

bool ReadSupplier(const core::AuxRow &row, int64_t &suppkey, std::string &name,
                  std::string &address, int32_t &nationkey) {
  return IntCell(row, 0, suppkey) && StringCell(row, 1, name) &&
         StringCell(row, 2, address) && core::Int32Cell(row, 3, nationkey);
}

// Dense key-indexed arrays cover keys below max(kMinDense, 4 x row count);
// larger keys live in hash maps, so sparse keys never blow up memory.
constexpr int64_t kMinDense = int64_t{1} << 20;
int64_t DenseLimit(std::size_t rows) {
  return std::max<int64_t>(kMinDense, 4 * static_cast<int64_t>(rows));
}

// ---- bulk build: rows dealt into key-range buckets, each sorted on its own ----

using core::PartitionPieces;
using core::PieceList;
using core::PieceOffsets;

// Lowers *slot to `value` (slices write the same slot only for repeated keys).
void AtomicMin(uint32_t *slot, uint32_t value) {
  uint32_t seen = __atomic_load_n(slot, __ATOMIC_RELAXED);
  while (value < seen &&
         !__atomic_compare_exchange_n(slot, &seen, value, true, __ATOMIC_RELAXED,
                                      __ATOMIC_RELAXED)) {
  }
}

struct SeriesRow {
  uint64_t key;
  int32_t date;
  int64_t qty;
};

struct NameRow {
  int64_t key;
  const char *data;
  uint32_t len;
  std::string_view View() const { return {data, len}; }
};

struct SupplierRow {
  int64_t key;
  TpchQ20SupplierRecord record;
};

// The lineitem series of one sorted bucket, in the layout of TpchQ20Base.
struct SeriesBucket {
  std::vector<uint64_t> keys;
  std::vector<uint64_t> ends; // per series: end offset into `entries`
  std::vector<TpchQ20Base::Entry> entries;
  std::vector<std::pair<uint32_t, std::vector<TpchQ20DateQuantity>>> wide; // local idx
};

SeriesBucket GroupSeries(const std::vector<SeriesRow> &rows) {
  SeriesBucket out;
  std::vector<TpchQ20DateQuantity> run;
  for (std::size_t begin = 0, end = 0; begin < rows.size(); begin = end) {
    run.clear();
    for (end = begin; end < rows.size() && rows[end].key == rows[begin].key; ++end)
      run.push_back({rows[end].date, rows[end].qty});
    std::stable_sort(run.begin(), run.end(),
                     [](const TpchQ20DateQuantity &a, const TpchQ20DateQuantity &b) {
                       return a.shipdate_days < b.shipdate_days;
                     });
    const bool fits = std::all_of(run.begin(), run.end(), [](const TpchQ20DateQuantity &e) {
      return e.quantity_raw >= std::numeric_limits<int32_t>::min() &&
             e.quantity_raw <= std::numeric_limits<int32_t>::max();
    });
    if (fits) {
      for (const auto &e : run)
        out.entries.push_back({e.shipdate_days, static_cast<int32_t>(e.quantity_raw)});
    } else {
      out.wide.emplace_back(static_cast<uint32_t>(out.keys.size()), run);
    }
    out.keys.push_back(rows[begin].key);
    out.ends.push_back(out.entries.size());
  }
  return out;
}

class BaseBuilder {
public:
  explicit BaseBuilder(const core::AuxEngineServices &services)
      : services(services), base(std::make_shared<TpchQ20Base>()) {}

  // Every Adopt takes the rows of one table, sorted (bucket by bucket).
  void AdoptSeries(PieceList<SeriesRow> sorted) {
    std::vector<SeriesBucket> groups(sorted.size());
    core::ParallelEach(services, sorted.size(), 1, [&](std::size_t b) {
      groups[b] = GroupSeries(sorted[b]);
      std::vector<SeriesRow>().swap(sorted[b]);
    });
    std::vector<std::size_t> series_at(groups.size() + 1, 0), entries_at(groups.size() + 1, 0);
    for (std::size_t b = 0; b < groups.size(); ++b) {
      series_at[b + 1] = series_at[b] + groups[b].keys.size();
      entries_at[b + 1] = entries_at[b] + groups[b].entries.size();
    }
    if (series_at.back() > TpchQ20Base::kNoSeries)
      throw std::runtime_error("TPC-H Q20 aux: too many lineitem series");
    base->series_key.resize(series_at.back());
    base->series_off.assign(series_at.back() + 1, 0);
    base->series_entries.resize(entries_at.back());
    core::ParallelEach(services, groups.size(), 1, [&](std::size_t b) {
      const auto &g = groups[b];
      std::copy(g.keys.begin(), g.keys.end(), base->series_key.begin() + series_at[b]);
      for (std::size_t i = 0; i < g.ends.size(); ++i)
        base->series_off[series_at[b] + i + 1] = entries_at[b] + g.ends[i];
      std::copy(g.entries.begin(), g.entries.end(), base->series_entries.begin() + entries_at[b]);
    });
    for (std::size_t b = 0; b < groups.size(); ++b)
      for (auto &[local, run] : groups[b].wide)
        base->series_wide[static_cast<uint32_t>(series_at[b] + local)] = std::move(run);
  }

  void AdoptPartsupp(PieceList<TpchQ20PartsuppInput> sorted) {
    const auto at = PieceOffsets(sorted);
    ps.resize(at.back());
    core::ParallelEach(services, sorted.size(), 1, [&](std::size_t b) {
      std::size_t out = at[b];
      for (const auto &row : sorted[b]) {
        auto &e = ps[out++];
        e.partkey = row.partkey;
        e.availqty = row.availqty;
        e.suppkey = static_cast<int32_t>(row.suppkey);
      }
      std::vector<TpchQ20PartsuppInput>().swap(sorted[b]);
    });
  }

  // Part names sorted bytewise (and the ranks they give each part key).
  void AdoptNames(const PieceList<NameRow> &sorted) {
    const auto at = PieceOffsets(sorted);
    const std::size_t n = at.back();
    std::vector<std::size_t> bytes_at(sorted.size() + 1, 0);
    for (std::size_t b = 0; b < sorted.size(); ++b) {
      std::size_t bytes = 0;
      for (const auto &row : sorted[b])
        bytes += row.len;
      bytes_at[b + 1] = bytes_at[b] + bytes;
    }
    auto &base_ref = *base;
    base_ref.name_arena.resize(bytes_at.back());
    base_ref.name_off.resize(n + 1);
    base_ref.name_partkey.resize(n);
    core::ParallelEach(services, sorted.size(), 1, [&](std::size_t b) {
      std::size_t rank = at[b], off = bytes_at[b];
      for (const auto &row : sorted[b]) {
        std::memcpy(&base_ref.name_arena[off], row.data, row.len);
        base_ref.name_off[rank] = off;
        base_ref.name_partkey[rank++] = row.key;
        off += row.len;
      }
    });
    base_ref.name_off[n] = bytes_at.back();
    AssignRanks();
  }

  void AdoptSuppliers(PieceList<SupplierRow> sorted) {
    const auto at = PieceOffsets(sorted);
    auto &b = *base;
    b.supplier_keys.resize(at.back());
    b.supplier_records.resize(at.back());
    core::ParallelEach(services, sorted.size(), 1, [&](std::size_t piece) {
      std::size_t out = at[piece];
      for (auto &row : sorted[piece]) {
        b.supplier_keys[out] = row.key;
        b.supplier_records[out++] = std::move(row.record);
      }
      std::vector<SupplierRow>().swap(sorted[piece]);
    });
    const int64_t dense = DenseLimit(b.supplier_keys.size());
    const int64_t max_key = MaxKeyBelow(b.supplier_keys, dense);
    b.supp_nation.assign(static_cast<std::size_t>(max_key + 1), -1);
    b.supp_rank.assign(static_cast<std::size_t>(max_key + 1), TpchQ20Base::kNoRank);
    Slices(b.supplier_keys.size(), kPartsuppGrain,
           [&](std::size_t, std::size_t begin, std::size_t end) {
             for (std::size_t r = begin; r < end; ++r) {
               const int64_t key = b.supplier_keys[r];
               if (key > max_key)
                 continue;
               b.supp_nation[static_cast<std::size_t>(key)] = b.supplier_records[r].nationkey;
               b.supp_rank[static_cast<std::size_t>(key)] = static_cast<uint32_t>(r);
             }
           });
    for (std::size_t r = 0; r < b.supplier_keys.size(); ++r)
      if (b.supplier_keys[r] > max_key)
        b.sparse_supp_rank[b.supplier_keys[r]] = static_cast<uint32_t>(r);
  }

  void AddNation(int32_t key, const std::string &name) {
    if (key < 0)
      return;
    base->nations[key] = name;
    base->nation_name_index[name].push_back(key);
  }

  std::shared_ptr<const TpchQ20Base> Finish() {
    ResolveSeriesIndexes();
    FinishPairs();
    return std::move(base);
  }

private:
  template <class Fn> void Slices(std::size_t n, std::size_t grain, Fn &&fn) const {
    core::ParallelSlices(services, n, grain, fn);
  }

  struct PsRow {
    int64_t partkey = 0;
    int64_t availqty = 0;
    int32_t suppkey = 0;
    uint32_t series_idx = TpchQ20Base::kNoSeries;
  };

  // The largest key below `dense`, or -1.
  int64_t MaxKeyBelow(const std::vector<int64_t> &keys, int64_t dense) const {
    int64_t max_key = -1;
    core::ParallelReduce(
        services, keys.size(), kPartsuppGrain, max_key,
        [&](int64_t &acc, std::size_t begin, std::size_t end) {
          for (std::size_t i = begin; i < end; ++i)
            if (keys[i] < dense)
              acc = std::max(acc, keys[i]);
        },
        [](int64_t &into, int64_t &&from) { into = std::max(into, from); });
    return max_key;
  }

  // part_rank: a part key's first (lowest) rank; further ranks of a key go to
  // extra_ranks, keys past the dense range to sparse_rank.
  void AssignRanks() {
    auto &b = *base;
    const std::size_t n = b.name_partkey.size();
    const int64_t dense = DenseLimit(n);
    const int64_t max_key = MaxKeyBelow(b.name_partkey, dense);
    b.part_rank.assign(static_cast<std::size_t>(max_key + 1), TpchQ20Base::kNoRank);
    Slices(n, kPartsuppGrain, [&](std::size_t, std::size_t begin, std::size_t end) {
      for (std::size_t r = begin; r < end; ++r)
        if (b.name_partkey[r] < dense)
          AtomicMin(&b.part_rank[static_cast<std::size_t>(b.name_partkey[r])],
                    static_cast<uint32_t>(r));
    });
    // Ranks that are not their key's first, in rank order (normally none).
    const std::size_t slices = core::SliceCount(services, n, kPartsuppGrain);
    std::vector<std::vector<uint32_t>> later(slices);
    core::RunSlices(services, slices, n, [&](std::size_t slice, std::size_t begin, std::size_t end) {
      for (std::size_t r = begin; r < end; ++r) {
        const int64_t key = b.name_partkey[r];
        if (key >= dense || b.part_rank[static_cast<std::size_t>(key)] != r)
          later[slice].push_back(static_cast<uint32_t>(r)); // sparse, or not the first
      }
    });
    for (const auto &ranks : later)
      for (const uint32_t rank : ranks) {
        const int64_t key = b.name_partkey[rank];
        const bool first = key >= dense && b.sparse_rank.emplace(key, rank).second;
        if (!first)
          b.extra_ranks[key].push_back(rank);
      }
  }

  // Resolves each partsupp row's lineitem series; both sides are sorted by pair
  // key, each slice starts its merge cursor at its first key.
  void ResolveSeriesIndexes() {
    const auto &keys = base->series_key;
    Slices(ps.size(), kPartsuppGrain, [&](std::size_t, std::size_t begin, std::size_t end) {
      if (begin == end)
        return;
      const auto &first = ps[begin];
      std::size_t j = std::lower_bound(keys.begin(), keys.end(),
                                       MakePairKey(first.partkey, first.suppkey)) -
                      keys.begin();
      for (std::size_t i = begin; i < end; ++i) {
        auto &e = ps[i];
        const uint64_t key = MakePairKey(e.partkey, e.suppkey);
        while (j < keys.size() && keys[j] < key)
          ++j;
        if (j < keys.size() && keys[j] == key)
          e.series_idx = static_cast<uint32_t>(j);
      }
    });
  }

  // Lays the partsupp rows out nation-major (see TpchQ20Base::pairs): one
  // entry per (row, base name of its part), bucketed by the supplier's slot,
  // then each slot sorted by (rank, suppkey). Slices count, then fill, in row
  // order, so the layout is the serial one at any thread count.
  void FinishPairs() {
    auto &b = *base;
    // Slots: one per distinct base nation of a supplier, then "no supplier".
    std::set<int32_t> nations;
    core::ParallelReduce(
        services, b.supplier_records.size(), kPartsuppGrain, nations,
        [&](std::set<int32_t> &acc, std::size_t begin, std::size_t end) {
          for (std::size_t r = begin; r < end; ++r)
            acc.insert(b.supplier_records[r].nationkey);
        },
        [](std::set<int32_t> &into, std::set<int32_t> &&from) { into.merge(from); });
    for (const int32_t nationkey : nations)
      b.nation_slot.emplace(nationkey, static_cast<uint32_t>(b.nation_slot.size()));
    const std::size_t slots = nations.size() + 1;
    const auto no_supplier = static_cast<uint32_t>(slots - 1);
    std::vector<uint32_t> dense_slot(b.supp_nation.size(), no_supplier);
    Slices(b.supplier_keys.size(), kPartsuppGrain,
           [&](std::size_t, std::size_t begin, std::size_t end) {
             for (std::size_t r = begin; r < end; ++r)
               if (static_cast<std::size_t>(b.supplier_keys[r]) < dense_slot.size())
                 dense_slot[static_cast<std::size_t>(b.supplier_keys[r])] =
                     b.nation_slot.at(b.supplier_records[r].nationkey);
           });
    const auto slot_of = [&](int32_t suppkey) -> uint32_t {
      if (static_cast<std::size_t>(suppkey) < dense_slot.size())
        return dense_slot[static_cast<std::size_t>(suppkey)];
      const auto *rec = b.BaseSupplier(suppkey);
      return rec ? b.nation_slot.at(rec->nationkey) : no_supplier;
    };
    // f(rank) for every base name of the part; false when it has none.
    const auto for_each_rank = [&](int64_t partkey, auto &&f) {
      const uint32_t first = b.RankOf(partkey);
      if (first == TpchQ20Base::kNoRank)
        return false;
      f(first);
      if (!b.extra_ranks.empty()) {
        const auto it = b.extra_ranks.find(partkey);
        if (it != b.extra_ranks.end())
          for (const uint32_t r : it->second)
            f(r);
      }
      return true;
    };

    const std::size_t rows = ps.size();
    const std::size_t slices = core::SliceCount(services, rows, kPartsuppGrain);
    std::vector<std::vector<std::size_t>> count(slices, std::vector<std::size_t>(slots, 0));
    std::vector<std::vector<TpchQ20Base::OrphanPair>> orphans(slices);
    core::RunSlices(services, slices, rows, [&](std::size_t slice, std::size_t begin, std::size_t end) {
      for (std::size_t i = begin; i < end; ++i) {
        const auto &row = ps[i];
        const uint32_t slot = slot_of(row.suppkey);
        if (!for_each_rank(row.partkey, [&](uint32_t) { ++count[slice][slot]; }))
          orphans[slice].push_back({row.partkey, row.availqty, row.suppkey, row.series_idx});
      }
    });
    // cursor[slice][slot]: where the slice writes its next entry of the slot.
    std::vector<std::size_t> offsets(slots + 1, 0);
    auto cursor = count;
    for (std::size_t slot = 0; slot < slots; ++slot) {
      std::size_t at = offsets[slot];
      for (std::size_t slice = 0; slice < slices; ++slice) {
        cursor[slice][slot] = at;
        at += count[slice][slot];
      }
      offsets[slot + 1] = at;
    }
    if (offsets[slots] > std::numeric_limits<uint32_t>::max())
      throw std::runtime_error("TPC-H Q20 aux: too many partsupp pairs");
    for (auto &slice_orphans : orphans)
      b.orphan_pairs.insert(b.orphan_pairs.end(), slice_orphans.begin(), slice_orphans.end());
    b.slot_off.assign(offsets.begin(), offsets.end());
    b.pairs.resize(offsets[slots]);
    std::vector<std::vector<std::pair<uint64_t, int64_t>>> wide(slices);
    core::RunSlices(services, slices, rows, [&](std::size_t slice, std::size_t begin, std::size_t end) {
      for (std::size_t i = begin; i < end; ++i) {
        const auto &row = ps[i];
        const uint32_t slot = slot_of(row.suppkey);
        for_each_rank(row.partkey, [&](uint32_t rank) {
          auto &e = b.pairs[cursor[slice][slot]++];
          e.rank = rank;
          e.suppkey = row.suppkey;
          e.series_idx = row.series_idx;
          if (row.availqty > std::numeric_limits<int32_t>::min() &&
              row.availqty <= std::numeric_limits<int32_t>::max()) {
            e.availqty = static_cast<int32_t>(row.availqty);
          } else {
            e.availqty = TpchQ20Base::kWideAvail;
            wide[slice].emplace_back(MakePairKey(row.partkey, row.suppkey), row.availqty);
          }
        });
      }
    });
    for (const auto &slice_wide : wide)
      for (const auto &[key, availqty] : slice_wide)
        b.avail_wide[key] = availqty;
    ps.clear();
    ps.shrink_to_fit();
    Slices(slots, 1, [&](std::size_t, std::size_t begin, std::size_t end) {
      for (std::size_t s = begin; s < end; ++s)
        std::sort(b.pairs.begin() + static_cast<std::ptrdiff_t>(b.slot_off[s]),
                  b.pairs.begin() + static_cast<std::ptrdiff_t>(b.slot_off[s + 1]),
                  [](const TpchQ20Base::PairEntry &x, const TpchQ20Base::PairEntry &y) {
                    return x.rank != y.rank ? x.rank < y.rank : x.suppkey < y.suppkey;
                  });
    });
  }

  const core::AuxEngineServices &services;
  std::shared_ptr<TpchQ20Base> base;
  // partsupp rows in (partkey, suppkey) order; consumed by FinishPairs.
  std::vector<PsRow> ps;
};

// Inserts or removes one lineitem's (shipdate, quantity) in a pair's series.
void ApplySeriesDelta(std::vector<TpchQ20DateQuantity> &series,
                      const TpchQ20LineitemDelta &delta) {
  if (delta.sign > 0) {
    const auto pos = std::upper_bound(
        series.begin(), series.end(), delta.shipdate_days,
        [](int32_t d, const TpchQ20DateQuantity &e) { return d < e.shipdate_days; });
    series.insert(pos, {delta.shipdate_days, delta.quantity_raw});
    return;
  }
  auto it = std::lower_bound(
      series.begin(), series.end(), delta.shipdate_days,
      [](const TpchQ20DateQuantity &e, int32_t d) { return e.shipdate_days < d; });
  for (; it != series.end() && it->shipdate_days == delta.shipdate_days; ++it) {
    if (it->quantity_raw == delta.quantity_raw) {
      series.erase(it);
      break;
    }
  }
}

// Effective (merged) views used by Encode.
std::vector<TpchQ20DateQuantity> BaseSeriesCopy(const TpchQ20Base &base,
                                                uint32_t idx) {
  std::vector<TpchQ20DateQuantity> out;
  const auto wit = base.series_wide.find(idx);
  if (wit != base.series_wide.end())
    return wit->second;
  for (uint64_t i = base.series_off[idx]; i < base.series_off[idx + 1]; ++i)
    out.push_back({base.series_entries[i].date, base.series_entries[i].qty});
  return out;
}

// The part-name LIKE pattern. The literal text before the first wildcard
// bounds a contiguous range of the sorted base names [RankLo, RankHi). A
// pattern that is exactly that literal plus '%' (the TPC-H form) needs no
// further test; any other is tested only on the ranks a read visits.
class PartNameFilter {
public:
  PartNameFilter(const TpchQ20Base &base, const std::string &pattern)
      : base_(base), pattern_(pattern) {
    const std::size_t wild = pattern.find_first_of("%_");
    prefix_ = pattern.substr(0, wild);
    prefix_only_ = wild != std::string::npos && wild + 1 == pattern.size() &&
                   pattern[wild] == '%';
    const std::size_t name_count = base.name_partkey.size();
    std::size_t lo = 0, hi = name_count;
    while (lo < hi) { // first name >= prefix
      const std::size_t mid = lo + (hi - lo) / 2;
      if (base.PartName(mid) < std::string_view(prefix_))
        lo = mid + 1;
      else
        hi = mid;
    }
    rank_lo_ = static_cast<uint32_t>(lo);
    hi = name_count;
    while (lo < hi) { // first name past the names starting with prefix
      const std::size_t mid = lo + (hi - lo) / 2;
      if (HasPrefix(base.PartName(mid)))
        lo = mid + 1;
      else
        hi = mid;
    }
    rank_hi_ = static_cast<uint32_t>(lo);
  }

  uint32_t RankLo() const { return rank_lo_; }
  uint32_t RankHi() const { return rank_hi_; }

  bool Matches(std::string_view name) const {
    if (prefix_only_)
      return HasPrefix(name);
    return SqlLikeAt(name.data(), name.size(), pattern_.data(), pattern_.size(), 0, 0);
  }
  bool RankOk(uint32_t rank) const { return prefix_only_ || Matches(base_.PartName(rank)); }

private:
  bool HasPrefix(std::string_view name) const {
    return name.size() >= prefix_.size() &&
           std::memcmp(name.data(), prefix_.data(), prefix_.size()) == 0;
  }

  const TpchQ20Base &base_;
  const std::string &pattern_;
  std::string prefix_;
  bool prefix_only_ = false;
  uint32_t rank_lo_ = 0;
  uint32_t rank_hi_ = 0;
};

// The suppliers of the target nations that have a part/supplier pair with
// lineitems in the shipdate window and availqty above half their quantity.
class QualifyingSuppliers {
public:
  QualifyingSuppliers(const TpchQ20State &state, int32_t start_date_days,
                      int32_t end_date_days, const PartNameFilter &names,
                      std::vector<int32_t> target_nations, core::AuxEngineServices &services)
      : state_(state), base_(*state.base), start_date_days_(start_date_days),
        end_date_days_(end_date_days), names_(names),
        target_nations_(std::move(target_nations)), services_(services),
        overlay_parts_(state.parts.size() != 0), overlay_ps_(state.partsupps.size() != 0),
        overlay_series_(!state.series.Empty()),
        overlay_suppliers_(state.suppliers.size() != 0) {}

  bool IsTarget(int32_t nationkey) const {
    return nationkey >= 0 && std::find(target_nations_.begin(), target_nations_.end(),
                                       nationkey) != target_nations_.end();
  }

  // Sorted and deduplicated.
  std::vector<int64_t> Find() {
    WalkBaseNations();
    EvaluateChangedParts();
    WalkMovedSuppliers();
    std::sort(suppkeys_.begin(), suppkeys_.end());
    suppkeys_.erase(std::unique(suppkeys_.begin(), suppkeys_.end()), suppkeys_.end());
    return std::move(suppkeys_);
  }

private:
  // A part whose name or partsupp list changed since the base is evaluated
  // from the overlays (EvaluateChangedParts), never from its base pairs.
  bool PartChanged(int64_t partkey) const {
    return (overlay_parts_ && state_.parts.Find(partkey)) ||
           (overlay_ps_ && state_.partsupps.Find(partkey));
  }

  // The pair qualifies when it has lineitems in the window and availqty
  // exceeds half their quantity. The supplier's nation is checked by the caller.
  void Evaluate(std::vector<int64_t> &out, int64_t partkey, int64_t suppkey, int64_t availqty,
                uint32_t series_idx, bool resolve_idx) const {
    const uint64_t pair_key = MakePairKey(partkey, suppkey);
    int64_t sum = 0;
    bool has = false;
    const TpchQ20SeriesView overlay =
        overlay_series_ ? state_.series.Find(pair_key) : TpchQ20SeriesView{};
    if (overlay.found) {
      for (const auto &e : overlay) {
        if (e.shipdate_days < start_date_days_)
          continue;
        if (e.shipdate_days >= end_date_days_)
          break;
        has = true;
        sum += e.quantity_raw;
      }
    } else {
      if (resolve_idx)
        series_idx = base_.FindSeries(pair_key);
      if (series_idx == TpchQ20Base::kNoSeries)
        return;
      has = base_.WindowSum(series_idx, start_date_days_, end_date_days_, sum);
    }
    if (has && AvailQtyGreaterThanHalfSum(availqty, sum))
      out.push_back(suppkey);
  }

  // Walks pairs [b, e) of a slot's name range; `keep` filters suppkeys.
  template <class Keep>
  void WalkPairs(std::vector<int64_t> &out, std::size_t b, std::size_t e, Keep &&keep) const {
    constexpr std::size_t kAhead = 16;
    const auto *pairs = base_.pairs.data();
    for (std::size_t i = b; i < e; ++i) {
      if (i + kAhead < e && pairs[i + kAhead].series_idx != TpchQ20Base::kNoSeries)
        __builtin_prefetch(&base_.series_off[pairs[i + kAhead].series_idx]);
      if (i + kAhead / 2 < e && pairs[i + kAhead / 2].series_idx != TpchQ20Base::kNoSeries)
        __builtin_prefetch(base_.series_entries.data() +
                           base_.series_off[pairs[i + kAhead / 2].series_idx]);
      if (overlay_series_) {
        if (i + kAhead < e) {
          const auto &q = pairs[i + kAhead];
          state_.series.PrefetchSlot(MakePairKey(base_.name_partkey[q.rank], q.suppkey));
        }
        if (i + kAhead / 2 < e) {
          const auto &q = pairs[i + kAhead / 2];
          state_.series.PrefetchData(MakePairKey(base_.name_partkey[q.rank], q.suppkey));
        }
      }
      const auto &p = pairs[i];
      if (!keep(p.suppkey) || !names_.RankOk(p.rank))
        continue;
      const int64_t partkey = base_.name_partkey[p.rank];
      if ((overlay_parts_ || overlay_ps_) && PartChanged(partkey))
        continue;
      Evaluate(out, partkey, p.suppkey, base_.Avail(p), p.series_idx, false);
    }
  }

  // Slices may append in any grouping: the result is sorted afterwards.
  template <class Keep> void WalkSlot(uint32_t slot, Keep &&keep) {
    const auto [b, e] = base_.RankRange(slot, names_.RankLo(), names_.RankHi());
    core::ParallelReduce(
        services_, e - b, kPairGrain, suppkeys_,
        [&](std::vector<int64_t> &out, std::size_t begin, std::size_t end) {
          WalkPairs(out, b + begin, b + end, keep);
        },
        [](std::vector<int64_t> &into, std::vector<int64_t> &&from) {
          into.insert(into.end(), from.begin(), from.end());
        });
  }

  // Base pairs whose supplier's base nation is a target nation (minus
  // suppliers since moved out or deleted).
  void WalkBaseNations() {
    for (const int32_t nationkey : target_nations_) {
      const uint32_t slot = base_.SlotOfNation(nationkey);
      if (slot == TpchQ20Base::kNoSlot)
        continue;
      if (overlay_suppliers_)
        WalkSlot(slot, [&](int32_t suppkey) { return IsTarget(state_.SupplierNation(suppkey)); });
      else
        WalkSlot(slot, [](int32_t) { return true; });
    }
  }

  // Parts whose name or partsupp list changed since the base.
  void EvaluateChangedParts() {
    if (!overlay_parts_ && !overlay_ps_)
      return;
    std::vector<int64_t> changed;
    state_.parts.ForEach([&](uint64_t k, const TpchQ20PartOverlay &) {
      changed.push_back(static_cast<int64_t>(k));
    });
    state_.partsupps.ForEach([&](uint64_t k, const auto &) {
      changed.push_back(static_cast<int64_t>(k));
    });
    std::sort(changed.begin(), changed.end());
    changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
    for (const int64_t partkey : changed) {
      if (!ChangedPartNameMatches(partkey))
        continue;
      const auto consider = [&](int64_t suppkey, int64_t availqty, uint32_t series_idx,
                                bool resolve_idx) {
        if (IsTarget(state_.SupplierNation(suppkey)))
          Evaluate(suppkeys_, partkey, suppkey, availqty, series_idx, resolve_idx);
      };
      if (const auto *list = overlay_ps_ ? state_.partsupps.Find(partkey) : nullptr) {
        for (const auto &p : *list)
          consider(p.suppkey, p.availqty, TpchQ20Base::kNoSeries, true);
      } else {
        base_.ForEachBasePair(partkey, [&](int64_t suppkey, int64_t availqty,
                                           uint32_t series_idx) {
          consider(suppkey, availqty, series_idx, false);
        });
      }
    }
  }

  bool ChangedPartNameMatches(int64_t partkey) const {
    if (const auto *overlay = state_.parts.Find(partkey))
      return overlay->present && names_.Matches(overlay->name);
    const uint32_t rank = base_.RankOf(partkey);
    if (rank == TpchQ20Base::kNoRank)
      return false;
    if (names_.Matches(base_.PartName(rank)))
      return true;
    const auto extra = base_.extra_ranks.find(partkey);
    if (extra == base_.extra_ranks.end())
      return false;
    for (const uint32_t extra_rank : extra->second)
      if (names_.Matches(base_.PartName(extra_rank)))
        return true;
    return false;
  }

  // Suppliers now in a target nation whose base nation is not (moved, or new):
  // their base pairs sit in their base nation's slot.
  void WalkMovedSuppliers() {
    if (!overlay_suppliers_)
      return;
    std::vector<std::pair<uint32_t, int32_t>> moved; // (base slot, suppkey)
    state_.suppliers.ForEach([&](uint64_t k, const TpchQ20SupplierOverlay &overlay) {
      if (!overlay.present || !IsTarget(overlay.record.nationkey))
        return;
      const auto suppkey = static_cast<int64_t>(k);
      const int32_t base_nation = base_.BaseNation(suppkey);
      const bool in_base = base_nation >= 0 || base_.BaseSupplier(suppkey) != nullptr;
      if (in_base && IsTarget(base_nation))
        return; // already walked by WalkBaseNations
      const uint32_t slot = in_base ? base_.SlotOfNation(base_nation) : base_.NoNationSlot();
      if (slot != TpchQ20Base::kNoSlot)
        moved.emplace_back(slot, static_cast<int32_t>(suppkey));
    });
    std::sort(moved.begin(), moved.end());
    for (std::size_t i = 0; i < moved.size();) {
      std::size_t j = i;
      while (j < moved.size() && moved[j].first == moved[i].first)
        ++j;
      WalkSlot(moved[i].first, [&](int32_t suppkey) {
        return std::binary_search(moved.begin() + static_cast<std::ptrdiff_t>(i),
                                  moved.begin() + static_cast<std::ptrdiff_t>(j),
                                  std::pair<uint32_t, int32_t>{moved[i].first, suppkey});
      });
      i = j;
    }
  }

  const TpchQ20State &state_;
  const TpchQ20Base &base_;
  const int32_t start_date_days_;
  const int32_t end_date_days_;
  const PartNameFilter &names_;
  const std::vector<int32_t> target_nations_;
  core::AuxEngineServices &services_;
  const bool overlay_parts_;
  const bool overlay_ps_;
  const bool overlay_series_;
  const bool overlay_suppliers_;
  std::vector<int64_t> suppkeys_;
};

struct OutputSupplier {
  const TpchQ20SupplierRecord *record;
  uint32_t rank; // TpchQ20Base::kNoRank: overlay record
};

// The qualifying suppliers still in a target nation, in name order. A base
// supplier's name rank is its index in the name-sorted records, so without
// supplier changes this sorts integers and reads the records in memory order;
// a changed supplier (overlay record, no rank) compares by name, after base
// suppliers of an equal name.
std::vector<OutputSupplier> InNameOrder(const TpchQ20State &state,
                                        const QualifyingSuppliers &qualifying,
                                        const std::vector<int64_t> &suppkeys) {
  const TpchQ20Base &base = *state.base;
  const bool overlay_suppliers = state.suppliers.size() != 0;
  std::vector<OutputSupplier> rows;
  rows.reserve(suppkeys.size());
  for (const auto suppkey : suppkeys) {
    const TpchQ20SupplierOverlay *overlay =
        overlay_suppliers ? state.suppliers.Find(suppkey) : nullptr;
    OutputSupplier row{nullptr, TpchQ20Base::kNoRank};
    if (overlay) {
      if (overlay->present)
        row.record = &overlay->record;
    } else {
      row.rank = base.SupplierRank(suppkey);
      if (row.rank != TpchQ20Base::kNoRank)
        row.record = &base.supplier_records[row.rank];
    }
    if (row.record && qualifying.IsTarget(row.record->nationkey))
      rows.push_back(row);
  }
  std::sort(rows.begin(), rows.end(), [](const OutputSupplier &a, const OutputSupplier &b) {
    constexpr uint32_t kNone = TpchQ20Base::kNoRank;
    if (a.rank != kNone && b.rank != kNone)
      return a.rank < b.rank;
    const int c = a.record->name.compare(b.record->name);
    if (c != 0)
      return c < 0;
    return a.rank != kNone && b.rank == kNone;
  });
  return rows;
}

void WriteSuppliers(const std::vector<OutputSupplier> &suppliers, core::AuxRowSink &sink) {
  sink.Begin(core::OutputSchema{});
  // One two-cell row buffer for every row: its strings keep their capacity.
  core::AuxRow out;
  out.values.resize(2);
  for (auto &cell : out.values) {
    cell.type = core::AuxType::String;
    cell.is_null = false;
    cell.payload = std::string();
  }
  for (const auto &supplier : suppliers) {
    std::get<std::string>(out.values[0].payload).assign(supplier.record->name);
    std::get<std::string>(out.values[1].payload).assign(supplier.record->address);
    sink.WriteRow(out);
  }
  sink.End();
}

} // namespace

uint32_t TpchQ20Base::FindSeries(uint64_t pair_key) const {
  const auto it =
      std::lower_bound(series_key.begin(), series_key.end(), pair_key);
  if (it == series_key.end() || *it != pair_key)
    return kNoSeries;
  return static_cast<uint32_t>(it - series_key.begin());
}

bool TpchQ20Base::WindowSum(uint32_t idx, int32_t lo, int32_t hi,
                            int64_t &sum) const {
  sum = 0;
  bool has = false;
  if (!series_wide.empty()) {
    const auto wit = series_wide.find(idx);
    if (wit != series_wide.end()) {
      for (const auto &e : wit->second) {
        if (e.shipdate_days < lo)
          continue;
        if (e.shipdate_days >= hi)
          break;
        has = true;
        sum += e.quantity_raw;
      }
      return has;
    }
  }
  const Entry *it = series_entries.data() + series_off[idx];
  const Entry *end = series_entries.data() + series_off[idx + 1];
  for (; it != end; ++it) {
    if (it->date < lo)
      continue;
    if (it->date >= hi)
      break;
    has = true;
    sum += it->qty;
  }
  return has;
}

int32_t TpchQ20Base::BaseNation(int64_t suppkey) const {
  if (suppkey >= 0 && static_cast<std::size_t>(suppkey) < supp_nation.size())
    return supp_nation[static_cast<std::size_t>(suppkey)];
  const auto *rec = BaseSupplier(suppkey);
  return rec ? rec->nationkey : -1;
}

std::pair<std::size_t, std::size_t>
TpchQ20Base::RankRange(uint32_t slot, uint32_t rank_lo, uint32_t rank_hi) const {
  const PairEntry *first = pairs.data() + slot_off[slot];
  const PairEntry *last = pairs.data() + slot_off[slot + 1];
  const auto by_rank = [](const PairEntry &e, uint32_t r) { return e.rank < r; };
  const PairEntry *b = std::lower_bound(first, last, rank_lo, by_rank);
  const PairEntry *e = std::lower_bound(b, last, rank_hi, by_rank);
  return {static_cast<std::size_t>(b - pairs.data()),
          static_cast<std::size_t>(e - pairs.data())};
}

int64_t TpchQ20Base::Avail(const PairEntry &e) const {
  if (e.availqty != kWideAvail)
    return e.availqty;
  return avail_wide.at(MakePairKey(name_partkey[e.rank], e.suppkey));
}

int32_t TpchQ20State::SupplierNation(int64_t suppkey) const {
  if (const auto *ov = suppliers.Find(suppkey))
    return ov->present ? ov->record.nationkey : -1;
  return base->BaseNation(suppkey);
}

const TpchQ20SupplierRecord *
TpchQ20State::SupplierRecord(int64_t suppkey) const {
  if (const auto *ov = suppliers.Find(suppkey))
    return ov->present ? &ov->record : nullptr;
  return base->BaseSupplier(suppkey);
}

bool TpchQ20State::FindSupplier(int64_t suppkey,
                                TpchQ20SupplierRecord &out) const {
  if (const auto *ov = suppliers.Find(suppkey)) {
    if (!ov->present)
      return false;
    out = ov->record;
    return true;
  }
  const auto *rec = base->BaseSupplier(suppkey);
  if (!rec)
    return false;
  out = *rec;
  return true;
}

bool TpchQ20State::PairWindowSum(uint64_t pair_key, int32_t lo, int32_t hi,
                                 int64_t &sum) const {
  if (!series.Empty()) {
    if (const auto ov = series.Find(pair_key); ov.found) {
      sum = 0;
      bool has = false;
      for (const auto &e : ov) {
        if (e.shipdate_days < lo)
          continue;
        if (e.shipdate_days >= hi)
          break;
        has = true;
        sum += e.quantity_raw;
      }
      return has;
    }
  }
  const uint32_t idx = base->FindSeries(pair_key);
  if (idx == TpchQ20Base::kNoSeries)
    return false;
  return base->WindowSum(idx, lo, hi, sum);
}

std::vector<int32_t>
TpchQ20State::FindNationKeysByName(const std::string &name) const {
  const auto it = base->nation_name_index.find(name);
  if (it != base->nation_name_index.end())
    return it->second;
  return {};
}

bool TpchQ20TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ20TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  lineitem_deltas.clear();
  partsupp_deltas.clear();
  part_name_deltas.clear();
  supplier_deltas.clear();
}
bool TpchQ20TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
void TpchQ20TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  lineitem_deltas.clear();
  partsupp_deltas.clear();
  part_name_deltas.clear();
  supplier_deltas.clear();
}
const std::vector<TpchQ20LineitemDelta> &
TpchQ20TxnBuffer::LineitemDeltas() const {
  return lineitem_deltas;
}
const std::vector<TpchQ20PartsuppDelta> &
TpchQ20TxnBuffer::PartsuppDeltas() const {
  return partsupp_deltas;
}
const std::vector<TpchQ20PartNameDelta> &
TpchQ20TxnBuffer::PartNameDeltas() const {
  return part_name_deltas;
}
const std::vector<TpchQ20SupplierDelta> &
TpchQ20TxnBuffer::SupplierDeltas() const {
  return supplier_deltas;
}
void TpchQ20TxnBuffer::AddLineitemDelta(TpchQ20LineitemDelta delta) {
  pending = true;
  lineitem_deltas.push_back(delta);
}
void TpchQ20TxnBuffer::AddPartsuppDelta(TpchQ20PartsuppDelta delta) {
  pending = true;
  partsupp_deltas.push_back(delta);
}
void TpchQ20TxnBuffer::AddPartNameDelta(TpchQ20PartNameDelta delta) {
  pending = true;
  part_name_deltas.push_back(std::move(delta));
}
void TpchQ20TxnBuffer::AddSupplierDelta(TpchQ20SupplierDelta delta) {
  pending = true;
  supplier_deltas.push_back(std::move(delta));
}

core::AcceleratorId TpchQ20Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ20Module::Dependencies() const {
  return {{config.lineitem_table, {0, 1, 2, 3}, {10, 1, 2, 4}},
          {config.part_table, {0, 1}, {0, 1}},
          {config.partsupp_table, {0, 1, 2}, {0, 1, 2}},
          {config.supplier_table, {0, 1, 2, 3}, {0, 1, 2, 3}},
          {config.nation_table, {0, 1}, {0, 1}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ20Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ20TxnBuffer>();
}

void TpchQ20Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  auto state = std::make_shared<const TpchQ20State>(BuildState(services));
  builder.Write(core::StateImage::Make(
      TpchQ20Layout::kAccelerator, TpchQ20Layout::kFullState, 0,
      MakeStateImage(state)));
  builder.Finish();
}

core::InvalidationPlan
TpchQ20Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table ||
      mutation.table == config.part_table ||
      mutation.table == config.partsupp_table ||
      mutation.table == config.supplier_table ||
      mutation.table == config.nation_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  return {};
}

void TpchQ20Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ20TxnBuffer &>(base_buffer);

  if (mutation.table == config.lineitem_table) {
    auto add = [&](const core::AuxRow &row, int8_t sign) {
      int32_t shipdate = 0;
      int64_t partkey = 0, suppkey = 0, quantity = 0;
      if (!ReadLineitem(row, shipdate, partkey, suppkey, quantity) ||
          !PairKeyFits(partkey, suppkey)) {
        buffer.MarkRebuild();
        return;
      }
      buffer.AddLineitemDelta({shipdate, partkey, suppkey, quantity, sign});
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
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        add(mutation.old_rows[i], -1);
        add(mutation.new_rows[i], 1);
      }
      break;
    }
    return;
  }

  if (mutation.table == config.partsupp_table) {
    auto add = [&](const core::AuxRow &row, int8_t sign) {
      int64_t partkey = 0, suppkey = 0, availqty = 0;
      if (!ReadPartsupp(row, partkey, suppkey, availqty) ||
          !PairKeyFits(partkey, suppkey)) {
        buffer.MarkRebuild();
        return;
      }
      buffer.AddPartsuppDelta({partkey, suppkey, availqty, sign});
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
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        add(mutation.old_rows[i], -1);
        add(mutation.new_rows[i], 1);
      }
      break;
    }
    return;
  }

  if (mutation.table == config.part_table) {
    auto add = [&](const core::AuxRow &row, int8_t sign) {
      int64_t partkey = 0;
      std::string name;
      if (!ReadPart(row, partkey, name) || partkey < 0) {
        buffer.MarkRebuild();
        return;
      }
      buffer.AddPartNameDelta({partkey, {}, std::move(name), sign});
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
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        int64_t old_key = 0, new_key = 0;
        std::string old_name, new_name;
        if (!ReadPart(mutation.old_rows[i], old_key, old_name) ||
            !ReadPart(mutation.new_rows[i], new_key, new_name) ||
            old_key != new_key || old_key < 0) {
          buffer.MarkRebuild();
          return;
        }
        buffer.AddPartNameDelta(
            {old_key, std::move(old_name), std::move(new_name), 0});
      }
      break;
    }
    return;
  }

  if (mutation.table == config.supplier_table) {
    auto remove = [&](const core::AuxRow &row) {
      int64_t key = 0;
      TpchQ20SupplierRecord rec;
      if (!ReadSupplier(row, key, rec.name, rec.address, rec.nationkey) ||
          key < 0) {
        buffer.MarkRebuild();
        return;
      }
      buffer.AddSupplierDelta({key, false, {}});
    };
    auto put = [&](const core::AuxRow &row) {
      int64_t key = 0;
      TpchQ20SupplierRecord rec;
      if (!ReadSupplier(row, key, rec.name, rec.address, rec.nationkey) ||
          key < 0) {
        buffer.MarkRebuild();
        return;
      }
      buffer.AddSupplierDelta({key, true, std::move(rec)});
    };
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows)
        put(row);
      break;
    case core::MutationKind::Delete:
      for (const auto &row : mutation.old_rows)
        remove(row);
      break;
    case core::MutationKind::Update:
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        remove(mutation.old_rows[i]);
        put(mutation.new_rows[i]);
      }
      break;
    }
    return;
  }

  if (mutation.table == config.nation_table) {
    buffer.MarkRebuild();
    return;
  }
}

core::StagedMaintenanceOutcome TpchQ20Module::StageMaintenance(
    core::AuxEngineServices &services, const core::InvalidationPlan &,
    core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ20TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;

  if (buffer.RequiresRebuild()) {
    const auto decision = services.ChooseMaintenance(
        core::MaintenanceRequest::RebuildOnly(
            services.ExpectedQueriesBeforeNextWrite()));
    core::FinishRebuildAll(outcome, decision);
    return outcome;
  }

  const bool has_lineitem = !buffer.LineitemDeltas().empty();
  const bool has_partsupp = !buffer.PartsuppDeltas().empty();
  const bool has_part = !buffer.PartNameDeltas().empty();
  const bool has_supplier = !buffer.SupplierDeltas().empty();
  if (!has_lineitem && !has_partsupp && !has_part && !has_supplier)
    return outcome;

  const uint64_t delta_count =
      buffer.LineitemDeltas().size() + buffer.PartsuppDeltas().size() +
      buffer.PartNameDeltas().size() + buffer.SupplierDeltas().size();
  const auto decision = services.ChooseMaintenance(
      core::MaintenanceRequest::IncrementalSingleState(
          delta_count, services.ExpectedQueriesBeforeNextWrite()));
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    core::FinishRebuildAll(outcome, decision);
    return outcome;
  }

  core::Version existing_version = 0;
  const auto visibility = services.CurrentVisibility();
  const auto snapshot = services.StateStore().ReadPartitions(
      visibility, {TpchQ20Layout::kFullState});
  bool found = false;
  std::shared_ptr<const TpchQ20State> parent;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ20Layout::kFullState) {
      const auto *typed = AsQ20Image(image);
      if (!typed) {
        core::FinishRebuildAll(outcome);
        return outcome;
      }
      parent = typed->state;
      existing_version = image.version;
      found = true;
      break;
    }
  }
  if (!found || !parent) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }

  // Backstop only: the series overlay is a layered LSM merged per commit
  // (cost proportional to recent changes), so this bound is far above what
  // RF streams reach; it exists to cap memory when the overlay approaches the
  // size of the base image itself.
  const std::size_t max_overlay_weight =
      std::max<std::size_t>(4000000, parent->base->series_key.size() / 2);
  if (parent->OverlayWeight() + delta_count > max_overlay_weight) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }

  // The staged state shares every overlay layer/shard with its parent; the
  // series changes below go into one new layer, and only touched partsupp /
  // part / supplier shards are cloned, so published versions stay immutable.
  auto next = std::make_shared<TpchQ20State>(*parent);
  const TpchQ20Base &base = *next->base;

  // Lineitem deltas: per-pair complete series, copied from the overlay (or
  // base) on first touch, collected into one new overlay layer.
  {
    const auto &deltas = buffer.LineitemDeltas();
    // Deltas per (part, supplier) pair, in arrival order: a stable sort by pair key.
    std::vector<std::pair<uint64_t, uint32_t>> by_pair(deltas.size());
    core::ParallelSlices(services, deltas.size(), kSeriesSortGrain,
                         [&](std::size_t, std::size_t first, std::size_t last) {
                           for (std::size_t i = first; i < last; ++i)
                             by_pair[i] = {MakePairKey(deltas[i].partkey, deltas[i].suppkey),
                                           static_cast<uint32_t>(i)};
                         });
    core::ParallelStableSort(
        services, by_pair,
        [](const auto &a, const auto &b) { return a.first < b.first; }, kSeriesSortGrain);
    std::vector<uint64_t> keys;
    std::vector<std::size_t> job_begin; // job j owns by_pair[job_begin[j], job_begin[j + 1])
    for (std::size_t i = 0; i < by_pair.size(); ++i) {
      if (i == 0 || by_pair[i].first != by_pair[i - 1].first) {
        keys.push_back(by_pair[i].first);
        job_begin.push_back(i);
      }
    }
    job_begin.push_back(by_pair.size());
    // Pairs are independent: each slice rebuilds the series of its own pairs.
    std::vector<std::vector<TpchQ20DateQuantity>> series(keys.size());
    core::ParallelSlices(
        services, keys.size(), kSeriesGrain,
        [&](std::size_t, std::size_t first, std::size_t last) {
          for (std::size_t job = first; job < last; ++job) {
            auto &list = series[job];
            const auto cur = parent->series.Find(keys[job]);
            if (cur.found) {
              list.assign(cur.begin(), cur.end());
            } else {
              const uint32_t idx = base.FindSeries(keys[job]);
              if (idx != TpchQ20Base::kNoSeries)
                list = BaseSeriesCopy(base, idx);
            }
            for (std::size_t at = job_begin[job]; at < job_begin[job + 1]; ++at)
              ApplySeriesDelta(list, deltas[by_pair[at].second]);
          }
        });
    if (!keys.empty()) {
      TpchQ20SeriesLayer::Builder builder;
      builder.Reserve(keys.size(), keys.size() * 4);
      for (std::size_t job = 0; job < keys.size(); ++job)
        builder.Add(keys[job], series[job].data(), series[job].size());
      next->series.Push(builder.Finish());
    }
  }

  // Partsupp deltas: per-part complete list, copied from base on first touch.
  for (const auto &delta : buffer.PartsuppDeltas()) {
    const bool inserted = next->partsupps.Find(delta.partkey) == nullptr;
    auto &list = next->partsupps[delta.partkey];
    if (inserted) {
      base.ForEachBasePair(delta.partkey,
                           [&](int64_t suppkey, int64_t availqty, uint32_t) {
                             list.push_back({suppkey, availqty});
                           });
    }
    const auto it = std::find_if(
        list.begin(), list.end(),
        [&](const TpchQ20PsPair &p) { return p.suppkey == delta.suppkey; });
    if (delta.sign > 0) {
      if (it != list.end())
        it->availqty = delta.availqty;
      else
        list.push_back({delta.suppkey, delta.availqty});
    } else if (it != list.end()) {
      list.erase(it);
    }
  }

  // Part deltas: the overlay record shadows the base name.
  for (const auto &delta : buffer.PartNameDeltas()) {
    auto &rec = next->parts[delta.partkey];
    if (delta.sign < 0) {
      rec.present = false;
      rec.name.clear();
    } else {
      rec.present = true;
      rec.name = delta.new_name;
    }
  }

  // Supplier deltas.
  for (const auto &delta : buffer.SupplierDeltas()) {
    auto &rec = next->suppliers[delta.suppkey];
    rec.present = delta.present;
    if (delta.present)
      rec.record = delta.record;
    else
      rec.record = {};
  }

  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.updates.push_back(core::StateImage::Make(
      TpchQ20Layout::kAccelerator, TpchQ20Layout::kFullState, existing_version,
      MakeStateImage(std::static_pointer_cast<const TpchQ20State>(next))));
  return outcome;
}

bool TpchQ20Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ20Module::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  if (arguments.values.size() < 3) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  int32_t start_date_days = 0;
  std::string part_name_pattern;
  std::string nation_name;

  if (std::holds_alternative<core::AuxDate>(arguments.values[0].payload)) {
    start_date_days =
        std::get<core::AuxDate>(arguments.values[0].payload).days_since_epoch;
  } else if (std::holds_alternative<int64_t>(arguments.values[0].payload)) {
    start_date_days = static_cast<int32_t>(
        std::get<int64_t>(arguments.values[0].payload));
  } else {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  if (!arguments.values[1].is_null &&
      std::holds_alternative<std::string>(arguments.values[1].payload)) {
    part_name_pattern =
        std::get<std::string>(arguments.values[1].payload);
  } else {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  if (!arguments.values[2].is_null &&
      std::holds_alternative<std::string>(arguments.values[2].payload)) {
    nation_name = std::get<std::string>(arguments.values[2].payload);
  } else {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  const int32_t end_date_days = core::sql_date::AddYears(start_date_days, 1);

  if (!services.StateStore()
           .CheckValidity({Id(), {}}, services.CurrentVisibility())
           .IsValid()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  std::shared_ptr<const TpchQ20State> state;
  if (!LoadState(services, state)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  EmitResult(*state, start_date_days, end_date_days, part_name_pattern,
             nation_name, services, sink);
}

namespace {

template <class Row, class Scan, class Consume>
PieceList<Row> ScanPieces(const core::AuxEngineServices &services, Scan &&scan, Consume &&consume) {
  return core::PartitionedBuild<PartitionPieces<Row>>(
             services, [] { return PartitionPieces<Row>{}; }, std::forward<Scan>(scan),
             std::forward<Consume>(consume),
             [](PartitionPieces<Row> &into, PartitionPieces<Row> &&from) { into.Absorb(std::move(from)); })
      .Take();
}

PieceList<SeriesRow> SortedSeries(core::AuxEngineServices &services,
                               TpchQ20InitialScan &initial_scan) {
  const auto pool = core::Parallelism::OnPool(services);
  auto pieces = ScanPieces<SeriesRow>(
      services,
      [&](std::size_t parts, auto &&sink) {
        initial_scan.ScanLineitems(services, parts,
                                    [&](std::size_t p, const TpchQ20LineitemInput &input) {
                                      sink(p, core::SingleRowBatch<TpchQ20LineitemInput>{input});
                                    });
      },
      [](PartitionPieces<SeriesRow> &partial, const TpchQ20LineitemInput &input) {
        if (input.partkey < 0 || input.suppkey < 0)
          return;
        RequirePairKey(input.partkey, input.suppkey);
        partial.rows.push_back({MakePairKey(input.partkey, input.suppkey), input.shipdate_days,
                                input.quantity_raw});
      });
  return core::SortIntoKeyBuckets(
      pool, std::move(pieces), [](const SeriesRow &r) { return r.key; },
      [](const SeriesRow &a, const SeriesRow &b) { return a.key < b.key; });
}

PieceList<TpchQ20PartsuppInput> SortedPartsupps(core::AuxEngineServices &services,
                                             TpchQ20InitialScan &initial_scan) {
  const auto pool = core::Parallelism::OnPool(services);
  auto pieces = ScanPieces<TpchQ20PartsuppInput>(
      services,
      [&](std::size_t parts, auto &&sink) {
        initial_scan.ScanPartsupps(services, parts,
                                    [&](std::size_t p, const TpchQ20PartsuppInput &input) {
                                      sink(p, core::SingleRowBatch<TpchQ20PartsuppInput>{input});
                                    });
      },
      [](PartitionPieces<TpchQ20PartsuppInput> &partial, const TpchQ20PartsuppInput &input) {
        if (input.partkey < 0 || input.suppkey < 0)
          return;
        RequirePairKey(input.partkey, input.suppkey);
        partial.rows.push_back(input);
      });
  const auto pair_key = [](const TpchQ20PartsuppInput &r) {
    return MakePairKey(r.partkey, r.suppkey);
  };
  return core::SortIntoKeyBuckets(pool, std::move(pieces), pair_key,
                         [&](const TpchQ20PartsuppInput &a, const TpchQ20PartsuppInput &b) {
                           return pair_key(a) < pair_key(b);
                         });
}

// A scan partial of part names: the bytes of its names and where each starts.
struct NamePiece {
  struct Ref {
    int64_t key;
    uint64_t off;
    uint32_t len;
  };
  std::string bytes;
  std::vector<Ref> refs;
  // Pieces merged into this one, in partition order; heap-stable so the
  // NameRows built later can point into their bytes.
  std::vector<std::unique_ptr<NamePiece>> merged;

  void Absorb(NamePiece &&from) {
    auto piece = std::make_unique<NamePiece>();
    piece->bytes = std::move(from.bytes);
    piece->refs = std::move(from.refs);
    merged.push_back(std::move(piece));
    for (auto &later : from.merged)
      merged.push_back(std::move(later));
  }
};

// Part names sorted; the rows point into the scanned bytes, which `owner` keeps.
struct SortedNameRows {
  std::unique_ptr<NamePiece> owner;
  PieceList<NameRow> rows;
};

SortedNameRows SortedNames(core::AuxEngineServices &services,
                           TpchQ20InitialScan &initial_scan) {
  const auto pool = core::Parallelism::OnPool(services);
  struct Name {
    int64_t key;
    std::string_view name;
  };
  auto scanned = std::make_unique<NamePiece>(core::PartitionedBuild<NamePiece>(
      services, [] { return NamePiece{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan.ScanPartNames(services, parts,
                                    [&](std::size_t p, int64_t key, std::string_view name) {
                                      sink(p, core::SingleRowBatch<Name>{{key, name}});
                                    });
      },
      [](NamePiece &partial, const Name &part) {
        if (part.key < 0)
          return;
        partial.refs.push_back({part.key, partial.bytes.size(),
                                static_cast<uint32_t>(part.name.size())});
        partial.bytes.append(part.name);
      },
      [](NamePiece &into, NamePiece &&from) { into.Absorb(std::move(from)); }));
  std::vector<const NamePiece *> sources{scanned.get()};
  for (const auto &piece : scanned->merged)
    sources.push_back(piece.get());
  PieceList<NameRow> pieces(sources.size());
  core::ParallelEach(services, sources.size(), 1, [&](std::size_t i) {
    pieces[i].reserve(sources[i]->refs.size());
    for (const auto &ref : sources[i]->refs)
      pieces[i].push_back({ref.key, sources[i]->bytes.data() + ref.off, ref.len});
  });
  const std::size_t skip =
      core::SharedPrefixLength(pool, pieces, [](const NameRow &r) { return r.View(); });
  return {std::move(scanned),
          core::SortIntoKeyBuckets(
              pool, std::move(pieces),
              [&](const NameRow &r) { return core::StringPrefixKey(r.View(), skip); },
              [](const NameRow &a, const NameRow &b) { return a.View() < b.View(); })};
}

PieceList<SupplierRow> SortedSuppliers(core::AuxEngineServices &services,
                                    TpchQ20InitialScan &initial_scan) {
  const auto pool = core::Parallelism::OnPool(services);
  auto pieces = ScanPieces<SupplierRow>(
      services,
      [&](std::size_t parts, auto &&sink) {
        initial_scan.ScanSuppliers(services, parts,
                                    [&](std::size_t p, const TpchQ20SupplierInput &input) {
                                      sink(p, core::SingleRowBatch<TpchQ20SupplierInput>{input});
                                    });
      },
      [](PartitionPieces<SupplierRow> &partial, const TpchQ20SupplierInput &input) {
        if (input.key >= 0)
          partial.rows.push_back({input.key, {input.name, input.address, input.nationkey}});
      });
  const std::size_t skip = core::SharedPrefixLength(
      pool, pieces, [](const SupplierRow &r) { return std::string_view(r.record.name); });
  return core::SortIntoKeyBuckets(
      pool, std::move(pieces),
      [&](const SupplierRow &r) { return core::StringPrefixKey(r.record.name, skip); },
      [](const SupplierRow &a, const SupplierRow &b) {
        return a.record.name != b.record.name ? a.record.name < b.record.name : a.key < b.key;
      });
}

} // namespace

TpchQ20State
TpchQ20Module::BuildState(core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error(
        "TPC-H Q20 aux requires a typed initial scan source");

  BaseBuilder builder(services);
  builder.AdoptSeries(SortedSeries(services, *initial_scan));
  builder.AdoptPartsupp(SortedPartsupps(services, *initial_scan));
  const auto names = SortedNames(services, *initial_scan);
  builder.AdoptNames(names.rows);
  builder.AdoptSuppliers(SortedSuppliers(services, *initial_scan));
  initial_scan->ScanNationsSerial([&](const TpchQ20NationInput &input) {
    builder.AddNation(input.key, input.name);
  });

  TpchQ20State state;
  state.base = builder.Finish();
  return state;
}

bool TpchQ20Module::LoadState(core::AuxEngineServices &services,
                              std::shared_ptr<const TpchQ20State> &state) const {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {TpchQ20Layout::kFullState});
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ20Layout::kFullState) {
      const auto *typed = AsQ20Image(image);
      if (!typed)
        return false;
      state = typed->state;
      return true;
    }
  }
  return false;
}

void TpchQ20Module::EmitResult(const TpchQ20State &state, int32_t start_date_days,
                               int32_t end_date_days, const std::string &part_name_pattern,
                               const std::string &nation_name, core::AuxEngineServices &services,
                               core::AuxRowSink &sink) const {
  auto nation_keys = state.FindNationKeysByName(nation_name);
  if (nation_keys.empty()) { // no nation has this name: no supplier qualifies
    sink.Begin(core::OutputSchema{});
    sink.End();
    return;
  }
  const PartNameFilter names(*state.base, part_name_pattern);
  QualifyingSuppliers qualifying(state, start_date_days, end_date_days, names,
                                 std::move(nation_keys), services);
  const auto suppkeys = qualifying.Find();
  WriteSuppliers(InNameOrder(state, qualifying, suppkeys), sink);
}

std::vector<core::CalibrationWrite> TpchQ20Module::CalibrationWrites() const {
  return TpchQ20CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
