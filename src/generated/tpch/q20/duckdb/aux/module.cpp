#include "tpch/q20/duckdb/aux/module.hpp"
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
constexpr uint32_t kVersion = 1;
constexpr int32_t kOneYearDays = 365;
static bool CustomProfileEnabled() {
  static const bool enabled = [] {
    const char *v = std::getenv("ACCEL_AUX_CUSTOM_PROFILE");
    return v && (v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' ||
                 v[0] == 'Y');
  }();
  return enabled;
}

static void EmitCustomProfile(const char *phase, double us,
                              const std::vector<std::pair<const char *, int64_t>> &counts) {
  if (!CustomProfileEnabled())
    return;
  std::string json = "ACCEL_CUSTOM_PROFILE {\"phase\":\"";
  json += phase;
  json += "\",\"us\":";
  json += std::to_string(us);
  for (const auto &[key, val] : counts) {
    json += ",\"";
    json += key;
    json += "\":";
    json += std::to_string(val);
  }
  json += "}";
  fprintf(stderr, "%s\n", json.c_str());
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

bool ReadNation(const core::AuxRow &row, int32_t &nationkey,
                std::string &name) {
  return Int32Cell(row, 0, nationkey) && StringCell(row, 1, name);
}

} // namespace

// ── TpchQ20State accessors ──────────────────────────────────────────────────
void TpchQ20State::AddPairEntry(uint64_t pair_key, int32_t shipdate_days,
                                int64_t quantity_raw) {
  // Copy the full ancestor series (not just immediate parent) so stacked
  // overlay publishes still see base lineitem history.
  if (pair_series.find(pair_key) == pair_series.end() && parent_state) {
    if (const auto *src = parent_state->FindPairSeries(pair_key))
      pair_series[pair_key] = *src;
  }
  auto &series = pair_series[pair_key];
  series.entries.push_back({shipdate_days, quantity_raw});
  series.total_quantity_raw += quantity_raw;
  series.dirty = true;
}

void TpchQ20State::RemovePairEntry(uint64_t pair_key, int32_t shipdate_days,
                                   int64_t quantity_raw) {
  if (pair_series.find(pair_key) == pair_series.end() && parent_state) {
    if (const auto *src = parent_state->FindPairSeries(pair_key))
      pair_series[pair_key] = *src;
  }
  auto it = pair_series.find(pair_key);
  if (it == pair_series.end())
    return;
  auto &series = it->second;
  for (auto entry_it = series.entries.begin();
       entry_it != series.entries.end(); ++entry_it) {
    if (entry_it->shipdate_days == shipdate_days &&
        entry_it->quantity_raw == quantity_raw) {
      series.entries.erase(entry_it);
      series.total_quantity_raw -= quantity_raw;
      series.dirty = true;
      break;
    }
  }
  if (series.entries.empty())
    pair_series.erase(it);
}

int64_t TpchQ20State::SumPairQuantity(uint64_t pair_key,
                                      int32_t start_date_days,
                                      int32_t end_date_days) const {
  auto it = pair_series.find(pair_key);
  if (it == pair_series.end()) {
    if (parent_state) return parent_state->SumPairQuantity(pair_key, start_date_days, end_date_days);
    return 0;
  }
  const auto &entries = it->second.entries;
  int64_t total = 0;
  for (const auto &entry : entries) {
    if (entry.shipdate_days >= start_date_days &&
        entry.shipdate_days < end_date_days) {
      total += entry.quantity_raw;
    }
  }
  return total;
}

bool TpchQ20State::HasPairQuantityInWindow(
    uint64_t pair_key, int32_t start_date_days, int32_t end_date_days) const {
  auto it = pair_series.find(pair_key);
  if (it == pair_series.end()) {
    if (parent_state) return parent_state->HasPairQuantityInWindow(pair_key, start_date_days, end_date_days);
    return false;
  }
  const auto &entries = it->second.entries;
  // Entries are sorted by shipdate_days after build/delta apply. Skip the
  // prefix outside the window and stop as soon as the window ends.
  for (const auto &entry : entries) {
    if (entry.shipdate_days < start_date_days)
      continue;
    if (entry.shipdate_days >= end_date_days)
      break;
    return true;
  }
  return false;
}

std::pair<int64_t, bool> TpchQ20State::QueryWindowSum(
    uint64_t pair_key, int32_t start_date_days, int32_t end_date_days) const {
  auto it = pair_series.find(pair_key);
  if (it == pair_series.end()) {
    if (parent_state) return parent_state->QueryWindowSum(pair_key, start_date_days, end_date_days);
    return {-1, false};
  }
  const auto &entries = it->second.entries;
  int64_t total = 0;
  bool has_entry = false;
  for (const auto &entry : entries) {
    if (entry.shipdate_days < start_date_days)
      continue;
    if (entry.shipdate_days >= end_date_days)
      break;
    has_entry = true;
    total += entry.quantity_raw;
  }
  return {total, has_entry};
}

const TpchQ20PairSeries *
TpchQ20State::FindPairSeries(uint64_t pair_key) const {
  const auto it = pair_series.find(pair_key);
  if (it != pair_series.end())
    return &it->second;
  if (parent_state)
    return parent_state->FindPairSeries(pair_key);
  return nullptr;
}

void TpchQ20State::SortPairEntries() {
  for (auto &[key, series] : pair_series) {
    (void)key;
    if (!series.dirty)
      continue;
    std::sort(series.entries.begin(), series.entries.end(),
              [](const TpchQ20DateQuantity &a, const TpchQ20DateQuantity &b) {
                return a.shipdate_days < b.shipdate_days;
              });
    series.dirty = false;
  }
}

const TpchQ20SupplierRecord *
TpchQ20State::FindSupplier(int64_t suppkey) const {
  const auto it = suppliers.find(suppkey);
  if (it != suppliers.end())
    return &it->second;
  if (parent_state)
    return parent_state->FindSupplier(suppkey);
  return nullptr;
}

const TpchQ20PartRecord *TpchQ20State::FindPart(int64_t partkey) const {
  const auto it = parts.find(partkey);
  if (it != parts.end())
    return &it->second;
  if (parent_state)
    return parent_state->FindPart(partkey);
  return nullptr;
}

void TpchQ20State::BuildFlatPartsuppKeys() {
  flat_partsupp_keys.clear();
  flat_partsupp_keys.reserve(parts.size() * 4 + 16);
  for (const auto &[partkey, part] : parts) {
    for (const auto &[suppkey, availqty] : part.partsupps) {
      const uint64_t pair_key = MakePairKey(partkey, suppkey);
      const TpchQ20PairSeries *series = FindPairSeries(pair_key);
      flat_partsupp_keys.push_back({pair_key, availqty, series});
    }
  }
  std::sort(flat_partsupp_keys.begin(), flat_partsupp_keys.end(),
            [](const auto &a, const auto &b) { return a.pair_key < b.pair_key; });
  flat_partsupp_keys_sorted = true;
}

std::vector<int64_t>
TpchQ20State::FindPartKeysByPrefix(const std::string &prefix) const {
  std::vector<int64_t> result;
  std::unordered_set<int64_t> seen;
  auto collect = [&](const TpchQ20State &s) {
    auto it = s.part_name_index.lower_bound(prefix);
    for (; it != s.part_name_index.end(); ++it) {
      if (it->first.size() < prefix.size())
        break;
      if (it->first.compare(0, prefix.size(), prefix) != 0)
        break;
      for (auto key : it->second) {
        if (seen.insert(key).second)
          result.push_back(key);
      }
    }
  };
  for (const TpchQ20State *s = this; s; s = s->parent_state.get())
    collect(*s);
  return result;
}

std::vector<int32_t>
TpchQ20State::FindNationKeysByName(const std::string &name) const {
  const auto it = nation_name_index.find(name);
  if (it != nation_name_index.end())
    return it->second;
  if (parent_state)
    return parent_state->FindNationKeysByName(name);
  return {};
}

// ── Encode / Decode ─────────────────────────────────────────────────────────
std::vector<uint8_t> EncodeTpchQ20State(const TpchQ20State &state) {
  ByteWriter writer;
  core::WriteImageHeader(writer, kStateMagic, kVersion,
                         static_cast<uint32_t>(state.pair_series.size()));

  writer.U32(static_cast<uint32_t>(state.pair_series.size()));
  for (const auto &[key, series] : state.pair_series) {
    writer.U64(key);
    writer.I64(series.total_quantity_raw);
    writer.U32(static_cast<uint32_t>(series.entries.size()));
    for (const auto &entry : series.entries) {
      writer.I32(entry.shipdate_days);
      writer.I64(entry.quantity_raw);
    }
  }

  writer.U32(static_cast<uint32_t>(state.part_name_index.size()));
  for (const auto &[name, keys] : state.part_name_index) {
    writer.String(name);
    writer.U32(static_cast<uint32_t>(keys.size()));
    for (const auto key : keys)
      writer.I64(key);
  }

  writer.U32(static_cast<uint32_t>(state.parts.size()));
  for (const auto &[key, part] : state.parts) {
    writer.I64(key);
    writer.String(part.name);
    writer.U32(static_cast<uint32_t>(part.partsupps.size()));
    for (const auto &[suppkey, availqty] : part.partsupps) {
      writer.I64(suppkey);
      writer.I64(availqty);
    }
  }

  writer.U32(static_cast<uint32_t>(state.suppliers.size()));
  for (const auto &[key, supplier] : state.suppliers) {
    writer.I64(key);
    writer.String(supplier.name);
    writer.String(supplier.address);
    writer.I32(supplier.nationkey);
  }

  writer.U32(static_cast<uint32_t>(state.nations.size()));
  for (const auto &[key, nation] : state.nations) {
    writer.I32(key);
    writer.String(nation.name);
  }

  writer.U32(static_cast<uint32_t>(state.nation_name_index.size()));
  for (const auto &[name, keys] : state.nation_name_index) {
    writer.String(name);
    writer.U32(static_cast<uint32_t>(keys.size()));
    for (const auto key : keys)
      writer.I32(key);
  }

  writer.U32(static_cast<uint32_t>(state.flat_partsupp_keys.size()));
  for (const auto &entry : state.flat_partsupp_keys) {
    writer.U64(entry.pair_key);
    writer.I64(entry.availqty);
  }

  return std::move(writer.bytes);
}

bool DecodeTpchQ20State(const std::vector<uint8_t> &bytes,
                        TpchQ20State &state) {
  ByteReader reader(bytes);
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kStateMagic, kVersion, count))
    return false;

  state = TpchQ20State{};

  uint32_t series_count = 0;
  if (!reader.U32(series_count))
    return false;
  for (uint32_t i = 0; i < series_count; ++i) {
    uint64_t key = 0;
    TpchQ20PairSeries series;
    if (!reader.U64(key) || !reader.I64(series.total_quantity_raw))
      return false;
    uint32_t entry_count = 0;
    if (!reader.U32(entry_count))
      return false;
    series.entries.reserve(entry_count);
    for (uint32_t j = 0; j < entry_count; ++j) {
      TpchQ20DateQuantity entry;
      if (!reader.I32(entry.shipdate_days) || !reader.I64(entry.quantity_raw))
        return false;
      series.entries.push_back(entry);
    }
    state.pair_series[key] = std::move(series);
  }

  uint32_t name_idx_count = 0;
  if (!reader.U32(name_idx_count))
    return false;
  for (uint32_t i = 0; i < name_idx_count; ++i) {
    std::string name;
    if (!reader.String(name))
      return false;
    uint32_t key_count = 0;
    if (!reader.U32(key_count))
      return false;
    std::vector<int64_t> keys;
    keys.reserve(key_count);
    for (uint32_t j = 0; j < key_count; ++j) {
      int64_t k = 0;
      if (!reader.I64(k))
        return false;
      keys.push_back(k);
    }
    state.part_name_index[std::move(name)] = std::move(keys);
  }

  uint32_t parts_count = 0;
  if (!reader.U32(parts_count))
    return false;
  for (uint32_t i = 0; i < parts_count; ++i) {
    int64_t key = 0;
    TpchQ20PartRecord part;
    if (!reader.I64(key) || !reader.String(part.name))
      return false;
    uint32_t ps_count = 0;
    if (!reader.U32(ps_count))
      return false;
    part.partsupps.reserve(ps_count);
    for (uint32_t j = 0; j < ps_count; ++j) {
      int64_t suppkey = 0;
      int64_t availqty = 0;
      if (!reader.I64(suppkey) || !reader.I64(availqty))
        return false;
      part.partsupps[suppkey] = availqty;
    }
    state.parts[key] = std::move(part);
  }

  uint32_t supplier_count = 0;
  if (!reader.U32(supplier_count))
    return false;
  for (uint32_t i = 0; i < supplier_count; ++i) {
    int64_t key = 0;
    TpchQ20SupplierRecord supplier;
    if (!reader.I64(key) || !reader.String(supplier.name) ||
        !reader.String(supplier.address) || !reader.I32(supplier.nationkey))
      return false;
    state.suppliers[key] = std::move(supplier);
  }

  uint32_t nation_count = 0;
  if (!reader.U32(nation_count))
    return false;
  for (uint32_t i = 0; i < nation_count; ++i) {
    int32_t key = 0;
    TpchQ20NationRecord nation;
    if (!reader.I32(key) || !reader.String(nation.name))
      return false;
    state.nations[key] = std::move(nation);
  }

  uint32_t nni_count = 0;
  if (!reader.U32(nni_count))
    return false;
  for (uint32_t i = 0; i < nni_count; ++i) {
    std::string name;
    if (!reader.String(name))
      return false;
    uint32_t key_count = 0;
    if (!reader.U32(key_count))
      return false;
    std::vector<int32_t> keys;
    keys.reserve(key_count);
    for (uint32_t j = 0; j < key_count; ++j) {
      int32_t k = 0;
      if (!reader.I32(k))
        return false;
      keys.push_back(k);
    }
    state.nation_name_index[std::move(name)] = std::move(keys);
  }

  uint32_t flat_count = 0;
  if (!reader.U32(flat_count))
    return true;
  state.flat_partsupp_keys.reserve(flat_count);
  for (uint32_t i = 0; i < flat_count; ++i) {
    uint64_t pair_key = 0;
    int64_t availqty = 0;
    if (!reader.U64(pair_key) || !reader.I64(availqty))
      return true;
    state.flat_partsupp_keys.push_back({pair_key, availqty, nullptr});
  }
  state.flat_partsupp_keys_sorted = true;
  // Rebuild series pointers after decode (not serialized).
  for (auto &entry : state.flat_partsupp_keys) {
    auto it = state.pair_series.find(entry.pair_key);
    if (it != state.pair_series.end())
      entry.series = &it->second;
  }

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
}
bool TpchQ20TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
void TpchQ20TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  lineitem_deltas.clear();
  partsupp_deltas.clear();
  part_name_deltas.clear();
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
  part_name_deltas.push_back(delta);
}

// ── Module implementation ───────────────────────────────────────────────────
core::AcceleratorId TpchQ20Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ20Module::Dependencies() const {
  return {{config.lineitem_table, {0, 1, 2, 3}},
          {config.part_table, {0, 1}},
          {config.partsupp_table, {0, 1, 2}},
          {config.supplier_table, {0, 1, 2, 3}},
          {config.nation_table, {0, 1}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ20Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ20TxnBuffer>();
}

void TpchQ20Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  auto state = BuildState(services);
  // BuildFlatPartsuppKeys stores pointers into pair_series; run it on the
  // final shared object so pointers are alive for the lifetime of the state.
  auto typed_state = std::make_shared<TpchQ20State>(std::move(state));
  typed_state->BuildFlatPartsuppKeys();
  builder.Write(core::StateImage::Make(
      TpchQ20Layout::kAccelerator, TpchQ20Layout::kFullState, 0,
      MakeStateImage(typed_state)));
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
      if (!ReadLineitem(row, shipdate, partkey, suppkey, quantity)) {
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
      if (!ReadPartsupp(row, partkey, suppkey, availqty)) {
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
      if (!ReadPart(row, partkey, name)) {
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
            !ReadPart(mutation.new_rows[i], new_key, new_name)) {
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

  if (mutation.table == config.supplier_table ||
      mutation.table == config.nation_table) {
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
  if (!has_lineitem && !has_partsupp && !has_part)
    return outcome;

  const uint64_t delta_count =
      buffer.LineitemDeltas().size() + buffer.PartsuppDeltas().size() +
      buffer.PartNameDeltas().size();
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
  std::shared_ptr<TpchQ20State> new_state;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ20Layout::kFullState) {
      const auto *typed = AsQ20Image(image);
      if (!typed) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
      new_state = std::make_shared<TpchQ20State>();
      new_state->parent_state = typed->state;
      existing_version = image.version;
      found = true;
      break;
    }
  }
  if (!found) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  // Apply lineitem deltas
  for (const auto &delta : buffer.LineitemDeltas()) {
    const auto pair_key = MakePairKey(delta.partkey, delta.suppkey);
    if (delta.sign > 0) {
      new_state->AddPairEntry(pair_key, delta.shipdate_days,
                              delta.quantity_raw);
    } else {
      new_state->RemovePairEntry(pair_key, delta.shipdate_days,
                                 delta.quantity_raw);
    }
  }
  // Re-sort affected series after lineitem delta application.
  new_state->SortPairEntries();

  // Apply partsupp deltas (lazy-copy parent part records when needed)
  for (const auto &delta : buffer.PartsuppDeltas()) {
    if (delta.sign > 0) {
      if (new_state->parts.find(delta.partkey) == new_state->parts.end()) {
        const auto *parent_part =
            new_state->parent_state ? new_state->parent_state->FindPart(delta.partkey) : nullptr;
        if (parent_part)
          new_state->parts[delta.partkey] = *parent_part;
      }
      new_state->parts[delta.partkey].partsupps[delta.suppkey] =
          delta.availqty;
    } else {
      if (new_state->parts.find(delta.partkey) == new_state->parts.end()) {
        const auto *parent_part =
            new_state->parent_state ? new_state->parent_state->FindPart(delta.partkey) : nullptr;
        if (parent_part)
          new_state->parts[delta.partkey] = *parent_part;
      }
      auto part_it = new_state->parts.find(delta.partkey);
      if (part_it != new_state->parts.end())
        part_it->second.partsupps.erase(delta.suppkey);
    }
  }

  // Apply part name deltas
  for (const auto &delta : buffer.PartNameDeltas()) {
    if (delta.sign == 0 && !delta.old_name.empty() &&
        new_state->parts.find(delta.partkey) == new_state->parts.end()) {
      const auto *parent_part =
          new_state->parent_state ? new_state->parent_state->FindPart(delta.partkey) : nullptr;
      if (parent_part)
        new_state->parts[delta.partkey] = *parent_part;
    }
    if (delta.sign > 0) {
      // Insert
      if (!delta.new_name.empty()) {
        new_state->parts[delta.partkey].name = delta.new_name;
        new_state->part_name_index[delta.new_name].push_back(delta.partkey);
      }
    } else if (delta.sign < 0) {
      // Delete
      auto idx_it = new_state->part_name_index.find(delta.new_name);
      if (idx_it == new_state->part_name_index.end() &&
          new_state->parent_state) {
        const auto parent_keys =
            new_state->parent_state->FindPartKeysByPrefix(delta.new_name);
        for (auto key : parent_keys)
          new_state->part_name_index[delta.new_name].push_back(key);
        idx_it = new_state->part_name_index.find(delta.new_name);
      }
      if (idx_it != new_state->part_name_index.end()) {
        auto &idx = idx_it->second;
        idx.erase(std::remove(idx.begin(), idx.end(), delta.partkey),
                  idx.end());
        if (idx.empty())
          new_state->part_name_index.erase(idx_it);
      }
      new_state->parts.erase(delta.partkey);
    } else {
      // Update (sign == 0)
      if (!delta.old_name.empty()) {
        auto old_idx_it = new_state->part_name_index.find(delta.old_name);
        if (old_idx_it == new_state->part_name_index.end() &&
            new_state->parent_state) {
          const auto parent_keys =
              new_state->parent_state->FindPartKeysByPrefix(delta.old_name);
          for (auto key : parent_keys)
            new_state->part_name_index[delta.old_name].push_back(key);
          old_idx_it = new_state->part_name_index.find(delta.old_name);
        }
        if (old_idx_it != new_state->part_name_index.end()) {
          auto &old_idx = old_idx_it->second;
          old_idx.erase(
              std::remove(old_idx.begin(), old_idx.end(), delta.partkey),
              old_idx.end());
          if (old_idx.empty())
            new_state->part_name_index.erase(old_idx_it);
        }
      }
      if (!delta.new_name.empty()) {
        new_state->part_name_index[delta.new_name].push_back(delta.partkey);
        auto part_it = new_state->parts.find(delta.partkey);
        if (part_it != new_state->parts.end())
          part_it->second.name = delta.new_name;
      }
    }
  }

  // Publish the overlay state. It carries a parent pointer for lookups,
  // so we do NOT merge or deep-copy the large base map here. Delta stays local.
  outcome.action = core::MaintenanceAction::ApplyDelta;
  const auto typed_state =
      std::static_pointer_cast<const TpchQ20State>(std::move(new_state));
  outcome.updates.push_back(core::StateImage::Make(
      TpchQ20Layout::kAccelerator, TpchQ20Layout::kFullState, existing_version,
      MakeStateImage(typed_state)));
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

  const int32_t end_date_days = start_date_days + kOneYearDays;

  {
    auto t0 = std::chrono::steady_clock::now();
    const bool valid = services.StateStore()
        .CheckValidity({Id(), {}}, services.CurrentVisibility())
        .IsValid();
    auto t1 = std::chrono::steady_clock::now();
    EmitCustomProfile("check_validity",
                      std::chrono::duration<double, std::micro>(t1 - t0).count(),
                      {{"valid", valid ? static_cast<int64_t>(1) : static_cast<int64_t>(0)}});
    if (!valid) {
      ExecuteFallback(services, arguments, sink);
      return;
    }
  }

  std::shared_ptr<const TpchQ20State> state;
  {
    auto t0 = std::chrono::steady_clock::now();
    const bool loaded = LoadState(services, state);
    auto t1 = std::chrono::steady_clock::now();
    EmitCustomProfile("load_state",
                      std::chrono::duration<double, std::micro>(t1 - t0).count(),
                      {{"loaded", loaded ? static_cast<int64_t>(1) : static_cast<int64_t>(0)}});
    if (!loaded) {
      ExecuteFallback(services, arguments, sink);
      return;
    }
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

  const int32_t end_date_days = start_date_days + kOneYearDays;
  const auto state = BuildState(services);
  EmitResult(state, start_date_days, end_date_days, part_name_pattern,
             nation_name, services, sink);
}

TpchQ20State
TpchQ20Module::BuildState(core::AuxEngineServices &services) const {
  (void)services;
  TpchQ20State state;
  if (!initial_scan)
    throw std::logic_error(
        "TPC-H Q20 aux requires a typed initial scan source");

  initial_scan->ScanLineitems(
      [&](const TpchQ20LineitemInput &input) {
        if (input.partkey < 0 || input.suppkey < 0)
          return;
        const auto key = MakePairKey(input.partkey, input.suppkey);
        state.AddPairEntry(key, input.shipdate_days, input.quantity_raw);
      });

  initial_scan->ScanParts([&](const TpchQ20PartInput &input) {
    if (input.key < 0)
      return;
    TpchQ20PartRecord part;
    part.name = input.name;
    state.parts[input.key] = std::move(part);
    state.part_name_index[input.name].push_back(input.key);
  });

  initial_scan->ScanPartsupps([&](const TpchQ20PartsuppInput &input) {
    if (input.partkey < 0 || input.suppkey < 0)
      return;
    auto part_it = state.parts.find(input.partkey);
    if (part_it == state.parts.end()) {
      TpchQ20PartRecord part;
      part.partsupps[input.suppkey] = input.availqty;
      state.parts[input.partkey] = std::move(part);
    } else {
      part_it->second.partsupps[input.suppkey] = input.availqty;
    }
  });

  initial_scan->ScanSuppliers([&](const TpchQ20SupplierInput &input) {
    if (input.key < 0)
      return;
    state.suppliers[input.key] = {input.name, input.address, input.nationkey};
  });

  initial_scan->ScanNations([&](const TpchQ20NationInput &input) {
    if (input.key < 0)
      return;
    state.nations[input.key].name = input.name;
    state.nation_name_index[input.name].push_back(input.key);
  });

  state.SortPairEntries();
  // Flat partsupp keys rebuilt in Build() after copy to shared state.
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

  auto find_keys_t0 = std::chrono::steady_clock::now();
  const auto nation_keys = state.FindNationKeysByName(nation_name);
  std::unordered_set<int32_t> target_nationkeys(nation_keys.begin(),
                                                nation_keys.end());
  auto find_keys_t1 = std::chrono::steady_clock::now();
  EmitCustomProfile("find_keys",
                    std::chrono::duration<double, std::micro>(find_keys_t1 - find_keys_t0).count(),
                    {{"nation_keys", static_cast<int64_t>(nation_keys.size())}});

  // Prefix index is only valid for a single trailing '%' with no other
  // wildcards (same rule as PrefixLikeMatcher in the base kernel). Patterns
  // like '%forest%' must go through SqlLike.
  std::string prefix;
  const bool trailing_percent_only =
      !part_name_pattern.empty() && part_name_pattern.back() == '%' &&
      part_name_pattern.find('%') == part_name_pattern.size() - 1 &&
      part_name_pattern.find('_') == std::string::npos;
  if (trailing_percent_only)
    prefix = part_name_pattern.substr(0, part_name_pattern.size() - 1);

  auto part_find_t0 = std::chrono::steady_clock::now();
  std::vector<int64_t> qualifying_partkeys;
  if (!prefix.empty()) {
    qualifying_partkeys = state.FindPartKeysByPrefix(prefix);
  } else {
    std::unordered_set<int64_t> seen_partkeys;
    for (const TpchQ20State *s = &state; s; s = s->parent_state.get()) {
      for (const auto &[partkey, part] : s->parts) {
        if (seen_partkeys.insert(partkey).second &&
            (part_name_pattern.empty() ||
             SqlLike(part.name, part_name_pattern))) {
          qualifying_partkeys.push_back(partkey);
        }
      }
    }
  }
  auto part_find_t1 = std::chrono::steady_clock::now();
  EmitCustomProfile("find_part_keys",
                    std::chrono::duration<double, std::micro>(part_find_t1 - part_find_t0).count(),
                    {{"qualifying_partkeys", static_cast<int64_t>(qualifying_partkeys.size())}});

  // Pre-reserve based on flat entry count scaled down to typical yield.
  const int64_t estimated_suppkeys =
      std::max<int64_t>(static_cast<int64_t>(qualifying_partkeys.size() * 4),
                        1024);
  std::unordered_set<int64_t> qualifying_suppkeys;
  qualifying_suppkeys.reserve(static_cast<std::size_t>(estimated_suppkeys));
  auto eval_t0 = std::chrono::steady_clock::now();
  if (!state.flat_partsupp_keys.empty()) {
    const auto flat_begin = state.flat_partsupp_keys.begin();
    const auto flat_end = state.flat_partsupp_keys.end();
    for (const auto partkey : qualifying_partkeys) {
      // Binary search in the partkey-major flat array. Eliminates FindPart
      // hash lookup + partsupps unordered_map traversal per qualifying part.
      const uint64_t lo_key = MakePairKey(partkey, 0);
      const uint64_t hi_key = MakePairKey(partkey, 0xFFFFFULL);
      auto it = std::lower_bound(flat_begin, flat_end, lo_key,
                                 [](const TpchQ20State::FlatPartsuppEntry &entry, uint64_t key) {
                                   return entry.pair_key < key;
                                 });
      for (; it != flat_end && it->pair_key >= lo_key && it->pair_key <= hi_key;
           ++it) {
        const int64_t availqty = it->availqty;
        const auto *series = it->series;
        if (!series) {
          // Fall back: pointer not cached (e.g. after decode in transient state).
          const auto [fallback_sum, fallback_has] =
              state.QueryWindowSum(it->pair_key, start_date_days, end_date_days);
          if (!fallback_has)
            continue;
          if (AvailQtyGreaterThanHalfSum(availqty, fallback_sum)) {
            const int64_t suppkey = static_cast<int64_t>(it->pair_key & 0xFFFFFULL);
            qualifying_suppkeys.insert(suppkey);
          }
          continue;
        }
        int64_t total = 0;
        bool has_entry = false;
        for (const auto &entry : series->entries) {
          if (entry.shipdate_days < start_date_days)
            continue;
          if (entry.shipdate_days >= end_date_days)
            break;
          has_entry = true;
          total += entry.quantity_raw;
        }
        if (!has_entry)
          continue;
        if (AvailQtyGreaterThanHalfSum(availqty, total)) {
          const int64_t suppkey = static_cast<int64_t>(it->pair_key & 0xFFFFFULL);
          qualifying_suppkeys.insert(suppkey);
        }
      }
    }
  } else {
    // Overlay / delta states: exact fallback using parts maps.
    for (const auto partkey : qualifying_partkeys) {
      const auto *part = state.FindPart(partkey);
      if (!part)
        continue;
      for (const auto &[suppkey, availqty] : part->partsupps) {
        const auto pair_key = MakePairKey(partkey, suppkey);
        const auto [sum_qty, has_entries] =
            state.QueryWindowSum(pair_key, start_date_days, end_date_days);
        if (!has_entries)
          continue;
        if (AvailQtyGreaterThanHalfSum(availqty, sum_qty))
          qualifying_suppkeys.insert(suppkey);
      }
    }
  }
  auto eval_t1 = std::chrono::steady_clock::now();
  EmitCustomProfile("evaluate",
                    std::chrono::duration<double, std::micro>(eval_t1 - eval_t0).count(),
                    {{"qualifying_partkeys", static_cast<int64_t>(qualifying_partkeys.size())},
                     {"qualifying_suppkeys", static_cast<int64_t>(qualifying_suppkeys.size())}});

  // Nation name maps to exactly one key in TPC-H Q20 (`nation_name = ...`).
  // Avoid a hash set containment lookup per candidate: fetch target key once.
  const int32_t target_nationkey =
      target_nationkeys.empty() ? -1 : *target_nationkeys.begin();

  std::vector<TpchQ20OutputRow> output_rows;
  output_rows.reserve(qualifying_suppkeys.size());
  for (const auto suppkey : qualifying_suppkeys) {
    const auto *supplier = state.FindSupplier(suppkey);
    if (!supplier)
      continue;
    if (target_nationkey >= 0 && supplier->nationkey != target_nationkey)
      continue;
    output_rows.push_back({supplier->name, supplier->address});
  }

  auto sort_t0 = std::chrono::steady_clock::now();
  std::sort(output_rows.begin(), output_rows.end(),
            [](const TpchQ20OutputRow &a, const TpchQ20OutputRow &b) {
              return a.name < b.name;
            });
  auto sort_t1 = std::chrono::steady_clock::now();
  EmitCustomProfile("output_sort",
                    std::chrono::duration<double, std::micro>(sort_t1 - sort_t0).count(),
                    {{"rows", static_cast<int64_t>(output_rows.size())}});

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

} // namespace accelerator_codegen::aux_state::query
