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
constexpr uint32_t kVersion = 2;

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

inline uint64_t MakePairKey(int64_t partkey, int64_t suppkey) {
  return (static_cast<uint64_t>(partkey) << 20) |
         static_cast<uint64_t>(suppkey);
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

bool SqlLikeAt(const char *text, std::size_t text_len, const char *pattern,
               std::size_t pattern_len, std::size_t text_pos,
               std::size_t pattern_pos) {
  while (pattern_pos < pattern_len) {
    const char ch = pattern[pattern_pos];
    if (ch == '%') {
      if (pattern_pos + 1 == pattern_len)
        return true;
      for (std::size_t start = text_pos; start <= text_len; ++start) {
        if (SqlLikeAt(text, text_len, pattern, pattern_len, start,
                      pattern_pos + 1))
          return true;
      }
      return false;
    }
    if (ch == '_') {
      if (text_pos >= text_len)
        return false;
      ++text_pos;
      ++pattern_pos;
      continue;
    }
    if (text_pos >= text_len || text[text_pos] != ch)
      return false;
    ++text_pos;
    ++pattern_pos;
  }
  return text_pos == text_len;
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
constexpr int64_t kMaxPartkey = int64_t{1} << 28;
constexpr int64_t kMaxDenseSupplier = int64_t{1} << 22;

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
    if (partkey >= kMaxPartkey)
      throw std::runtime_error("TPC-H Q20 aux: partkey out of range");
    auto &ps_off = base->ps_off;
    while (ps_off.size() <= static_cast<std::size_t>(partkey))
      ps_off.push_back(static_cast<uint32_t>(base->ps.size()));
    TpchQ20Base::PsEntry e;
    e.availqty = availqty;
    e.suppkey = static_cast<int32_t>(suppkey);
    base->ps.push_back(e);
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
    base->suppliers[key] = {std::move(name), std::move(address), nationkey};
  }

  void AddNation(int32_t key, const std::string &name) {
    if (key < 0)
      return;
    base->nations[key] = name;
    base->nation_name_index[name].push_back(key);
  }

  std::shared_ptr<const TpchQ20Base> Finish() {
    CloseSeries();
    // partsupp offsets + series index resolution (both sides sorted).
    base->ps_off.push_back(static_cast<uint32_t>(base->ps.size()));
    {
      const auto &keys = base->series_key;
      std::size_t j = 0;
      for (std::size_t part = 0; part + 1 < base->ps_off.size(); ++part) {
        for (uint32_t i = base->ps_off[part]; i < base->ps_off[part + 1]; ++i) {
          auto &e = base->ps[i];
          const uint64_t key =
              MakePairKey(static_cast<int64_t>(part), e.suppkey);
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
    }
    FinishNames();
    FinishSuppliers();
    return std::move(base);
  }

private:
  struct NameRef {
    int64_t key;
    uint64_t off;
    uint32_t len;
  };

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
    names.clear();
    names.shrink_to_fit();
    arena.clear();
    arena.shrink_to_fit();
  }

  void FinishSuppliers() {
    int64_t max_key = -1;
    for (const auto &[key, rec] : base->suppliers) {
      (void)rec;
      if (key < kMaxDenseSupplier)
        max_key = std::max(max_key, key);
    }
    base->supp_nation.assign(static_cast<std::size_t>(max_key + 1), -1);
    for (const auto &[key, rec] : base->suppliers)
      if (key <= max_key)
        base->supp_nation[static_cast<std::size_t>(key)] = rec.nationkey;
  }

  std::shared_ptr<TpchQ20Base> base;
  std::vector<TpchQ20DateQuantity> cur;
  uint64_t cur_key = 0;
  bool has_cur = false;
  std::vector<NameRef> names;
  std::string arena;
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

int32_t TpchQ20State::SupplierNation(int64_t suppkey) const {
  if (!suppliers.Empty()) {
    if (const auto *ov = suppliers.Find(static_cast<uint64_t>(suppkey)))
      return ov->present ? ov->record.nationkey : -1;
  }
  if (suppkey >= 0 &&
      static_cast<std::size_t>(suppkey) < base->supp_nation.size())
    return base->supp_nation[static_cast<std::size_t>(suppkey)];
  if (suppkey >= kMaxDenseSupplier) {
    const auto it = base->suppliers.find(suppkey);
    if (it != base->suppliers.end())
      return it->second.nationkey;
  }
  return -1;
}

bool TpchQ20State::FindSupplier(int64_t suppkey,
                                TpchQ20SupplierRecord &out) const {
  if (!suppliers.Empty()) {
    if (const auto *ov = suppliers.Find(static_cast<uint64_t>(suppkey))) {
      if (!ov->present)
        return false;
      out = ov->record;
      return true;
    }
  }
  const auto it = base->suppliers.find(suppkey);
  if (it == base->suppliers.end())
    return false;
  out = it->second;
  return true;
}

bool TpchQ20State::PairWindowSum(uint64_t pair_key, int32_t lo, int32_t hi,
                                 int64_t &sum) const {
  if (!series.Empty()) {
    if (const auto *ov = series.Find(pair_key)) {
      sum = 0;
      bool has = false;
      for (const auto &e : *ov) {
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
  state.series.ForEach([&](uint64_t k, const auto &) { keys.push_back(k); });
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  struct SeriesOut {
    uint64_t key;
    std::vector<TpchQ20DateQuantity> entries;
  };
  std::vector<SeriesOut> series_out;
  for (const uint64_t k : keys) {
    std::vector<TpchQ20DateQuantity> entries;
    if (const auto *ov = state.series.Find(k)) {
      entries = *ov;
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
  for (std::size_t part = 0; part + 1 < base.ps_off.size(); ++part) {
    if (state.partsupps.Find(part))
      continue;
    for (uint32_t i = base.ps_off[part]; i < base.ps_off[part + 1]; ++i)
      ps_rows.push_back({static_cast<int64_t>(part), base.ps[i].suppkey,
                         base.ps[i].availqty});
  }
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
    if (state.parts.Find(static_cast<uint64_t>(base.name_partkey[i])))
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
  std::map<int64_t, TpchQ20SupplierRecord> supp_rows(base.suppliers.begin(),
                                                     base.suppliers.end());
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
          partkey < 0 || suppkey < 0) {
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
      if (!ReadPartsupp(row, partkey, suppkey, availqty) || partkey < 0 ||
          suppkey < 0) {
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

  // Overlay compaction: past this bound the cheapest exact "compaction" is a
  // fresh bulk build (the overlays hold changes since the base image).
  constexpr std::size_t kMaxOverlayWeight = 400000;
  if (parent->OverlayWeight() + delta_count > kMaxOverlayWeight) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }

  // The staged state shares every overlay shard with its parent; only the
  // shards touched below are cloned, so published versions stay immutable.
  auto next = std::make_shared<TpchQ20State>(*parent);
  const TpchQ20Base &base = *next->base;

  // Lineitem deltas: per-pair series, copied from base on first touch.
  for (const auto &delta : buffer.LineitemDeltas()) {
    const uint64_t pair_key = MakePairKey(delta.partkey, delta.suppkey);
    bool inserted = false;
    auto &series = next->series.Mutable(pair_key, &inserted);
    if (inserted) {
      const uint32_t idx = base.FindSeries(pair_key);
      if (idx != TpchQ20Base::kNoSeries)
        series = BaseSeriesCopy(base, idx);
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

  // Partsupp deltas: per-part complete list, copied from base on first touch.
  for (const auto &delta : buffer.PartsuppDeltas()) {
    bool inserted = false;
    auto &list = next->partsupps.Mutable(static_cast<uint64_t>(delta.partkey),
                                         &inserted);
    if (inserted) {
      const std::size_t part = static_cast<std::size_t>(delta.partkey);
      if (part + 1 < base.ps_off.size()) {
        for (uint32_t i = base.ps_off[part]; i < base.ps_off[part + 1]; ++i)
          list.push_back({base.ps[i].suppkey, base.ps[i].availqty});
      }
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
    auto &rec = next->parts.Mutable(static_cast<uint64_t>(delta.partkey));
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
    auto &rec = next->suppliers.Mutable(static_cast<uint64_t>(delta.suppkey));
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

  // Exact trailing-'%' prefix matching is a contiguous range of the sorted
  // name array; any other pattern goes through SqlLike.
  const bool trailing_percent_only =
      !part_name_pattern.empty() && part_name_pattern.back() == '%' &&
      part_name_pattern.find('%') == part_name_pattern.size() - 1 &&
      part_name_pattern.find('_') == std::string::npos;
  const std::string prefix =
      trailing_percent_only
          ? part_name_pattern.substr(0, part_name_pattern.size() - 1)
          : std::string();
  const auto matches = [&](const char *name, std::size_t len) {
    if (trailing_percent_only)
      return len >= prefix.size() &&
             std::memcmp(name, prefix.data(), prefix.size()) == 0;
    return SqlLikeAt(name, len, part_name_pattern.data(),
                     part_name_pattern.size(), 0, 0);
  };

  // Qualifying partkeys: base names (minus parts overridden in the overlay)
  // plus live overlay names.
  std::vector<int64_t> partkeys;
  const std::size_t name_count = base.name_partkey.size();
  const bool overlay_parts = !state.parts.Empty();
  const auto take_base = [&](std::size_t i) {
    const int64_t key = base.name_partkey[i];
    if (overlay_parts && state.parts.Find(static_cast<uint64_t>(key)))
      return;
    partkeys.push_back(key);
  };
  if (trailing_percent_only) {
    std::size_t lo = 0, hi = name_count;
    while (lo < hi) { // first name >= prefix
      const std::size_t mid = lo + (hi - lo) / 2;
      if (base.PartName(mid) < std::string_view(prefix))
        lo = mid + 1;
      else
        hi = mid;
    }
    for (std::size_t i = lo; i < name_count; ++i) {
      const auto name = base.PartName(i);
      if (!matches(name.data(), name.size()))
        break;
      take_base(i);
    }
  } else {
    for (std::size_t i = 0; i < name_count; ++i) {
      const auto name = base.PartName(i);
      if (matches(name.data(), name.size()))
        take_base(i);
    }
  }
  if (overlay_parts) {
    state.parts.ForEach([&](uint64_t key, const TpchQ20PartOverlay &ov) {
      if (ov.present && matches(ov.name.data(), ov.name.size()))
        partkeys.push_back(static_cast<int64_t>(key));
    });
  }

  const bool overlay_ps = !state.partsupps.Empty();
  const bool overlay_series = !state.series.Empty();
  std::vector<int64_t> qualifying_suppkeys;

  const auto consider = [&](int64_t partkey, int64_t suppkey, int64_t availqty,
                            uint32_t series_idx, bool resolve_idx) {
    // Supplier nation first: only ~1/25 of pairs reach the lineitem series.
    if (!is_target(state.SupplierNation(suppkey)))
      return;
    const uint64_t pair_key = MakePairKey(partkey, suppkey);
    int64_t sum = 0;
    bool has = false;
    const std::vector<TpchQ20DateQuantity> *ov =
        overlay_series ? state.series.Find(pair_key) : nullptr;
    if (ov) {
      for (const auto &e : *ov) {
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

  constexpr std::size_t kAhead = 16;
  const std::size_t n = partkeys.size();
  const std::size_t ps_off_size = base.ps_off.size();
  for (std::size_t i = 0; i < n; ++i) {
    if (i + kAhead < n) {
      const uint64_t k = static_cast<uint64_t>(partkeys[i + kAhead]);
      if (k + 1 < ps_off_size)
        __builtin_prefetch(&base.ps_off[k]);
    }
    if (i + kAhead / 2 < n) {
      const uint64_t k = static_cast<uint64_t>(partkeys[i + kAhead / 2]);
      if (k + 1 < ps_off_size)
        __builtin_prefetch(base.ps.data() + base.ps_off[k]);
    }
    const int64_t partkey = partkeys[i];
    const std::vector<TpchQ20PsPair> *ps_ov =
        overlay_ps ? state.partsupps.Find(static_cast<uint64_t>(partkey))
                   : nullptr;
    if (ps_ov) {
      for (const auto &p : *ps_ov)
        consider(partkey, p.suppkey, p.availqty, TpchQ20Base::kNoSeries, true);
      continue;
    }
    const uint64_t k = static_cast<uint64_t>(partkey);
    if (k + 1 >= ps_off_size)
      continue;
    for (uint32_t j = base.ps_off[k]; j < base.ps_off[k + 1]; ++j) {
      const auto &e = base.ps[j];
      consider(partkey, e.suppkey, e.availqty, e.series_idx, false);
    }
  }

  std::sort(qualifying_suppkeys.begin(), qualifying_suppkeys.end());
  qualifying_suppkeys.erase(
      std::unique(qualifying_suppkeys.begin(), qualifying_suppkeys.end()),
      qualifying_suppkeys.end());

  std::vector<TpchQ20OutputRow> output_rows;
  output_rows.reserve(qualifying_suppkeys.size());
  for (const auto suppkey : qualifying_suppkeys) {
    TpchQ20SupplierRecord supplier;
    if (!state.FindSupplier(suppkey, supplier))
      continue;
    if (!is_target(supplier.nationkey))
      continue;
    output_rows.push_back(
        {std::move(supplier.name), std::move(supplier.address)});
  }

  std::sort(output_rows.begin(), output_rows.end(),
            [](const TpchQ20OutputRow &a, const TpchQ20OutputRow &b) {
              return a.name < b.name;
            });

  sink.Begin(core::OutputSchema{});
  for (const auto &row : output_rows) {
    core::AuxRow out;
    out.values.reserve(2);
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::String, row.name));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::String, row.address));
    sink.WriteRow(out);
  }
  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ20Module::CalibrationWrites() const {
  return TpchQ20CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
