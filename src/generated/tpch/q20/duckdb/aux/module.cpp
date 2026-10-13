#include "tpch/q20/duckdb/aux/module.hpp"
#include "tpch/q20/calibration_writes.hpp"
#include "accelerator_codegen/stateful/core/byte_stream.hpp"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

using core::ByteReader;
using core::ByteWriter;

constexpr uint32_t kStateMagic = 0x51323053; // Q20S
constexpr uint32_t kVersion = 3;

// Calendar helpers (proleptic Gregorian; days since 1970-01-01).
int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void CivilFromDays(int64_t z, int64_t &y, unsigned &m, unsigned &d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y += m <= 2;
}

// start + INTERVAL '1' YEAR (Feb 29 clamps to Feb 28, as SQL does).
int32_t AddOneYear(int32_t start_days) {
  int64_t y = 0;
  unsigned m = 0, d = 0;
  CivilFromDays(start_days, y, m, d);
  ++y;
  if (m == 2 && d == 29) {
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    if (!leap)
      d = 28;
  }
  return static_cast<int32_t>(DaysFromCivil(y, m, d));
}
bool StringCell(const core::AuxRow &row, std::size_t index, std::string &out) {
  if (index >= row.values.size() || row.values[index].is_null ||
      !std::holds_alternative<std::string>(row.values[index].payload))
    return false;
  out = std::get<std::string>(row.values[index].payload);
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
  if (std::holds_alternative<core::AuxDate>(cell.payload)) {
    out = std::get<core::AuxDate>(cell.payload).days_since_epoch;
    return true;
  }
  return false;
}

bool Int32Cell(const core::AuxRow &row, std::size_t index, int32_t &out) {
  int64_t value = 0;
  if (!IntCell(row, index, value) || value < std::numeric_limits<int32_t>::min() ||
      value > std::numeric_limits<int32_t>::max())
    return false;
  out = static_cast<int32_t>(value);
  return true;
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

bool DateCell(const core::AuxRow &row, std::size_t index, int32_t &days) {
  int64_t value = 0;
  if (!IntCell(row, index, value) ||
      value < std::numeric_limits<int32_t>::min() ||
      value > std::numeric_limits<int32_t>::max())
    return false;
  days = static_cast<int32_t>(value);
  return true;
}

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

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
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
  int64_t raw = 0;
  int32_t scale = 0;
  if (!RawDecimalCell(row, 3, raw, scale))
    return false;
  if (scale != 2)
    return false;
  quantity_raw = raw;
  return true;
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
         StringCell(row, 2, address) && Int32Cell(row, 3, nationkey);
}

// ── Base builder ────────────────────────────────────────────────────────────
// Dense key-indexed arrays cover keys below max(kMinDense, 4 x row count);
// larger keys live in hash maps, so sparse keys never blow up memory.
constexpr int64_t kMinDense = int64_t{1} << 20;
int64_t DenseLimit(std::size_t rows) {
  return std::max<int64_t>(kMinDense, 4 * static_cast<int64_t>(rows));
}

class BaseBuilder {
public:
  BaseBuilder() : base(std::make_shared<TpchQ20Base>()) {
    base->series_off.push_back(0);
  }

  // Rows must arrive in nondecreasing pair-key order.
  void AddSeriesRow(uint64_t key, int32_t date, int64_t qty) {
    if (has_cur && key != cur_key) {
      if (key < cur_key)
        throw std::logic_error("TPC-H Q20 aux: lineitem scan not ordered");
      CloseSeries();
    }
    cur_key = key;
    has_cur = true;
    cur.push_back({date, qty});
  }
  void FinishSeries() { CloseSeries(); }

  // Rows must arrive in nondecreasing (partkey, suppkey) order.
  void AddPartsupp(int64_t partkey, int64_t suppkey, int64_t availqty) {
    if (partkey < 0 || suppkey < 0)
      return;
    RequirePairKey(partkey, suppkey);
    PsRow e;
    e.partkey = partkey;
    e.availqty = availqty;
    e.suppkey = static_cast<int32_t>(suppkey);
    ps.push_back(e);
  }

  void AddPart(int64_t key, const char *name, std::size_t len) {
    if (key < 0)
      return;
    names.push_back({key, static_cast<uint64_t>(arena.size()),
                     static_cast<uint32_t>(len)});
    arena.append(name, len);
  }

  void AddSupplier(int64_t key, std::string name, std::string address,
                   int32_t nationkey) {
    if (key < 0)
      return;
    suppliers[key] = {std::move(name), std::move(address), nationkey};
  }

  void AddNation(int32_t key, const std::string &name) {
    if (key < 0)
      return;
    base->nations[key] = name;
    base->nation_name_index[name].push_back(key);
  }

  std::shared_ptr<const TpchQ20Base> Finish() {
    CloseSeries();
    // series index resolution (both sides sorted by pair key).
    {
      const auto &keys = base->series_key;
      std::size_t j = 0;
      for (auto &e : ps) {
        const uint64_t key = MakePairKey(e.partkey, e.suppkey);
        while (j < keys.size() && keys[j] < key)
          ++j;
        if (j < keys.size() && keys[j] == key) {
          e.series_idx = static_cast<uint32_t>(j);
        } else {
          const auto it = std::lower_bound(keys.begin(), keys.end(), key);
          e.series_idx = (it != keys.end() && *it == key)
                             ? static_cast<uint32_t>(it - keys.begin())
                             : TpchQ20Base::kNoSeries;
        }
      }
    }
    FinishNames();
    FinishSuppliers();
    FinishPairs();
    return std::move(base);
  }

private:
  struct NameRef {
    int64_t key;
    uint64_t off;
    uint32_t len;
  };
  struct PsRow {
    int64_t partkey = 0;
    int64_t availqty = 0;
    int32_t suppkey = 0;
    uint32_t series_idx = TpchQ20Base::kNoSeries;
  };

  // Lays the partsupp rows out nation-major (see TpchQ20Base::pairs): one
  // entry per (row, base name of its part), bucketed by the supplier's slot,
  // then each slot sorted by (rank, suppkey).
  void FinishPairs() {
    auto &b = *base;
    // Slots: one per distinct base nation of a supplier, then "no supplier".
    std::vector<int32_t> slot_nations;
    for (const auto &rec : b.supplier_records)
      if (b.nation_slot.emplace(rec.nationkey, 0).second)
        slot_nations.push_back(rec.nationkey);
    std::sort(slot_nations.begin(), slot_nations.end());
    for (std::size_t s = 0; s < slot_nations.size(); ++s)
      b.nation_slot[slot_nations[s]] = static_cast<uint32_t>(s);
    const std::size_t slots = slot_nations.size() + 1;
    const auto no_supplier = static_cast<uint32_t>(slots - 1);
    std::vector<uint32_t> dense_slot(b.supp_nation.size(), no_supplier);
    for (std::size_t r = 0; r < b.supplier_keys.size(); ++r)
      if (static_cast<std::size_t>(b.supplier_keys[r]) < dense_slot.size())
        dense_slot[static_cast<std::size_t>(b.supplier_keys[r])] =
            b.nation_slot.at(b.supplier_records[r].nationkey);
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

    std::vector<std::size_t> count(slots + 1, 0);
    for (const auto &row : ps) {
      const uint32_t slot = slot_of(row.suppkey);
      if (!for_each_rank(row.partkey, [&](uint32_t) { ++count[slot + 1]; }))
        b.orphan_pairs.push_back(
            {row.partkey, row.availqty, row.suppkey, row.series_idx});
    }
    for (std::size_t s = 1; s <= slots; ++s)
      count[s] += count[s - 1];
    if (count[slots] > std::numeric_limits<uint32_t>::max())
      throw std::runtime_error("TPC-H Q20 aux: too many partsupp pairs");
    b.slot_off.assign(count.begin(), count.end());
    b.pairs.resize(count[slots]);
    for (const auto &row : ps) {
      const uint32_t slot = slot_of(row.suppkey);
      for_each_rank(row.partkey, [&](uint32_t rank) {
        auto &e = b.pairs[count[slot]++];
        e.rank = rank;
        e.suppkey = row.suppkey;
        e.series_idx = row.series_idx;
        if (row.availqty > std::numeric_limits<int32_t>::min() &&
            row.availqty <= std::numeric_limits<int32_t>::max()) {
          e.availqty = static_cast<int32_t>(row.availqty);
        } else {
          e.availqty = TpchQ20Base::kWideAvail;
          b.avail_wide[MakePairKey(row.partkey, row.suppkey)] = row.availqty;
        }
      });
    }
    ps.clear();
    ps.shrink_to_fit();
    for (std::size_t s = 0; s < slots; ++s)
      std::sort(b.pairs.begin() + static_cast<std::ptrdiff_t>(b.slot_off[s]),
                b.pairs.begin() + static_cast<std::ptrdiff_t>(b.slot_off[s + 1]),
                [](const TpchQ20Base::PairEntry &x, const TpchQ20Base::PairEntry &y) {
                  return x.rank != y.rank ? x.rank < y.rank : x.suppkey < y.suppkey;
                });
  }

  void CloseSeries() {
    if (!has_cur)
      return;
    std::stable_sort(cur.begin(), cur.end(),
                     [](const TpchQ20DateQuantity &a,
                        const TpchQ20DateQuantity &b) {
                       return a.shipdate_days < b.shipdate_days;
                     });
    bool fits = true;
    for (const auto &e : cur) {
      if (e.quantity_raw < std::numeric_limits<int32_t>::min() ||
          e.quantity_raw > std::numeric_limits<int32_t>::max()) {
        fits = false;
        break;
      }
    }
    const uint32_t idx = static_cast<uint32_t>(base->series_key.size());
    base->series_key.push_back(cur_key);
    if (fits) {
      for (const auto &e : cur)
        base->series_entries.push_back(
            {e.shipdate_days, static_cast<int32_t>(e.quantity_raw)});
    } else {
      base->series_wide[idx] = cur;
    }
    base->series_off.push_back(
        static_cast<uint32_t>(base->series_entries.size()));
    cur.clear();
    has_cur = false;
  }

  void FinishNames() {
    const auto view = [&](const NameRef &n) {
      return std::string_view(arena.data() + n.off, n.len);
    };
    const auto less = [&](const NameRef &a, const NameRef &b) {
      return view(a) < view(b);
    };
    if (!std::is_sorted(names.begin(), names.end(), less))
      std::sort(names.begin(), names.end(), less);
    base->name_arena.reserve(arena.size());
    base->name_off.reserve(names.size() + 1);
    base->name_partkey.reserve(names.size());
    base->name_off.push_back(0);
    for (const auto &n : names) {
      base->name_arena.append(arena.data() + n.off, n.len);
      base->name_off.push_back(static_cast<uint32_t>(base->name_arena.size()));
      base->name_partkey.push_back(n.key);
    }
    const int64_t dense = DenseLimit(names.size());
    int64_t max_key = -1;
    for (const auto &n : names)
      if (n.key < dense)
        max_key = std::max(max_key, n.key);
    base->part_rank.assign(static_cast<std::size_t>(max_key + 1),
                           TpchQ20Base::kNoRank);
    for (std::size_t r = 0; r < names.size(); ++r) {
      const int64_t key = names[r].key;
      const auto rank = static_cast<uint32_t>(r);
      bool first = false;
      if (key < dense) {
        auto &slot = base->part_rank[static_cast<std::size_t>(key)];
        first = slot == TpchQ20Base::kNoRank;
        if (first)
          slot = rank;
      } else {
        first = base->sparse_rank.emplace(key, rank).second;
      }
      if (!first)
        base->extra_ranks[key].push_back(rank);
    }
    names.clear();
    names.shrink_to_fit();
    arena.clear();
    arena.shrink_to_fit();
  }

  void FinishSuppliers() {
    auto &b = *base;
    std::vector<std::pair<int64_t, TpchQ20SupplierRecord>> rows(
        std::make_move_iterator(suppliers.begin()),
        std::make_move_iterator(suppliers.end()));
    suppliers.clear();
    std::sort(rows.begin(), rows.end(), [](const auto &x, const auto &y) {
      return x.second.name != y.second.name ? x.second.name < y.second.name
                                            : x.first < y.first;
    });
    b.supplier_keys.reserve(rows.size());
    b.supplier_records.reserve(rows.size());
    for (auto &[key, rec] : rows) {
      b.supplier_keys.push_back(key);
      b.supplier_records.push_back(std::move(rec));
    }
    const int64_t dense = DenseLimit(rows.size());
    int64_t max_key = -1;
    for (const int64_t key : b.supplier_keys)
      if (key < dense)
        max_key = std::max(max_key, key);
    b.supp_nation.assign(static_cast<std::size_t>(max_key + 1), -1);
    b.supp_rank.assign(static_cast<std::size_t>(max_key + 1),
                       TpchQ20Base::kNoRank);
    for (std::size_t r = 0; r < b.supplier_keys.size(); ++r) {
      const int64_t key = b.supplier_keys[r];
      if (key <= max_key) {
        b.supp_nation[static_cast<std::size_t>(key)] =
            b.supplier_records[r].nationkey;
        b.supp_rank[static_cast<std::size_t>(key)] = static_cast<uint32_t>(r);
      } else {
        b.sparse_supp_rank[key] = static_cast<uint32_t>(r);
      }
    }
  }


  std::shared_ptr<TpchQ20Base> base;
  std::vector<TpchQ20DateQuantity> cur;
  uint64_t cur_key = 0;
  bool has_cur = false;
  std::vector<NameRef> names;
  std::string arena;
  std::unordered_map<int64_t, TpchQ20SupplierRecord> suppliers;
  // partsupp rows in (partkey, suppkey) order; consumed by FinishPairs.
  std::vector<PsRow> ps;
};

} // namespace

// ── Base / state accessors ──────────────────────────────────────────────────
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

namespace {

// Effective (merged) views used by Encode.
std::vector<TpchQ20DateQuantity> BaseSeriesCopy(const TpchQ20Base &base,
                                                uint32_t idx) {
  std::vector<TpchQ20DateQuantity> out;
  const auto wit = base.series_wide.find(idx);
  if (wit != base.series_wide.end())
    return wit->second;
  for (uint32_t i = base.series_off[idx]; i < base.series_off[idx + 1]; ++i)
    out.push_back({base.series_entries[i].date, base.series_entries[i].qty});
  return out;
}

} // namespace

// ── Encode / Decode ─────────────────────────────────────────────────────────
std::vector<uint8_t> EncodeTpchQ20State(const TpchQ20State &state) {
  const auto &base = *state.base;
  ByteWriter writer;
  core::WriteImageHeader(writer, kStateMagic, kVersion, 0);

  // series (merged, ascending key)
  std::vector<uint64_t> keys(base.series_key.begin(), base.series_key.end());
  state.series.ForEachKey([&](uint64_t k) { keys.push_back(k); });
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  struct SeriesOut {
    uint64_t key;
    std::vector<TpchQ20DateQuantity> entries;
  };
  std::vector<SeriesOut> series_out;
  for (const uint64_t k : keys) {
    std::vector<TpchQ20DateQuantity> entries;
    if (const auto ov = state.series.Find(k); ov.found) {
      entries.assign(ov.begin(), ov.end());
    } else {
      const uint32_t idx = base.FindSeries(k);
      if (idx != TpchQ20Base::kNoSeries)
        entries = BaseSeriesCopy(base, idx);
    }
    if (!entries.empty())
      series_out.push_back({k, std::move(entries)});
  }
  writer.U32(static_cast<uint32_t>(series_out.size()));
  for (const auto &s : series_out) {
    writer.U64(s.key);
    writer.U32(static_cast<uint32_t>(s.entries.size()));
    for (const auto &e : s.entries) {
      writer.I32(e.shipdate_days);
      writer.I64(e.quantity_raw);
    }
  }

  // partsupps (merged, ascending)
  struct PsRow {
    int64_t part, supp, avail;
  };
  std::vector<PsRow> ps_rows;
  for (const auto &e : base.pairs) {
    const int64_t part = base.name_partkey[e.rank];
    // A part with several names has its pairs once per name: keep one copy.
    if (base.RankOf(part) != e.rank || state.partsupps.Find(part))
      continue;
    ps_rows.push_back({part, e.suppkey, base.Avail(e)});
  }
  for (const auto &p : base.orphan_pairs)
    if (!state.partsupps.Find(p.partkey))
      ps_rows.push_back({p.partkey, p.suppkey, p.availqty});
  state.partsupps.ForEach([&](uint64_t part, const auto &list) {
    for (const auto &p : list)
      ps_rows.push_back({static_cast<int64_t>(part), p.suppkey, p.availqty});
  });
  std::sort(ps_rows.begin(), ps_rows.end(), [](const PsRow &a, const PsRow &b) {
    return a.part != b.part ? a.part < b.part : a.supp < b.supp;
  });
  writer.U32(static_cast<uint32_t>(ps_rows.size()));
  for (const auto &r : ps_rows) {
    writer.I64(r.part);
    writer.I64(r.supp);
    writer.I64(r.avail);
  }

  // parts (merged)
  std::vector<std::pair<int64_t, std::string>> part_rows;
  for (std::size_t i = 0; i < base.name_partkey.size(); ++i) {
    if (state.parts.Find(base.name_partkey[i]))
      continue;
    part_rows.emplace_back(base.name_partkey[i],
                           std::string(base.PartName(i)));
  }
  state.parts.ForEach([&](uint64_t k, const TpchQ20PartOverlay &ov) {
    if (ov.present)
      part_rows.emplace_back(static_cast<int64_t>(k), ov.name);
  });
  writer.U32(static_cast<uint32_t>(part_rows.size()));
  for (const auto &[k, name] : part_rows) {
    writer.I64(k);
    writer.String(name);
  }

  // suppliers (merged)
  std::map<int64_t, TpchQ20SupplierRecord> supp_rows;
  for (std::size_t r = 0; r < base.supplier_keys.size(); ++r)
    supp_rows[base.supplier_keys[r]] = base.supplier_records[r];
  state.suppliers.ForEach([&](uint64_t k, const TpchQ20SupplierOverlay &ov) {
    if (ov.present)
      supp_rows[static_cast<int64_t>(k)] = ov.record;
    else
      supp_rows.erase(static_cast<int64_t>(k));
  });
  writer.U32(static_cast<uint32_t>(supp_rows.size()));
  for (const auto &[key, supplier] : supp_rows) {
    writer.I64(key);
    writer.String(supplier.name);
    writer.String(supplier.address);
    writer.I32(supplier.nationkey);
  }

  std::map<int32_t, std::string> nation_rows(base.nations.begin(),
                                             base.nations.end());
  writer.U32(static_cast<uint32_t>(nation_rows.size()));
  for (const auto &[key, name] : nation_rows) {
    writer.I32(key);
    writer.String(name);
  }
  return std::move(writer.bytes);
}

bool DecodeTpchQ20State(const std::vector<uint8_t> &bytes,
                        TpchQ20State &state) {
  ByteReader reader(bytes);
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kStateMagic, kVersion, count))
    return false;

  BaseBuilder builder;
  uint32_t series_count = 0;
  if (!reader.U32(series_count))
    return false;
  for (uint32_t i = 0; i < series_count; ++i) {
    uint64_t key = 0;
    uint32_t entry_count = 0;
    if (!reader.U64(key) || !reader.U32(entry_count))
      return false;
    for (uint32_t j = 0; j < entry_count; ++j) {
      int32_t date = 0;
      int64_t qty = 0;
      if (!reader.I32(date) || !reader.I64(qty))
        return false;
      builder.AddSeriesRow(key, date, qty);
    }
  }
  builder.FinishSeries();

  uint32_t ps_count = 0;
  if (!reader.U32(ps_count))
    return false;
  for (uint32_t i = 0; i < ps_count; ++i) {
    int64_t part = 0, supp = 0, avail = 0;
    if (!reader.I64(part) || !reader.I64(supp) || !reader.I64(avail))
      return false;
    builder.AddPartsupp(part, supp, avail);
  }

  uint32_t part_count = 0;
  if (!reader.U32(part_count))
    return false;
  for (uint32_t i = 0; i < part_count; ++i) {
    int64_t key = 0;
    std::string name;
    if (!reader.I64(key) || !reader.String(name))
      return false;
    builder.AddPart(key, name.data(), name.size());
  }

  uint32_t supplier_count = 0;
  if (!reader.U32(supplier_count))
    return false;
  for (uint32_t i = 0; i < supplier_count; ++i) {
    int64_t key = 0;
    int32_t nationkey = 0;
    std::string name, address;
    if (!reader.I64(key) || !reader.String(name) || !reader.String(address) ||
        !reader.I32(nationkey))
      return false;
    builder.AddSupplier(key, std::move(name), std::move(address), nationkey);
  }

  uint32_t nation_count = 0;
  if (!reader.U32(nation_count))
    return false;
  for (uint32_t i = 0; i < nation_count; ++i) {
    int32_t key = 0;
    std::string name;
    if (!reader.I32(key) || !reader.String(name))
      return false;
    builder.AddNation(key, name);
  }
  if (!reader.FullyConsumed())
    return false;

  state = TpchQ20State{};
  state.base = builder.Finish();
  return true;
}

// ── TxnBuffer ───────────────────────────────────────────────────────────────
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

// ── Module implementation ───────────────────────────────────────────────────
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
        {0, 0, false, false, true, true, false,
         core::MaintenanceAction::NoOp,
         services.ExpectedQueriesBeforeNextWrite()});
    outcome.action = decision.action == core::MaintenanceAction::ApplyDelta
                         ? core::MaintenanceAction::RebuildAll
                         : decision.action;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
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
      {delta_count, 1, true, false, true, false, false,
       core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  if (decision.action == core::MaintenanceAction::RebuildPartition) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    outcome.action = decision.action;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
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
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
      parent = typed->state;
      existing_version = image.version;
      found = true;
      break;
    }
  }
  if (!found || !parent) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
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
    std::unordered_map<uint64_t, std::vector<TpchQ20DateQuantity>> touched;
    touched.reserve(buffer.LineitemDeltas().size());
    for (const auto &delta : buffer.LineitemDeltas()) {
      const uint64_t pair_key = MakePairKey(delta.partkey, delta.suppkey);
      auto [it0, inserted] = touched.try_emplace(pair_key);
      auto &series = it0->second;
      if (inserted) {
        const auto cur = parent->series.Find(pair_key);
        if (cur.found) {
          series.assign(cur.begin(), cur.end());
        } else {
          const uint32_t idx = base.FindSeries(pair_key);
          if (idx != TpchQ20Base::kNoSeries)
            series = BaseSeriesCopy(base, idx);
        }
      }
      if (delta.sign > 0) {
        const auto pos = std::upper_bound(
            series.begin(), series.end(), delta.shipdate_days,
            [](int32_t d, const TpchQ20DateQuantity &e) {
              return d < e.shipdate_days;
            });
        series.insert(pos, {delta.shipdate_days, delta.quantity_raw});
      } else {
        auto it = std::lower_bound(
            series.begin(), series.end(), delta.shipdate_days,
            [](const TpchQ20DateQuantity &e, int32_t d) {
              return e.shipdate_days < d;
            });
        for (; it != series.end() && it->shipdate_days == delta.shipdate_days;
             ++it) {
          if (it->quantity_raw == delta.quantity_raw) {
            series.erase(it);
            break;
          }
        }
      }
    }
    if (!touched.empty()) {
      TpchQ20SeriesLayer::Builder builder;
      builder.Reserve(touched.size(), touched.size() * 4);
      for (const auto &[key, vec] : touched)
        builder.Add(key, vec.data(), vec.size());
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

  const int32_t end_date_days = AddOneYear(start_date_days);

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

void TpchQ20Module::ExecuteFallback(core::AuxEngineServices &services,
                                    const core::QueryArguments &arguments,
                                    core::AuxRowSink &sink) {
  int32_t start_date_days = 0;
  std::string part_name_pattern;
  std::string nation_name;

  if (arguments.values.size() >= 3) {
    if (std::holds_alternative<core::AuxDate>(arguments.values[0].payload)) {
      start_date_days =
          std::get<core::AuxDate>(arguments.values[0].payload).days_since_epoch;
    } else if (std::holds_alternative<int64_t>(
                   arguments.values[0].payload)) {
      start_date_days = static_cast<int32_t>(
          std::get<int64_t>(arguments.values[0].payload));
    }
    if (!arguments.values[1].is_null &&
        std::holds_alternative<std::string>(arguments.values[1].payload)) {
      part_name_pattern =
          std::get<std::string>(arguments.values[1].payload);
    }
    if (!arguments.values[2].is_null &&
        std::holds_alternative<std::string>(arguments.values[2].payload)) {
      nation_name = std::get<std::string>(arguments.values[2].payload);
    }
  }

  const int32_t end_date_days = AddOneYear(start_date_days);
  const auto state = BuildState(services);
  EmitResult(state, start_date_days, end_date_days, part_name_pattern,
             nation_name, services, sink);
}

TpchQ20State
TpchQ20Module::BuildState(core::AuxEngineServices &services) const {
  (void)services;
  if (!initial_scan)
    throw std::logic_error(
        "TPC-H Q20 aux requires a typed initial scan source");

  BaseBuilder builder;

  // Lineitem series, grouped per (partkey, suppkey).
  const bool ordered_series = initial_scan->ScanLineitemsByPair(
      [&](int64_t partkey, int64_t suppkey, int32_t date, int64_t qty) {
        if (partkey < 0 || suppkey < 0)
          return;
        RequirePairKey(partkey, suppkey);
        builder.AddSeriesRow(MakePairKey(partkey, suppkey), date, qty);
      });
  if (!ordered_series) {
    struct Row {
      uint64_t key;
      int32_t date;
      int64_t qty;
    };
    std::vector<Row> rows;
    initial_scan->ScanLineitems([&](const TpchQ20LineitemInput &input) {
      if (input.partkey < 0 || input.suppkey < 0)
        return;
      RequirePairKey(input.partkey, input.suppkey);
      rows.push_back({MakePairKey(input.partkey, input.suppkey),
                      input.shipdate_days, input.quantity_raw});
    });
    std::stable_sort(rows.begin(), rows.end(),
                     [](const Row &a, const Row &b) { return a.key < b.key; });
    for (const auto &r : rows)
      builder.AddSeriesRow(r.key, r.date, r.qty);
  }
  builder.FinishSeries();

  const bool ordered_ps = initial_scan->ScanPartsuppsOrdered(
      [&](int64_t partkey, int64_t suppkey, int64_t availqty) {
        builder.AddPartsupp(partkey, suppkey, availqty);
      });
  if (!ordered_ps) {
    std::vector<TpchQ20PartsuppInput> rows;
    initial_scan->ScanPartsupps([&](const TpchQ20PartsuppInput &input) {
      rows.push_back(input);
    });
    std::sort(rows.begin(), rows.end(),
              [](const TpchQ20PartsuppInput &a, const TpchQ20PartsuppInput &b) {
                return a.partkey != b.partkey ? a.partkey < b.partkey
                                              : a.suppkey < b.suppkey;
              });
    for (const auto &r : rows)
      builder.AddPartsupp(r.partkey, r.suppkey, r.availqty);
  }

  if (!initial_scan->ScanPartNames(
          [&](int64_t key, const char *name, std::size_t len) {
            builder.AddPart(key, name, len);
          })) {
    initial_scan->ScanParts([&](const TpchQ20PartInput &input) {
      builder.AddPart(input.key, input.name.data(), input.name.size());
    });
  }

  initial_scan->ScanSuppliers([&](const TpchQ20SupplierInput &input) {
    builder.AddSupplier(input.key, input.name, input.address, input.nationkey);
  });

  initial_scan->ScanNations([&](const TpchQ20NationInput &input) {
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

void TpchQ20Module::EmitResult(const TpchQ20State &state,
                               int32_t start_date_days,
                               int32_t end_date_days,
                               const std::string &part_name_pattern,
                               const std::string &nation_name,
                               core::AuxEngineServices &services,
                               core::AuxRowSink &sink) const {
  (void)services;
  const TpchQ20Base &base = *state.base;

  const auto nation_keys = state.FindNationKeysByName(nation_name);
  if (nation_keys.empty()) {
    // No nation has this name: no supplier qualifies.
    sink.Begin(core::OutputSchema{});
    sink.End();
    return;
  }
  const auto is_target = [&](int32_t nationkey) {
    if (nationkey < 0)
      return false;
    for (const int32_t k : nation_keys)
      if (k == nationkey)
        return true;
    return false;
  };

  // The literal text before the first wildcard bounds a contiguous range of
  // the sorted base names [rank_lo, rank_hi).  A pattern that is exactly that
  // literal plus '%' (the TPC-H form) needs no further test; any other is
  // tested only on the ranks a read actually visits.
  const std::size_t wild = part_name_pattern.find_first_of("%_");
  const std::string prefix = part_name_pattern.substr(0, wild);
  const bool prefix_only = wild != std::string::npos &&
                           wild + 1 == part_name_pattern.size() &&
                           part_name_pattern[wild] == '%';
  const auto has_prefix = [&](std::string_view name) {
    return name.size() >= prefix.size() &&
           std::memcmp(name.data(), prefix.data(), prefix.size()) == 0;
  };
  const auto matches = [&](const char *name, std::size_t len) {
    if (prefix_only)
      return has_prefix(std::string_view(name, len));
    return SqlLikeAt(name, len, part_name_pattern.data(),
                     part_name_pattern.size(), 0, 0);
  };
  const std::size_t name_count = base.name_partkey.size();
  std::size_t lo = 0, hi = name_count;
  while (lo < hi) { // first name >= prefix
    const std::size_t mid = lo + (hi - lo) / 2;
    if (base.PartName(mid) < std::string_view(prefix))
      lo = mid + 1;
    else
      hi = mid;
  }
  const auto rank_lo = static_cast<uint32_t>(lo);
  hi = name_count;
  while (lo < hi) { // first name past the names starting with prefix
    const std::size_t mid = lo + (hi - lo) / 2;
    if (has_prefix(base.PartName(mid)))
      lo = mid + 1;
    else
      hi = mid;
  }
  const auto rank_hi = static_cast<uint32_t>(lo);
  const auto rank_ok = [&](uint32_t rank) {
    if (prefix_only)
      return true;
    const auto name = base.PartName(rank);
    return matches(name.data(), name.size());
  };

  const bool overlay_parts = state.parts.size() != 0;
  const bool overlay_ps = state.partsupps.size() != 0;
  const bool overlay_series = !state.series.Empty();
  const bool overlay_suppliers = state.suppliers.size() != 0;
  // A part whose name or partsupp list changed since the base is evaluated
  // from the overlays (pass 2), never from its base pairs.
  const auto part_changed = [&](int64_t partkey) {
    const int64_t k = partkey;
    return (overlay_parts && state.parts.Find(k)) ||
           (overlay_ps && state.partsupps.Find(k));
  };
  std::vector<int64_t> qualifying_suppkeys;

  // The pair qualifies when it has lineitems in the window and availqty
  // exceeds half their quantity.  The supplier's nation is checked by caller.
  const auto evaluate = [&](int64_t partkey, int64_t suppkey, int64_t availqty,
                            uint32_t series_idx, bool resolve_idx) {
    const uint64_t pair_key = MakePairKey(partkey, suppkey);
    int64_t sum = 0;
    bool has = false;
    const TpchQ20SeriesView ov =
        overlay_series ? state.series.Find(pair_key) : TpchQ20SeriesView{};
    if (ov.found) {
      for (const auto &e : ov) {
        if (e.shipdate_days < start_date_days)
          continue;
        if (e.shipdate_days >= end_date_days)
          break;
        has = true;
        sum += e.quantity_raw;
      }
    } else {
      if (resolve_idx)
        series_idx = base.FindSeries(pair_key);
      if (series_idx == TpchQ20Base::kNoSeries)
        return;
      has = base.WindowSum(series_idx, start_date_days, end_date_days, sum);
    }
    if (has && AvailQtyGreaterThanHalfSum(availqty, sum))
      qualifying_suppkeys.push_back(suppkey);
  };

  // Walks base pairs of `slot` in the name range; `keep` filters suppkeys.
  const auto walk_slot = [&](uint32_t slot, auto &&keep) {
    const auto [b, e] = base.RankRange(slot, rank_lo, rank_hi);
    constexpr std::size_t kAhead = 16;
    const auto *pairs = base.pairs.data();
    for (std::size_t i = b; i < e; ++i) {
      if (i + kAhead < e && pairs[i + kAhead].series_idx != TpchQ20Base::kNoSeries)
        __builtin_prefetch(&base.series_off[pairs[i + kAhead].series_idx]);
      if (i + kAhead / 2 < e &&
          pairs[i + kAhead / 2].series_idx != TpchQ20Base::kNoSeries)
        __builtin_prefetch(base.series_entries.data() +
                           base.series_off[pairs[i + kAhead / 2].series_idx]);
      if (overlay_series) {
        if (i + kAhead < e) {
          const auto &q = pairs[i + kAhead];
          state.series.PrefetchSlot(MakePairKey(base.name_partkey[q.rank], q.suppkey));
        }
        if (i + kAhead / 2 < e) {
          const auto &q = pairs[i + kAhead / 2];
          state.series.PrefetchData(MakePairKey(base.name_partkey[q.rank], q.suppkey));
        }
      }
      const auto &p = pairs[i];
      if (!keep(p.suppkey) || !rank_ok(p.rank))
        continue;
      const int64_t partkey = base.name_partkey[p.rank];
      if ((overlay_parts || overlay_ps) && part_changed(partkey))
        continue;
      evaluate(partkey, p.suppkey, base.Avail(p), p.series_idx, false);
    }
  };

  // 1. Base pairs whose supplier's base nation is a target nation (minus
  //    suppliers since moved out or deleted).
  for (const int32_t nationkey : nation_keys) {
    const uint32_t slot = base.SlotOfNation(nationkey);
    if (slot == TpchQ20Base::kNoSlot)
      continue;
    if (overlay_suppliers)
      walk_slot(slot, [&](int32_t suppkey) {
        return is_target(state.SupplierNation(suppkey));
      });
    else
      walk_slot(slot, [](int32_t) { return true; });
  }

  // 2. Parts whose name or partsupp list changed since the base.
  if (overlay_parts || overlay_ps) {
    std::vector<int64_t> changed;
    state.parts.ForEach([&](uint64_t k, const TpchQ20PartOverlay &) {
      changed.push_back(static_cast<int64_t>(k));
    });
    state.partsupps.ForEach([&](uint64_t k, const auto &) {
      changed.push_back(static_cast<int64_t>(k));
    });
    std::sort(changed.begin(), changed.end());
    changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
    for (const int64_t partkey : changed) {
      bool name_ok = false;
      if (const auto *ov = state.parts.Find(partkey)) {
        name_ok = ov->present && matches(ov->name.data(), ov->name.size());
      } else if (const uint32_t r = base.RankOf(partkey);
                 r != TpchQ20Base::kNoRank) {
        const auto name = base.PartName(r);
        name_ok = matches(name.data(), name.size());
        if (!name_ok) {
          const auto extra = base.extra_ranks.find(partkey);
          if (extra != base.extra_ranks.end())
            for (const uint32_t er : extra->second) {
              const auto n = base.PartName(er);
              name_ok = name_ok || matches(n.data(), n.size());
            }
        }
      }
      if (!name_ok)
        continue;
      const auto consider = [&](int64_t suppkey, int64_t availqty,
                                uint32_t series_idx, bool resolve_idx) {
        if (is_target(state.SupplierNation(suppkey)))
          evaluate(partkey, suppkey, availqty, series_idx, resolve_idx);
      };
      if (const auto *list =
              overlay_ps ? state.partsupps.Find(partkey)
                         : nullptr) {
        for (const auto &p : *list)
          consider(p.suppkey, p.availqty, TpchQ20Base::kNoSeries, true);
      } else {
        base.ForEachBasePair(partkey, [&](int64_t suppkey, int64_t availqty,
                                          uint32_t series_idx) {
          consider(suppkey, availqty, series_idx, false);
        });
      }
    }
  }

  // 3. Suppliers now in a target nation whose base nation is not (moved, or
  //    new): their base pairs sit in their base nation's slot.
  if (overlay_suppliers) {
    std::vector<std::pair<uint32_t, int32_t>> moved; // (base slot, suppkey)
    state.suppliers.ForEach([&](uint64_t k, const TpchQ20SupplierOverlay &ov) {
      if (!ov.present || !is_target(ov.record.nationkey))
        return;
      const auto suppkey = static_cast<int64_t>(k);
      const int32_t base_nation = base.BaseNation(suppkey);
      const bool in_base = base_nation >= 0 ||
                           base.BaseSupplier(suppkey) != nullptr;
      if (in_base && is_target(base_nation))
        return; // already walked in pass 1
      const uint32_t slot =
          in_base ? base.SlotOfNation(base_nation) : base.NoNationSlot();
      if (slot != TpchQ20Base::kNoSlot)
        moved.emplace_back(slot, static_cast<int32_t>(suppkey));
    });
    std::sort(moved.begin(), moved.end());
    for (std::size_t i = 0; i < moved.size();) {
      std::size_t j = i;
      while (j < moved.size() && moved[j].first == moved[i].first)
        ++j;
      walk_slot(moved[i].first, [&](int32_t suppkey) {
        return std::binary_search(
            moved.begin() + static_cast<std::ptrdiff_t>(i),
            moved.begin() + static_cast<std::ptrdiff_t>(j),
            std::pair<uint32_t, int32_t>{moved[i].first, suppkey});
      });
      i = j;
    }
  }

  std::sort(qualifying_suppkeys.begin(), qualifying_suppkeys.end());
  qualifying_suppkeys.erase(
      std::unique(qualifying_suppkeys.begin(), qualifying_suppkeys.end()),
      qualifying_suppkeys.end());

  // Output in name order.  A base supplier's name rank is its index in the
  // name-sorted records, so without supplier changes this sorts integers and
  // reads the records in memory order; a changed supplier (overlay record,
  // no rank) compares by name, after base suppliers of an equal name.
  struct OutRow {
    const TpchQ20SupplierRecord *rec;
    uint32_t rank; // kNoRank: overlay record
  };
  std::vector<OutRow> output_rows;
  output_rows.reserve(qualifying_suppkeys.size());
  for (const auto suppkey : qualifying_suppkeys) {
    const TpchQ20SupplierOverlay *ov =
        overlay_suppliers ? state.suppliers.Find(suppkey)
                          : nullptr;
    OutRow row{nullptr, TpchQ20Base::kNoRank};
    if (ov) {
      if (ov->present)
        row.rec = &ov->record;
    } else {
      row.rank = base.SupplierRank(suppkey);
      if (row.rank != TpchQ20Base::kNoRank)
        row.rec = &base.supplier_records[row.rank];
    }
    if (row.rec && is_target(row.rec->nationkey))
      output_rows.push_back(row);
  }
  std::sort(output_rows.begin(), output_rows.end(),
            [](const OutRow &a, const OutRow &b) {
              constexpr uint32_t kNone = TpchQ20Base::kNoRank;
              if (a.rank != kNone && b.rank != kNone)
                return a.rank < b.rank;
              const int c = a.rec->name.compare(b.rec->name);
              if (c != 0)
                return c < 0;
              return a.rank != kNone && b.rank == kNone;
            });

  sink.Begin(core::OutputSchema{});
  // One two-cell row buffer for every row: its strings keep their capacity.
  core::AuxRow out;
  out.values.resize(2);
  for (auto &cell : out.values) {
    cell.type = core::AuxType::String;
    cell.is_null = false;
    cell.payload = std::string();
  }
  for (const auto &row : output_rows) {
    std::get<std::string>(out.values[0].payload).assign(row.rec->name);
    std::get<std::string>(out.values[1].payload).assign(row.rec->address);
    sink.WriteRow(out);
  }
  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ20Module::CalibrationWrites() const {
  return TpchQ20CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
