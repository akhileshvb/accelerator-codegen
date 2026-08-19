#include "tpch/q9/aux/module.hpp"
#include "accelerator_codegen/stateful/core/byte_stream.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

using core::ByteReader;
using core::ByteWriter;

constexpr uint32_t kVersion = 5;
constexpr uint32_t kCompleteStateMagic = 0x43395141; // Q9AC
constexpr uint32_t kMetadataMagic = 0x4D395141;      // Q9AM
constexpr uint32_t kMaintenanceSideMagic = 0x53395141; // Q9AS
constexpr uint32_t kProfitSlotMagic = 0x50395141;    // Q9SP
constexpr int32_t kEpoch = 719468;
constexpr int32_t kMaxYear = 9999;
constexpr int32_t kMinYear = 1900;
constexpr int32_t kYearBase = 1992;
constexpr int32_t kYearCount = 7;
constexpr int32_t kMaxNationKey = 24;

// ── Cell extraction helpers ─────────────────────────────────────────────────
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

bool DateCell(const core::AuxRow &row, std::size_t index, int32_t &days) {
  int64_t value = 0;
  if (!IntCell(row, index, value) ||
      value < std::numeric_limits<int32_t>::min() ||
      value > std::numeric_limits<int32_t>::max())
    return false;
  days = static_cast<int32_t>(value);
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

// ── Date helpers ────────────────────────────────────────────────────────────
void CivilFromDays(int32_t days, int32_t &year, unsigned &month,
                   unsigned &day) {
  int32_t z = days + kEpoch;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  year = static_cast<int32_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  day = doy - (153 * mp + 2) / 5 + 1;
  month = mp + (mp < 10 ? 3 : -9);
  year += month <= 2;
}

// ── Image helpers ───────────────────────────────────────────────────────────
std::shared_ptr<const TpchQ9StateImage>
MakeStateImage(const std::shared_ptr<const TpchQ9State> &state) {
  auto image = std::make_shared<TpchQ9StateImage>();
  image->kind = TpchQ9ImageKind::CompleteState;
  image->state = state;
  return image;
}

std::shared_ptr<const TpchQ9StateImage>
MakeMetadataImage(const std::shared_ptr<const TpchQ9State> &state) {
  auto image = std::make_shared<TpchQ9StateImage>();
  image->kind = TpchQ9ImageKind::Metadata;
  image->state = state;
  return image;
}

std::shared_ptr<const TpchQ9StateImage>
MakeMaintenanceSideImage(const std::shared_ptr<const TpchQ9State> &state) {
  auto image = std::make_shared<TpchQ9StateImage>();
  image->kind = TpchQ9ImageKind::MaintenanceSide;
  image->state = state;
  return image;
}

std::shared_ptr<const TpchQ9StateImage>
MakeProfitSlotImage(core::PartitionId slot,
                    const std::shared_ptr<const TpchQ9State> &state) {
  auto image = std::make_shared<TpchQ9StateImage>();
  image->kind = TpchQ9ImageKind::ProfitSlot;
  image->profit_slot = slot;
  image->state = state;
  return image;
}

const TpchQ9StateImage *AsQ9Image(const core::StateImage &image) {
  const auto *payload = image.As<TpchQ9StateImage>();
  return payload && payload->state ? payload : nullptr;
}

bool IsCompleteQ9Image(const TpchQ9StateImage *image) {
  return image &&
         (image->kind == TpchQ9ImageKind::CompleteState ||
          image->kind == TpchQ9ImageKind::Metadata);
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

core::InvalidationPlan PartitionSet() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
  return plan;
}

bool CopyMaintenanceSideFromImage(const TpchQ9StateImage *image,
                                  TpchQ9State &state) {
  if (!image || !image->state)
    return false;
  state.supplier_nations = image->state->supplier_nations;
  state.order_years = image->state->order_years;
  state.order_year_overflow = image->state->order_year_overflow;
  state.partsupp_rows = image->state->partsupp_rows;
  state.partsupp_overflow = image->state->partsupp_overflow;
  return true;
}

void CollectSupplierNations(const TpchQ9State &state,
                            std::vector<int64_t> &keys) {
  keys.clear();
  for (std::size_t chunk_idx = 0; chunk_idx < state.supplier_nations.chunks.size();
       ++chunk_idx) {
    const auto &chunk = state.supplier_nations.chunks[chunk_idx];
    const int64_t base =
        static_cast<int64_t>(chunk_idx << core::DenseIdArray<uint8_t>::kShift);
    for (std::size_t slot = 0; slot < chunk.size(); ++slot) {
      if (chunk[slot] == TpchQ9State::kAbsentNationCode)
        continue;
      keys.push_back(base + static_cast<int64_t>(slot));
    }
  }
  std::sort(keys.begin(), keys.end());
}

void CollectOrderYears(const TpchQ9State &state, std::vector<int64_t> &keys) {
  keys.clear();
  for (std::size_t chunk_idx = 0; chunk_idx < state.order_years.chunks.size();
       ++chunk_idx) {
    const auto &chunk = state.order_years.chunks[chunk_idx];
    const int64_t base =
        static_cast<int64_t>(chunk_idx << core::DenseIdArray<uint8_t>::kShift);
    for (std::size_t slot = 0; slot < chunk.size(); ++slot) {
      if (chunk[slot] == TpchQ9State::kAbsentYearCode)
        continue;
      keys.push_back(base + static_cast<int64_t>(slot));
    }
  }
  for (const auto &entry : state.order_year_overflow)
    keys.push_back(entry.first);
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
}

struct PartsuppTriple {
  int64_t partkey = 0;
  int64_t suppkey = 0;
  int64_t cost = 0;
};

void CollectPartsupp(const TpchQ9State &state,
                     std::vector<PartsuppTriple> &rows) {
  rows.clear();
  for (std::size_t chunk_idx = 0; chunk_idx < state.partsupp_rows.chunks.size();
       ++chunk_idx) {
    const auto &chunk = state.partsupp_rows.chunks[chunk_idx];
    const int64_t base = static_cast<int64_t>(
        chunk_idx << core::DenseIdArray<TpchQ9State::PackedPartsuppRow>::kShift);
    for (std::size_t slot = 0; slot < chunk.size(); ++slot) {
      const auto &row = chunk[slot];
      const int64_t partkey = base + static_cast<int64_t>(slot);
      for (int i = 0; i < TpchQ9State::PackedPartsuppRow::kSlots; ++i) {
        if (row.suppkeys[i] == 0)
          continue;
        rows.push_back({partkey, row.suppkeys[i], row.costs[i]});
      }
    }
  }
  for (const auto &entry : state.partsupp_overflow)
    rows.push_back({entry.first.partkey, entry.first.suppkey, entry.second});
  std::sort(rows.begin(), rows.end(),
            [](const PartsuppTriple &a, const PartsuppTriple &b) {
              if (a.partkey != b.partkey)
                return a.partkey < b.partkey;
              return a.suppkey < b.suppkey;
            });
}

bool LoadMaintenanceSide(core::AuxEngineServices &services,
                         const core::VisibilityToken &visibility,
                         TpchQ9State &state) {
  const auto snapshot = services.StateStore().ReadPartitions(
      visibility, {TpchQ9Layout::kMaintenanceSide});
  for (const auto &image : snapshot.partitions) {
    if (image.partition != TpchQ9Layout::kMaintenanceSide)
      continue;
    const auto *typed = AsQ9Image(image);
    if (!typed || typed->kind != TpchQ9ImageKind::MaintenanceSide)
      return false;
    return CopyMaintenanceSideFromImage(typed, state);
  }
  return false;
}

} // namespace
std::string NormalizeForIndex(const std::string &s) {
  std::string result;
  result.reserve(s.size());
  for (char c : s) {
    result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return result;
}

std::vector<std::string> NameTokens(const std::string &normalized) {
  std::vector<std::string> result;
  std::size_t i = 0;
  while (i < normalized.size()) {
    while (i < normalized.size() &&
           std::isspace(static_cast<unsigned char>(normalized[i])))
      ++i;
    if (i >= normalized.size())
      break;
    const std::size_t start = i;
    while (i < normalized.size() &&
           !std::isspace(static_cast<unsigned char>(normalized[i])))
      ++i;
    result.emplace_back(normalized.substr(start, i - start));
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::vector<std::string> Trigrams(const std::string &normalized) {
  std::vector<std::string> result;
  if (normalized.size() < 3) {
    // For short strings (< 3 chars), use the whole string as a single trigram
    if (!normalized.empty())
      result.push_back(normalized);
    return result;
  }
  result.reserve(normalized.size() - 2);
  for (std::size_t i = 0; i + 2 < normalized.size(); ++i)
    result.emplace_back(normalized.substr(i, 3));
  return result;
}

bool ContainsSubstring(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) return true;
  return haystack.find(needle) != std::string_view::npos;
}

namespace {

bool PostingContains(const std::vector<int32_t> &posting, int64_t partkey) {
  if (partkey < std::numeric_limits<int32_t>::min() ||
      partkey > std::numeric_limits<int32_t>::max())
    return false;
  return std::binary_search(posting.begin(), posting.end(),
                            static_cast<int32_t>(partkey));
}

void PostingInsert(std::vector<int32_t> &posting, int64_t partkey) {
  if (PostingContains(posting, partkey)) return;
  posting.push_back(static_cast<int32_t>(partkey));
}

void PostingRemove(std::vector<int32_t> &posting, int64_t partkey) {
  if (partkey < std::numeric_limits<int32_t>::min() ||
      partkey > std::numeric_limits<int32_t>::max())
    return;
  const auto key = static_cast<int32_t>(partkey);
  auto it = std::lower_bound(posting.begin(), posting.end(), key);
  if (it != posting.end() && *it == key)
    posting.erase(it);
}

} // namespace

namespace {

// ── Profit computation ──────────────────────────────────────────────────────
int64_t ComputeProfitRaw(int64_t extendedprice_raw, int64_t discount_raw,
                          int64_t supplycost_raw, int64_t quantity_raw) {
  // profit = extendedprice * (1 - discount) - supplycost * quantity
  // All values are stored with scale 2 (cents)
  // extendedprice_raw is in cents, discount_raw is in hundredths (e.g. 4 = 0.04)
  // supplycost_raw is in cents, quantity_raw is in hundredths

  // revenue = extendedprice_raw * (100 - discount_raw)
  // This gives us value in 1/10000 units (since extendedprice is in 1/100 and
  // (100-discount) is in 1/100, product is in 1/10000)
  const int64_t revenue =
      extendedprice_raw * (static_cast<int64_t>(100) - discount_raw);

  // cost = supplycost_raw * quantity_raw
  // supplycost_raw is in cents (1/100), quantity_raw is in hundredths (1/100)
  // product is in 1/10000
  const int64_t cost = supplycost_raw * quantity_raw;

  return revenue - cost;
}

// Read joined lineitem and compute profit
bool ReadJoinedLineitem(const core::AuxRow &row, int64_t &partkey,
                         int64_t &suppkey, int64_t &orderkey,
                         int64_t &quantity_raw, int64_t &extendedprice_raw,
                         int64_t &discount_raw) {
  int32_t dummy_scale = 0;
  return IntCell(row, 0, partkey) && IntCell(row, 1, suppkey) &&
         IntCell(row, 2, orderkey) &&
         RawDecimalCell(row, 3, quantity_raw, dummy_scale) &&
         RawDecimalCell(row, 4, extendedprice_raw, dummy_scale) &&
         RawDecimalCell(row, 5, discount_raw, dummy_scale);
}

bool ReadPart(const core::AuxRow &row, int64_t &partkey, std::string &name) {
  return IntCell(row, 0, partkey) && StringCell(row, 1, name);
}

bool ReadSupplier(const core::AuxRow &row, int64_t &suppkey,
                  int64_t &nationkey_raw) {
  int64_t nk = 0;
  if (!IntCell(row, 0, suppkey) || !IntCell(row, 1, nk))
    return false;
  if (nk < std::numeric_limits<int32_t>::min() ||
      nk > std::numeric_limits<int32_t>::max())
    return false;
  nationkey_raw = nk;
  return true;
}

bool ReadNation(const core::AuxRow &row, int64_t &nationkey_raw,
                std::string &name) {
  int64_t nk = 0;
  if (!IntCell(row, 0, nk) || !StringCell(row, 1, name))
    return false;
  if (nk < std::numeric_limits<int32_t>::min() ||
      nk > std::numeric_limits<int32_t>::max())
    return false;
  nationkey_raw = nk;
  return true;
}

bool ReadOrder(const core::AuxRow &row, int64_t &orderkey, int32_t &orderdate) {
  return IntCell(row, 0, orderkey) && DateCell(row, 1, orderdate);
}

bool ReadPartsupp(const core::AuxRow &row, int64_t &partkey, int64_t &suppkey,
                  int64_t &supplycost_raw) {
  int64_t raw = 0;
  int32_t scale = 0;
  if (!IntCell(row, 0, partkey) || !IntCell(row, 1, suppkey) ||
      !RawDecimalCell(row, 2, raw, scale))
    return false;
  supplycost_raw = raw;
  return true;
}

// Resolve profit for a single fully-joined lineitem
bool ResolveProfit(const TpchQ9State &state, TpchQ9InitialScan *scan,
                   int64_t partkey, int64_t suppkey, int64_t orderkey,
                   int64_t quantity_raw, int64_t extendedprice_raw,
                   int64_t discount_raw, int64_t &profit_raw,
                   int32_t &nationkey, int32_t &year) {
  if (!state.LookupSupplierNation(suppkey, nationkey) &&
      !(scan && scan->LookupSupplierNation(suppkey, nationkey)))
    return false;
  if (!state.LookupOrderYear(orderkey, year) &&
      !(scan && scan->LookupOrderYear(orderkey, year)))
    return false;
  int64_t supplycost_raw = 0;
  if (!state.LookupPartsuppCost(partkey, suppkey, supplycost_raw) &&
      !(scan && scan->LookupPartsuppCost(partkey, suppkey, supplycost_raw)))
    return false;
  profit_raw = ComputeProfitRaw(extendedprice_raw, discount_raw,
                                 supplycost_raw, quantity_raw);
  return true;
}

} // namespace

// ── Public date/profit helpers ──────────────────────────────────────────────
int32_t TpchQ9ExtractYear(int32_t days_since_epoch) {
  int32_t year = 0;
  unsigned month = 0, day = 0;
  CivilFromDays(days_since_epoch, year, month, day);
  return year;
}

int64_t TpchQ9ComputeProfitRaw(int64_t extendedprice_raw, int64_t discount_raw,
                                int64_t supplycost_raw, int64_t quantity_raw) {
  return ComputeProfitRaw(extendedprice_raw, discount_raw, supplycost_raw,
                           quantity_raw);
}

core::PartitionId TpchQ9State::ChooseProfitSlots(std::size_t part_count) {
  constexpr core::PartitionId kMin = TpchQ9Layout::kMinProfitSlots;
  constexpr core::PartitionId kMax = TpchQ9Layout::kMaxProfitSlots;
  // Keep average parts-per-shard bounded so maintenance stays local at SF20+.
  constexpr std::size_t kTargetPartsPerSlot = 16384;
  if (part_count <= static_cast<std::size_t>(kMin) * kTargetPartsPerSlot)
    return kMin;
  core::PartitionId slots = kMin;
  while (slots < kMax &&
         part_count / static_cast<std::size_t>(slots) > kTargetPartsPerSlot) {
    slots = static_cast<core::PartitionId>(slots * 2);
  }
  return slots;
}

// ── State name posting methods ──────────────────────────────────────────────
void TpchQ9State::FinalizePostings() {
  for (auto &[token, posting] : name_postings) {
    (void)token;
    std::sort(posting.begin(), posting.end());
    posting.erase(std::unique(posting.begin(), posting.end()), posting.end());
  }
}

void TpchQ9State::RebuildNationEmitOrder() {
  nation_emit_order.clear();
  nation_emit_order.reserve(nation_names.size());
  for (const auto &[nationkey, name] : nation_names)
    nation_emit_order.emplace_back(name, nationkey);
  std::sort(nation_emit_order.begin(), nation_emit_order.end());
}

void TpchQ9State::AddNamePostings(int64_t partkey,
                                   const std::string &raw_name) {
  std::string normalized = NormalizeForIndex(raw_name);
  auto &record = PartRecord(partkey);
  record.normalized_name = std::move(normalized);
  for (const auto &token : NameTokens(record.normalized_name))
    PostingInsert(name_postings[token], partkey);
}

void TpchQ9State::RemoveNamePostings(int64_t partkey,
                                      const std::string &raw_name) {
  (void)raw_name;
  const auto *record = FindPart(partkey);
  if (!record) return;
  for (const auto &token : NameTokens(record->normalized_name)) {
    auto post_it = name_postings.find(token);
    if (post_it != name_postings.end()) {
      PostingRemove(post_it->second, partkey);
      if (post_it->second.empty())
        name_postings.erase(post_it);
    }
  }
}

// ── Encode ──────────────────────────────────────────────────────────────────
std::vector<uint8_t> EncodeTpchQ9CompleteState(const TpchQ9State &state) {
  ByteWriter writer;

  // ── Parts (name + profit cells) ─────────────────────────────────────────
  std::vector<int64_t> partkeys;
  for (const auto &partition : state.profit_partitions) {
    for (const auto &entry : partition.parts)
      partkeys.push_back(entry.first);
  }
  std::sort(partkeys.begin(), partkeys.end());
  core::WriteImageHeader(writer, kCompleteStateMagic, kVersion,
                         static_cast<uint32_t>(partkeys.size()));
  for (const auto partkey : partkeys) {
    const auto *record = state.FindPart(partkey);
    if (!record) continue;
    writer.I64(partkey);
    writer.String(record->normalized_name);
    writer.String(std::string{});
    writer.U32(static_cast<uint32_t>(record->cells.size()));
    for (const auto &cell : record->cells) {
      writer.I32(cell.Nation());
      writer.I32(cell.Year());
      writer.I64(cell.profit);
    }
  }

  // ── Name postings ───────────────────────────────────────────────────────
  writer.U32(static_cast<uint32_t>(state.name_postings.size()));
  std::vector<std::string> posting_keys;
  for (const auto &entry : state.name_postings)
    posting_keys.push_back(entry.first);
  std::sort(posting_keys.begin(), posting_keys.end());
  for (const auto &token : posting_keys) {
    writer.String(token);
    const auto &partkeys = state.name_postings.at(token);
    writer.U32(static_cast<uint32_t>(partkeys.size()));
    for (const auto pk : partkeys)
      writer.I64(pk);
  }

  // ── Nation emit order ───────────────────────────────────────────────────
  writer.U32(static_cast<uint32_t>(state.nation_emit_order.size()));
  for (const auto &[name, nationkey] : state.nation_emit_order) {
    writer.String(name);
    writer.I32(nationkey);
  }

  // ── Nation names ────────────────────────────────────────────────────────
  writer.U32(static_cast<uint32_t>(state.nation_names.size()));
  std::vector<int32_t> nation_keys;
  for (const auto &entry : state.nation_names)
    nation_keys.push_back(entry.first);
  std::sort(nation_keys.begin(), nation_keys.end());
  for (const auto key : nation_keys) {
    writer.I32(key);
    writer.String(state.nation_names.at(key));
  }

  // ── Supplier nations ────────────────────────────────────────────────────
  std::vector<int64_t> supp_keys;
  CollectSupplierNations(state, supp_keys);
  writer.U32(static_cast<uint32_t>(supp_keys.size()));
  for (const auto key : supp_keys) {
    int32_t nationkey = 0;
    state.LookupSupplierNation(key, nationkey);
    writer.I64(key);
    writer.I32(nationkey);
  }

  // ── Order years ─────────────────────────────────────────────────────────
  std::vector<int64_t> order_keys;
  CollectOrderYears(state, order_keys);
  writer.U32(static_cast<uint32_t>(order_keys.size()));
  for (const auto key : order_keys) {
    int32_t year = 0;
    state.LookupOrderYear(key, year);
    writer.I64(key);
    writer.I32(year);
  }

  // ── Partsupp costs ──────────────────────────────────────────────────────
  std::vector<PartsuppTriple> ps_rows;
  CollectPartsupp(state, ps_rows);
  writer.U32(static_cast<uint32_t>(ps_rows.size()));
  for (const auto &row : ps_rows) {
    writer.I64(row.partkey);
    writer.I64(row.suppkey);
    writer.I64(row.cost);
  }

  return std::move(writer.bytes);
}

void WriteTpchQ9MetadataBody(ByteWriter &writer, const TpchQ9State &state) {
  writer.U32(static_cast<uint32_t>(state.name_postings.size()));
  std::vector<std::string> posting_keys;
  for (const auto &entry : state.name_postings)
    posting_keys.push_back(entry.first);
  std::sort(posting_keys.begin(), posting_keys.end());
  for (const auto &token : posting_keys) {
    writer.String(token);
    const auto &partkeys = state.name_postings.at(token);
    writer.U32(static_cast<uint32_t>(partkeys.size()));
    for (const auto pk : partkeys)
      writer.I64(pk);
  }

  writer.U32(static_cast<uint32_t>(state.nation_emit_order.size()));
  for (const auto &[name, nationkey] : state.nation_emit_order) {
    writer.String(name);
    writer.I32(nationkey);
  }

  writer.U32(static_cast<uint32_t>(state.nation_names.size()));
  std::vector<int32_t> nation_keys;
  for (const auto &entry : state.nation_names)
    nation_keys.push_back(entry.first);
  std::sort(nation_keys.begin(), nation_keys.end());
  for (const auto key : nation_keys) {
    writer.I32(key);
    writer.String(state.nation_names.at(key));
  }
}

void WriteTpchQ9MaintenanceSideBody(ByteWriter &writer,
                                    const TpchQ9State &state) {
  std::vector<int64_t> supp_keys;
  CollectSupplierNations(state, supp_keys);
  writer.U32(static_cast<uint32_t>(supp_keys.size()));
  for (const auto key : supp_keys) {
    int32_t nationkey = 0;
    state.LookupSupplierNation(key, nationkey);
    writer.I64(key);
    writer.I32(nationkey);
  }

  std::vector<int64_t> order_keys;
  CollectOrderYears(state, order_keys);
  writer.U32(static_cast<uint32_t>(order_keys.size()));
  for (const auto key : order_keys) {
    int32_t year = 0;
    state.LookupOrderYear(key, year);
    writer.I64(key);
    writer.I32(year);
  }

  std::vector<PartsuppTriple> ps_rows;
  CollectPartsupp(state, ps_rows);
  writer.U32(static_cast<uint32_t>(ps_rows.size()));
  for (const auto &row : ps_rows) {
    writer.I64(row.partkey);
    writer.I64(row.suppkey);
    writer.I64(row.cost);
  }
}

void WriteTpchQ9SideMaps(ByteWriter &writer, const TpchQ9State &state) {
  WriteTpchQ9MetadataBody(writer, state);
  WriteTpchQ9MaintenanceSideBody(writer, state);
}

bool ReadTpchQ9MetadataBody(ByteReader &reader, TpchQ9State &state) {
  uint32_t count = 0;
  if (!reader.U32(count))
    return false;
  state.name_postings.clear();
  for (uint32_t i = 0; i < count; ++i) {
    std::string token;
    uint32_t posting_count = 0;
    if (!reader.String(token) || !reader.U32(posting_count))
      return false;
    auto &partkeys = state.name_postings[token];
    partkeys.reserve(posting_count);
    for (uint32_t j = 0; j < posting_count; ++j) {
      int64_t partkey = 0;
      if (!reader.I64(partkey))
        return false;
      partkeys.push_back(static_cast<int32_t>(partkey));
    }
  }

  if (!reader.U32(count))
    return false;
  state.nation_emit_order.clear();
  state.nation_emit_order.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    std::string name;
    int32_t nationkey = 0;
    if (!reader.String(name) || !reader.I32(nationkey))
      return false;
    state.nation_emit_order.emplace_back(std::move(name), nationkey);
  }

  if (!reader.U32(count))
    return false;
  state.nation_names.clear();
  for (uint32_t i = 0; i < count; ++i) {
    int32_t key = 0;
    std::string name;
    if (!reader.I32(key) || !reader.String(name))
      return false;
    state.nation_names.emplace(key, std::move(name));
  }
  return true;
}

bool ReadTpchQ9MaintenanceSideBody(ByteReader &reader, TpchQ9State &state) {
  uint32_t count = 0;
  if (!reader.U32(count))
    return false;
  state.supplier_nations.Clear();
  for (uint32_t i = 0; i < count; ++i) {
    int64_t key = 0;
    int32_t nationkey = 0;
    if (!reader.I64(key) || !reader.I32(nationkey))
      return false;
    state.SetSupplierNation(key, nationkey);
  }

  if (!reader.U32(count))
    return false;
  state.order_years.Clear();
  state.order_year_overflow.clear();
  for (uint32_t i = 0; i < count; ++i) {
    int64_t key = 0;
    int32_t year = 0;
    if (!reader.I64(key) || !reader.I32(year))
      return false;
    state.SetOrderYear(key, year);
  }

  if (!reader.U32(count))
    return false;
  state.partsupp_rows.Clear();
  state.partsupp_overflow.clear();
  for (uint32_t i = 0; i < count; ++i) {
    int64_t partkey = 0, suppkey = 0, cost = 0;
    if (!reader.I64(partkey) || !reader.I64(suppkey) || !reader.I64(cost))
      return false;
    state.SetPartsuppCost(partkey, suppkey, cost);
  }
  return true;
}

bool ReadTpchQ9SideMaps(ByteReader &reader, TpchQ9State &state) {
  if (!ReadTpchQ9MetadataBody(reader, state))
    return false;
  return ReadTpchQ9MaintenanceSideBody(reader, state);
}

void WriteTpchQ9ProfitParts(ByteWriter &writer,
                            const TpchQ9State::PartProfitPartition &partition) {
  std::vector<int64_t> partkeys;
  partkeys.reserve(partition.parts.size());
  for (const auto &entry : partition.parts)
    partkeys.push_back(entry.first);
  std::sort(partkeys.begin(), partkeys.end());
  writer.U32(static_cast<uint32_t>(partkeys.size()));
  for (const auto partkey : partkeys) {
    const auto &record = partition.parts.at(partkey);
    writer.I64(partkey);
    writer.String(record.normalized_name);
    writer.String(std::string{});
    writer.U32(static_cast<uint32_t>(record.cells.size()));
    for (const auto &cell : record.cells) {
      writer.I32(cell.Nation());
      writer.I32(cell.Year());
      writer.I64(cell.profit);
    }
  }
}

bool ReadTpchQ9ProfitParts(ByteReader &reader,
                           TpchQ9State::PartProfitPartition &partition) {
  uint32_t count = 0;
  if (!reader.U32(count))
    return false;
  partition.parts.clear();
  for (uint32_t i = 0; i < count; ++i) {
    int64_t partkey = 0;
    std::string normalized_name, original_name;
    uint32_t cell_count = 0;
    if (!reader.I64(partkey) || !reader.String(normalized_name) ||
        !reader.String(original_name) || !reader.U32(cell_count))
      return false;
    auto &record = partition.parts[partkey];
    record.normalized_name = std::move(normalized_name);
    record.cells.reserve(cell_count);
    for (uint32_t j = 0; j < cell_count; ++j) {
      int32_t nationkey = 0, year = 0;
      int64_t profit = 0;
      if (!reader.I32(nationkey) || !reader.I32(year) || !reader.I64(profit))
        return false;
      record.cells.push_back(Q9ProfitCell::Make(nationkey, year, profit));
    }
  }
  return true;
}

std::vector<uint8_t> EncodeTpchQ9Metadata(const TpchQ9State &state) {
  ByteWriter writer;
  core::WriteImageHeader(writer, kMetadataMagic, kVersion,
                         static_cast<uint32_t>(state.profit_slot_count));
  WriteTpchQ9MetadataBody(writer, state);
  return std::move(writer.bytes);
}

std::vector<uint8_t> EncodeTpchQ9MaintenanceSide(const TpchQ9State &state) {
  ByteWriter writer;
  core::WriteImageHeader(writer, kMaintenanceSideMagic, kVersion, 0);
  WriteTpchQ9MaintenanceSideBody(writer, state);
  return std::move(writer.bytes);
}

std::vector<uint8_t> EncodeTpchQ9ProfitSlot(
    core::PartitionId slot, const TpchQ9State::PartProfitPartition &partition) {
  (void)slot;
  ByteWriter writer;
  core::WriteImageHeader(writer, kProfitSlotMagic, kVersion, 0);
  WriteTpchQ9ProfitParts(writer, partition);
  return std::move(writer.bytes);
}

bool DecodeTpchQ9Metadata(const std::vector<uint8_t> &bytes,
                          TpchQ9State &state) {
  ByteReader reader{bytes};
  uint32_t slot_count = 0;
  if (!core::ReadImageHeader(reader, kMetadataMagic, kVersion, slot_count))
    return false;
  state.profit_partitions.clear();
  state.InitPartitions(slot_count);
  if (!ReadTpchQ9MetadataBody(reader, state))
    return false;
  if (state.nation_emit_order.empty())
    state.RebuildNationEmitOrder();
  return reader.offset == bytes.size();
}

bool DecodeTpchQ9MaintenanceSide(const std::vector<uint8_t> &bytes,
                                 TpchQ9State &state) {
  ByteReader reader{bytes};
  uint32_t reserved = 0;
  if (!core::ReadImageHeader(reader, kMaintenanceSideMagic, kVersion, reserved))
    return false;
  if (!ReadTpchQ9MaintenanceSideBody(reader, state))
    return false;
  return reader.offset == bytes.size();
}

bool DecodeTpchQ9ProfitSlot(const std::vector<uint8_t> &bytes,
                            TpchQ9State::PartProfitPartition &partition) {
  ByteReader reader{bytes};
  uint32_t reserved = 0;
  if (!core::ReadImageHeader(reader, kProfitSlotMagic, kVersion, reserved))
    return false;
  if (!ReadTpchQ9ProfitParts(reader, partition))
    return false;
  return reader.offset == bytes.size();
}

// ── Decode ──────────────────────────────────────────────────────────────────
bool DecodeTpchQ9CompleteState(const std::vector<uint8_t> &bytes,
                                TpchQ9State &state) {
  ByteReader reader{bytes};
  uint32_t count = 0;

  if (!core::ReadImageHeader(reader, kCompleteStateMagic, kVersion, count))
    return false;

  // ── Parts (name + profit cells) ─────────────────────────────────────────
  state.profit_partitions.clear();
  state.InitPartitions(count);
  for (uint32_t i = 0; i < count; ++i) {
    int64_t partkey = 0;
    Q9PartRecord record;
    std::string original_name;
    uint32_t cell_count = 0;
    if (!reader.I64(partkey) || !reader.String(record.normalized_name) ||
        !reader.String(original_name) || !reader.U32(cell_count))
      return false;
    record.cells.reserve(cell_count);
    for (uint32_t j = 0; j < cell_count; ++j) {
      int32_t nationkey = 0, year = 0;
      int64_t profit = 0;
      if (!reader.I32(nationkey) || !reader.I32(year) || !reader.I64(profit))
        return false;
      if (profit != 0)
        record.cells.push_back(Q9ProfitCell::Make(nationkey, year, profit));
    }
    state.GetPartition(partkey).parts.emplace(partkey, std::move(record));
  }

  // ── Name postings ───────────────────────────────────────────────────────
  if (!reader.U32(count)) return false;
  state.name_postings.clear();
  for (uint32_t i = 0; i < count; ++i) {
    std::string trigram;
    uint32_t pk_count = 0;
    if (!reader.String(trigram) || !reader.U32(pk_count))
      return false;
    auto &pk_vec = state.name_postings[trigram];
    pk_vec.reserve(pk_count);
    for (uint32_t j = 0; j < pk_count; ++j) {
      int64_t pk = 0;
      if (!reader.I64(pk)) return false;
      pk_vec.push_back(static_cast<int32_t>(pk));
    }
  }

  // ── Nation emit order ───────────────────────────────────────────────────
  if (!reader.U32(count)) return false;
  state.nation_emit_order.clear();
  state.nation_emit_order.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    std::string name;
    int32_t nationkey = 0;
    if (!reader.String(name) || !reader.I32(nationkey))
      return false;
    state.nation_emit_order.emplace_back(std::move(name), nationkey);
  }

  // ── Nation names ────────────────────────────────────────────────────────
  if (!reader.U32(count)) return false;
  state.nation_names.clear();
  for (uint32_t i = 0; i < count; ++i) {
    int32_t key = 0;
    std::string name;
    if (!reader.I32(key) || !reader.String(name))
      return false;
    state.nation_names.emplace(key, std::move(name));
  }

  // ── Supplier nations ────────────────────────────────────────────────────
  if (!reader.U32(count)) return false;
  state.supplier_nations.Clear();
  for (uint32_t i = 0; i < count; ++i) {
    int64_t key = 0;
    int32_t nk = 0;
    if (!reader.I64(key) || !reader.I32(nk)) return false;
    state.SetSupplierNation(key, nk);
  }

  // ── Order years ─────────────────────────────────────────────────────────
  if (!reader.U32(count)) return false;
  state.order_years.Clear();
  state.order_year_overflow.clear();
  for (uint32_t i = 0; i < count; ++i) {
    int64_t key = 0;
    int32_t year = 0;
    if (!reader.I64(key) || !reader.I32(year)) return false;
    state.SetOrderYear(key, year);
  }

  // ── Partsupp costs ──────────────────────────────────────────────────────
  if (!reader.U32(count)) return false;
  state.partsupp_rows.Clear();
  state.partsupp_overflow.clear();
  for (uint32_t i = 0; i < count; ++i) {
    int64_t partkey = 0, suppkey = 0, cost = 0;
    if (!reader.I64(partkey) || !reader.I64(suppkey) || !reader.I64(cost))
      return false;
    state.SetPartsuppCost(partkey, suppkey, cost);
  }

  if (state.nation_emit_order.empty())
    state.RebuildNationEmitOrder();

  return reader.offset == bytes.size();
}

// ── TxnBuffer ───────────────────────────────────────────────────────────────
bool TpchQ9TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ9TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
  order_deltas.clear();
  part_name_deltas.clear();
  rebuild_partkeys.clear();
}
bool TpchQ9TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ9LineitemDelta> &TpchQ9TxnBuffer::Deltas() const {
  return deltas;
}
const std::vector<TpchQ9OrderDelta> &TpchQ9TxnBuffer::OrderDeltas() const {
  return order_deltas;
}
void TpchQ9TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
  order_deltas.clear();
  part_name_deltas.clear();
}
void TpchQ9TxnBuffer::AddDelta(TpchQ9LineitemDelta delta) {
  pending = true;
  deltas.push_back(delta);
}
void TpchQ9TxnBuffer::AddOrderDelta(TpchQ9OrderDelta delta) {
  pending = true;
  order_deltas.push_back(delta);
}
void TpchQ9TxnBuffer::AddPartNamePosting(int64_t partkey,
                                          const std::string &old_name,
                                          const std::string &new_name) {
  pending = true;
  part_name_deltas.emplace_back(partkey, old_name, new_name);
}
void TpchQ9TxnBuffer::AddRebuildPartkey(int64_t partkey) {
  pending = true;
  rebuild_partkeys.insert(partkey);
}

// ── Module ──────────────────────────────────────────────────────────────────
core::AcceleratorId TpchQ9Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ9Module::Dependencies() const {
  return {{config.part_table, {0, 1}},
          {config.supplier_table, {0, 1}},
          {config.nation_table, {0, 1}},
          {config.orders_table, {0, 1}},
          {config.partsupp_table, {0, 1, 2}},
          {config.lineitem_table, {0, 1, 2, 3, 4, 5}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ9Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ9TxnBuffer>();
}

void TpchQ9Module::BuildStateInto(TpchQ9State &state) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q9 aux requires a typed initial scan source");

  // Scan nations for side state
  initial_scan->ScanNations([&](const TpchQ9NationInput &input) {
    state.nation_names.emplace(input.nationkey, input.name);
  });

  // Scan parts for name index, then size internal profit shards from cardinality.
  std::vector<std::pair<int64_t, std::string>> parts;
  parts.reserve(4096);
  initial_scan->ScanParts([&](const TpchQ9PartInput &input) {
    parts.emplace_back(input.partkey, input.name);
  });
  state.InitPartitions(parts.size());
  for (const auto &[partkey, name] : parts)
    state.AddNamePostings(partkey, name);

  const bool used_sql = initial_scan->ScanProfitContributions(
      [&](const TpchQ9ProfitContribution &input) {
        if (input.profit_raw == 0)
          return;
        state.AddContribution(input.partkey, input.nationkey, input.year,
                              input.profit_raw);
      });

  if (!used_sql) {
    initial_scan->ScanSuppliers([&](const TpchQ9SupplierInput &input) {
      state.SetSupplierNation(input.suppkey, input.nationkey);
    });
    initial_scan->ScanOrders([&](const TpchQ9OrdersInput &input) {
      int32_t year = TpchQ9ExtractYear(input.orderdate_days);
      state.SetOrderYear(input.orderkey, year);
    });
    initial_scan->ScanPartsupps([&](const TpchQ9PartsuppInput &input) {
      state.SetPartsuppCost(input.partkey, input.suppkey, input.supplycost_raw);
    });
    initial_scan->ScanLineitems([&](const TpchQ9LineitemInput &input) {
      int32_t nationkey = 0;
      if (!state.LookupSupplierNation(input.suppkey, nationkey))
        return;
      int32_t year = 0;
      if (!state.LookupOrderYear(input.orderkey, year))
        return;
      int64_t supplycost_raw = 0;
      if (!state.LookupPartsuppCost(input.partkey, input.suppkey, supplycost_raw))
        return;

      const int64_t profit_raw = ComputeProfitRaw(
          input.extendedprice_raw, input.discount_raw, supplycost_raw,
          input.quantity_raw);
      if (profit_raw == 0)
        return;

      state.AddContribution(input.partkey, nationkey, year, profit_raw);
    });
  }

  state.FinalizePostings();
  state.RebuildNationEmitOrder();
}

void TpchQ9Module::Build(core::AuxEngineServices &services,
                         core::StateBuilder &builder) {
  auto state = std::make_shared<TpchQ9State>();
  BuildStateInto(*state);
  const auto typed_state = std::static_pointer_cast<const TpchQ9State>(state);

  // Metadata keeps name index + nation labels; profit cells live in slot images.
  auto metadata_state = std::make_shared<TpchQ9State>();
  metadata_state->nation_names = typed_state->nation_names;
  metadata_state->name_postings = typed_state->name_postings;
  metadata_state->nation_emit_order = typed_state->nation_emit_order;
  metadata_state->profit_slot_count = typed_state->profit_slot_count;
  metadata_state->profit_partitions.resize(typed_state->profit_slot_count);
  builder.Write(core::StateImage::Make(
      TpchQ9Layout::kAccelerator, TpchQ9Layout::kCompleteState, 0,
      MakeMetadataImage(metadata_state)));

  // Do not publish dense order_years / partsupp / supplier maps. Execute
  // never reads them; writes resolve those joins with point SQL so SF20
  // aux state stays O(name postings + profit cells).

  for (core::PartitionId slot = 0; slot < typed_state->profit_slot_count;
       ++slot) {
    if (typed_state->profit_partitions[slot].parts.empty())
      continue;
    auto slot_state = std::make_shared<TpchQ9State>();
    slot_state->profit_slot_count = typed_state->profit_slot_count;
    slot_state->profit_partitions.resize(typed_state->profit_slot_count);
    slot_state->profit_partitions[slot] = typed_state->profit_partitions[slot];
    builder.Write(core::StateImage::Make(
        TpchQ9Layout::kAccelerator, TpchQ9Layout::ProfitSlotPartition(slot), 0,
        MakeProfitSlotImage(slot, slot_state)));
  }

  builder.Finish();
}

core::InvalidationPlan
TpchQ9Module::InterpretMutation(const core::MutationBatch &mutation) const {
  // Q9 watches all six tables; any mutation is potentially relevant
  if (mutation.table == config.part_table ||
      mutation.table == config.supplier_table ||
      mutation.table == config.nation_table ||
      mutation.table == config.orders_table ||
      mutation.table == config.partsupp_table ||
      mutation.table == config.lineitem_table) {
    return PartitionSet();
  }
  return {};
}

void TpchQ9Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ9TxnBuffer &>(base_buffer);

  // Part mutations — name changes affect postings; key changes need rebuild
  if (mutation.table == config.part_table) {
    auto add = [&](const core::AuxRow &row, bool is_old) {
      int64_t partkey = 0;
      std::string name;
      if (!ReadPart(row, partkey, name)) {
        buffer.MarkRebuild();
        return;
      }
      if (is_old)
        buffer.AddPartNamePosting(partkey, name, "");
      else
        buffer.AddPartNamePosting(partkey, "", name);
    };
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows)
        add(row, false);
      break;
    case core::MutationKind::Delete:
      for (const auto &row : mutation.old_rows)
        add(row, true);
      break;
    case core::MutationKind::Update:
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        int64_t old_pk = 0, new_pk = 0;
        std::string old_name, new_name;
        if (!ReadPart(mutation.old_rows[i], old_pk, old_name) ||
            !ReadPart(mutation.new_rows[i], new_pk, new_name)) {
          buffer.MarkRebuild();
          return;
        }
        if (old_pk != new_pk) {
          // Key change: rebuild both old and new partkey profit cells
          buffer.AddRebuildPartkey(old_pk);
          buffer.AddRebuildPartkey(new_pk);
          // Also handle name postings
          buffer.AddPartNamePosting(old_pk, old_name, "");
          buffer.AddPartNamePosting(new_pk, "", new_name);
        } else {
          buffer.AddPartNamePosting(old_pk, old_name, new_name);
        }
      }
      break;
    }
    return;
  }

  // Supplier mutations — s_nationkey changes need rebuild of all profit data
  if (mutation.table == config.supplier_table) {
    buffer.MarkRebuild();
    return;
  }

  // Nation mutations — n_name changes are side-store; n_nationkey changes rebuild
  if (mutation.table == config.nation_table) {
    buffer.MarkRebuild();
    return;
  }

  // Orders mutations — maintain order_years on the maintenance side.
  // RF1 must Apply orders Insert before lineitems in the same txn so Resolve
  // can see the new order year (AccumulateMutation order follows OnMutation).
  if (mutation.table == config.orders_table) {
    auto add = [&](const core::AuxRow &row, int8_t sign) {
      int64_t orderkey = 0;
      int32_t orderdate = 0;
      if (!ReadOrder(row, orderkey, orderdate)) {
        buffer.MarkRebuild();
        return;
      }
      buffer.AddOrderDelta(
          {orderkey, TpchQ9ExtractYear(orderdate), sign});
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

  // Partsupp mutations — supplycost changes need rebuild
  if (mutation.table == config.partsupp_table) {
    buffer.MarkRebuild();
    return;
  }

  // Lineitem mutations — apply delta when fully resolvable
  if (mutation.table != config.lineitem_table)
    return;

  auto add = [&](const core::AuxRow &row, int8_t sign) {
    int64_t partkey = 0, suppkey = 0, orderkey = 0;
    int64_t quantity_raw = 0, extendedprice_raw = 0, discount_raw = 0;
    if (!ReadJoinedLineitem(row, partkey, suppkey, orderkey, quantity_raw,
                             extendedprice_raw, discount_raw)) {
      buffer.MarkRebuild();
      return;
    }
    buffer.AddDelta({partkey, suppkey, orderkey, quantity_raw,
                     extendedprice_raw, discount_raw, sign});
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
}

core::StagedMaintenanceOutcome
TpchQ9Module::StageMaintenance(core::AuxEngineServices &services,
                                const core::InvalidationPlan &,
                                core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ9TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;

  if (!buffer.HasPendingChanges())
    return outcome;

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

  // Try incremental apply for lineitem deltas + part name deltas + order years.
  // RF1: orders Insert must be buffered before lineitems in the same txn so
  // order_years is updated before ResolveProfit.
  uint64_t total_changes =
      static_cast<uint64_t>(buffer.Deltas().size() +
                             buffer.PartNameDeltas().size() +
                             buffer.OrderDeltas().size());
  if (total_changes == 0 && buffer.HasRebuildSet()) {
    total_changes = static_cast<uint64_t>(buffer.RebuildPartkeys().size());
  }

  const auto decision = services.ChooseMaintenance(
      {total_changes, 1, true, false, true, false, false,
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

  // Load metadata and only the profit-slot partitions touched by this batch.
  const auto visibility = services.CurrentVisibility();
  std::shared_ptr<TpchQ9State> new_state;
  core::Version metadata_version = 0;
  const TpchQ9StateImage *metadata_image = nullptr;
  {
    const auto snapshot = services.StateStore().ReadPartitions(
        visibility, {TpchQ9Layout::kCompleteState});
    for (const auto &image : snapshot.partitions) {
      if (image.partition != TpchQ9Layout::kCompleteState)
        continue;
      metadata_image = AsQ9Image(image);
      if (!metadata_image) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
      metadata_version = image.version;
      break;
    }
  }
  if (!metadata_image) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  if (metadata_image->kind == TpchQ9ImageKind::CompleteState) {
    new_state = std::make_shared<TpchQ9State>(*metadata_image->state);
  } else {
    new_state = std::make_shared<TpchQ9State>();
    new_state->profit_slot_count = metadata_image->state->profit_slot_count;
    new_state->InitPartitions(new_state->profit_slot_count);

    const bool touch_names =
        !buffer.PartNameDeltas().empty() || buffer.HasRebuildSet();
    if (touch_names) {
      new_state->nation_names = metadata_image->state->nation_names;
      new_state->name_postings = metadata_image->state->name_postings;
      new_state->nation_emit_order = metadata_image->state->nation_emit_order;
    }

    std::set<core::PartitionId> touched_slots;
    for (const auto &delta : buffer.Deltas()) {
      touched_slots.insert(TpchQ9State::HashPartition(
          delta.partkey, new_state->profit_slot_count));
    }
    for (const auto &[partkey, _, __] : buffer.PartNameDeltas()) {
      (void)_;
      (void)__;
      touched_slots.insert(
          TpchQ9State::HashPartition(partkey, new_state->profit_slot_count));
    }
    for (const auto partkey : buffer.RebuildPartkeys()) {
      touched_slots.insert(
          TpchQ9State::HashPartition(partkey, new_state->profit_slot_count));
    }

    std::vector<core::PartitionId> slot_partitions;
    slot_partitions.reserve(touched_slots.size());
    for (const auto slot : touched_slots)
      slot_partitions.push_back(TpchQ9Layout::ProfitSlotPartition(slot));
    if (!slot_partitions.empty()) {
      const auto slot_snapshot =
          services.StateStore().ReadPartitions(visibility, slot_partitions);
      for (const auto &image : slot_snapshot.partitions) {
        if (image.partition < TpchQ9Layout::kProfitSlotBase)
          continue;
        const auto *typed = AsQ9Image(image);
        if (!typed || typed->kind != TpchQ9ImageKind::ProfitSlot)
          continue;
        const auto slot = image.partition - TpchQ9Layout::kProfitSlotBase;
        if (slot < new_state->profit_slot_count)
          new_state->profit_partitions[slot] =
              typed->state->profit_partitions[slot];
      }
    }
  }

  const bool touch_names =
      !buffer.PartNameDeltas().empty() || buffer.HasRebuildSet();

  // Apply order-year inserts into CompleteState images only. Sharded
  // metadata has no side maps; lineitem resolve uses point SQL, which
  // already sees same-txn RF1 order inserts.
  auto apply_order_inserts = [&](TpchQ9State &side) {
    for (const auto &delta : buffer.OrderDeltas()) {
      if (delta.sign > 0)
        side.SetOrderYear(delta.orderkey, delta.order_year);
    }
  };
  auto apply_order_deletes = [&](TpchQ9State &side) {
    for (const auto &delta : buffer.OrderDeltas()) {
      if (delta.sign < 0)
        side.EraseOrderYear(delta.orderkey);
    }
  };

  if (metadata_image->kind == TpchQ9ImageKind::CompleteState) {
    apply_order_inserts(*new_state);
  }

  // Apply part name deltas
  for (const auto &[partkey, old_name, new_name] : buffer.PartNameDeltas()) {
    if (!old_name.empty())
      new_state->RemoveNamePostings(partkey, old_name);
    if (!new_name.empty())
      new_state->AddNamePostings(partkey, new_name);
    else
      new_state->GetPartition(partkey).parts.erase(partkey);
  }

  // Rebuild specific partkeys if needed (from part key changes)
  if (buffer.HasRebuildSet()) {
    RebuildPartkeys(*new_state, buffer.RebuildPartkeys());
  }

  // Apply lineitem deltas. Join keys come from published maps when present
  // (CompleteState tests) and otherwise from point SQL on the open txn.
  for (const auto &delta : buffer.Deltas()) {
    int64_t profit_raw = 0;
    int32_t nationkey = 0, year = 0;

    if (!ResolveProfit(*new_state, initial_scan, delta.partkey, delta.suppkey,
                       delta.orderkey, delta.quantity_raw,
                       delta.extendedprice_raw, delta.discount_raw, profit_raw,
                       nationkey, year)) {
      outcome.action = core::MaintenanceAction::RebuildAll;
      outcome.invalidations = All();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }

    int64_t signed_profit = static_cast<int64_t>(delta.sign) * profit_raw;
    if (signed_profit == 0) continue;

    if (delta.sign > 0)
      new_state->AddContribution(delta.partkey, nationkey, year, signed_profit);
    else
      new_state->RemoveContribution(delta.partkey, nationkey, year, -signed_profit);
  }

  if (metadata_image->kind == TpchQ9ImageKind::CompleteState) {
    apply_order_deletes(*new_state);
  }

  outcome.action = core::MaintenanceAction::ApplyDelta;
  const auto typed_state =
      std::static_pointer_cast<const TpchQ9State>(std::move(new_state));

  if (touch_names || metadata_image->kind == TpchQ9ImageKind::CompleteState) {
    auto mutable_meta = std::make_shared<TpchQ9State>();
    mutable_meta->nation_names = typed_state->nation_names;
    mutable_meta->name_postings = typed_state->name_postings;
    mutable_meta->nation_emit_order = typed_state->nation_emit_order;
    mutable_meta->profit_slot_count = typed_state->profit_slot_count;
    mutable_meta->FinalizePostings();
    if (mutable_meta->nation_emit_order.empty())
      mutable_meta->RebuildNationEmitOrder();
    mutable_meta->profit_partitions.clear();
    mutable_meta->InitPartitions(typed_state->profit_slot_count);
    outcome.updates.push_back(core::StateImage::Make(
        TpchQ9Layout::kAccelerator, TpchQ9Layout::kCompleteState,
        metadata_version, MakeMetadataImage(mutable_meta)));
  }

  std::set<core::PartitionId> published_slots;
  for (const auto &delta : buffer.Deltas())
    published_slots.insert(TpchQ9State::HashPartition(
        delta.partkey, typed_state->profit_slot_count));
  for (const auto &[partkey, _, __] : buffer.PartNameDeltas()) {
    (void)_;
    (void)__;
    published_slots.insert(
        TpchQ9State::HashPartition(partkey, typed_state->profit_slot_count));
  }
  for (const auto partkey : buffer.RebuildPartkeys())
    published_slots.insert(
        TpchQ9State::HashPartition(partkey, typed_state->profit_slot_count));

  for (const auto slot : published_slots) {
    if (slot >= typed_state->profit_slot_count)
      continue;
    auto slot_state = std::make_shared<TpchQ9State>();
    slot_state->profit_slot_count = typed_state->profit_slot_count;
    slot_state->InitPartitions(typed_state->profit_slot_count);
    slot_state->profit_partitions[slot] = typed_state->profit_partitions[slot];
    outcome.updates.push_back(core::StateImage::Make(
        TpchQ9Layout::kAccelerator, TpchQ9Layout::ProfitSlotPartition(slot), 0,
        MakeProfitSlotImage(slot, slot_state)));
  }

  return outcome;
}

void TpchQ9Module::RebuildPartkeys(
    TpchQ9State &state, const std::unordered_set<int64_t> &partkeys) const {
  if (!initial_scan) return;

  // Clear existing cells for affected partkeys
  for (auto pk : partkeys) {
    auto it = state.GetPartition(pk).parts.find(pk);
    if (it != state.GetPartition(pk).parts.end())
      it->second.cells.clear();
  }

  // Rebuild by scanning lineitems for affected part keys only
  initial_scan->ScanLineitems([&](const TpchQ9LineitemInput &input) {
    if (partkeys.find(input.partkey) == partkeys.end()) return;

    int32_t nationkey = 0, year = 0;
    int64_t profit_raw = 0;
    if (!ResolveProfit(state, initial_scan, input.partkey, input.suppkey,
                       input.orderkey, input.quantity_raw,
                       input.extendedprice_raw, input.discount_raw, profit_raw,
                       nationkey, year))
      return;
    if (profit_raw == 0) return;

    state.AddContribution(input.partkey, nationkey, year, profit_raw);
  });
}

bool TpchQ9Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &arguments) const {
  if (arguments.values.empty())
    return false;
  // Validate only the single complete-state partition (was: empty list → 130-slot scan)
  return services.StateStore()
      .CheckValidity({Id(), {TpchQ9Layout::kCompleteState}},
                     services.CurrentVisibility())
      .IsValid();
}

void TpchQ9Module::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  if (arguments.values.empty() || arguments.values[0].is_null) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  std::string color;
  if (std::holds_alternative<std::string>(arguments.values[0].payload)) {
    color = std::get<std::string>(arguments.values[0].payload);
  } else {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  if (color.empty()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  if (!services.StateStore()
           .CheckValidity({Id(), {TpchQ9Layout::kCompleteState}},
                          services.CurrentVisibility())
           .IsValid()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  std::shared_ptr<const TpchQ9State> state;
  std::vector<core::PartitionId> required;
  if (!LoadState(services, state, required, &color)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  EmitResult(*state, color, services, sink);
}

void TpchQ9Module::ExecuteFallback(core::AuxEngineServices &services,
                                    const core::QueryArguments &arguments,
                                    core::AuxRowSink &sink) {
  // Recompute from base tables: build state and emit
  std::string color;
  if (!arguments.values.empty() && !arguments.values[0].is_null &&
      std::holds_alternative<std::string>(arguments.values[0].payload))
    color = std::get<std::string>(arguments.values[0].payload);

  TpchQ9State state;
  BuildStateInto(state);
  EmitResult(state, color, services, sink);
}

bool TpchQ9Module::LoadState(
    core::AuxEngineServices &services,
    std::shared_ptr<const TpchQ9State> &state,
    std::vector<core::PartitionId> &required_partitions,
    const std::string *color) const {
  required_partitions = {TpchQ9Layout::kCompleteState};

  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), required_partitions);

  const TpchQ9StateImage *metadata_image = nullptr;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ9Layout::kCompleteState) {
      metadata_image = AsQ9Image(image);
      break;
    }
  }
  if (!metadata_image)
    return false;

  if (metadata_image->kind == TpchQ9ImageKind::CompleteState) {
    state = metadata_image->state;
    return true;
  }

  auto assembled = std::make_shared<TpchQ9State>();
  // Execute only needs the name index + nation labels + profit cells. Skip
  // copying maintenance-only side maps (partsupp/order/supplier) on the hit path.
  assembled->nation_names = metadata_image->state->nation_names;
  assembled->nation_emit_order = metadata_image->state->nation_emit_order;
  assembled->profit_slot_count = metadata_image->state->profit_slot_count;
  assembled->InitPartitions(assembled->profit_slot_count);

  std::set<core::PartitionId> slots;
  if (color != nullptr && !color->empty()) {
    const std::string needle = NormalizeForIndex(*color);
    const auto tokens = NameTokens(needle);
    std::string best_token;
    std::size_t best_size = ~std::size_t{0};
    for (const auto &token : tokens) {
      auto post_it = metadata_image->state->name_postings.find(token);
      if (post_it == metadata_image->state->name_postings.end()) {
        state = std::move(assembled);
        return true;
      }
      assembled->name_postings.emplace(token, post_it->second);
      if (post_it->second.size() < best_size) {
        best_size = post_it->second.size();
        best_token = token;
      }
    }
    if (!best_token.empty()) {
      const auto &best_postings = assembled->name_postings.at(best_token);
      std::vector<const std::vector<int32_t> *> other_postings;
      other_postings.reserve(tokens.size());
      for (const auto &token : tokens) {
        const auto &posting = assembled->name_postings.at(token);
        if (&posting != &best_postings)
          other_postings.push_back(&posting);
      }
      // Intersect tokens before slot fanout so "green" does not pull every
      // profit shard that merely shares one token.
      for (const auto partkey : best_postings) {
        bool in_all = true;
        for (const auto *posting : other_postings) {
          if (!PostingContains(*posting, partkey)) {
            in_all = false;
            break;
          }
        }
        if (!in_all)
          continue;
        slots.insert(TpchQ9State::HashPartition(partkey,
                                              assembled->profit_slot_count));
      }
    }
  } else {
    assembled->name_postings = metadata_image->state->name_postings;
    for (core::PartitionId slot = 0; slot < assembled->profit_slot_count;
         ++slot)
      slots.insert(slot);
  }

  required_partitions.clear();
  required_partitions.push_back(TpchQ9Layout::kCompleteState);
  std::vector<core::PartitionId> slot_partitions;
  slot_partitions.reserve(slots.size());
  for (const auto slot : slots) {
    const auto partition = TpchQ9Layout::ProfitSlotPartition(slot);
    slot_partitions.push_back(partition);
    required_partitions.push_back(partition);
  }

  if (!slot_partitions.empty()) {
    const auto slot_snapshot = services.StateStore().ReadPartitions(
        services.CurrentVisibility(), slot_partitions);
    for (const auto &image : slot_snapshot.partitions) {
      if (image.partition < TpchQ9Layout::kProfitSlotBase)
        continue;
      const auto *typed = AsQ9Image(image);
      if (!typed || typed->kind != TpchQ9ImageKind::ProfitSlot)
        continue;
      const auto slot = image.partition - TpchQ9Layout::kProfitSlotBase;
      if (slot < assembled->profit_slot_count)
        assembled->profit_partitions[slot] =
            typed->state->profit_partitions[slot];
    }
  }

  state = std::move(assembled);
  return true;
}

void TpchQ9Module::EmitResult(const TpchQ9State &state,
                                const std::string &color,
                                core::AuxEngineServices &,
                                core::AuxRowSink &sink) const {
  const std::string needle = NormalizeForIndex(color);
  const auto tokens = NameTokens(needle);

  if (tokens.empty()) {
    sink.Begin(core::OutputSchema{});
    sink.End();
    return;
  }

  const std::vector<int32_t> *best_postings = nullptr;
  std::size_t best_size = ~std::size_t{0};
  std::vector<const std::vector<int32_t> *> other_postings;
  other_postings.reserve(tokens.size());

  for (const auto &token : tokens) {
    auto post_it = state.name_postings.find(token);
    if (post_it == state.name_postings.end()) {
      sink.Begin(core::OutputSchema{});
      sink.End();
      return;
    }
    const auto &posting = post_it->second;
    if (posting.size() < best_size) {
      best_size = posting.size();
      best_postings = &posting;
    }
  }
  if (!best_postings) {
    sink.Begin(core::OutputSchema{});
    sink.End();
    return;
  }

  for (const auto &token : tokens) {
    const auto &posting = state.name_postings.at(token);
    if (&posting != best_postings)
      other_postings.push_back(&posting);
  }

  constexpr int32_t kDenseNations = kMaxNationKey + 1;
  constexpr int32_t kDenseYears = kYearCount;
  int64_t dense_profit[kDenseNations][kDenseYears] = {};
  bool dense_valid[kDenseNations][kDenseYears] = {};
  bool nation_seen[kDenseNations] = {};
  std::unordered_map<Q9CellKey, int64_t, Q9CellKeyHash> sparse;

  const std::string_view needle_view(needle);
  for (const auto pk : *best_postings) {
    bool in_all = true;
    for (const auto *posting : other_postings) {
      if (!PostingContains(*posting, pk)) {
        in_all = false;
        break;
      }
    }
    if (!in_all) continue;

    const auto *record = state.FindPart(pk);
    if (!record) continue;
    if (!ContainsSubstring(record->normalized_name, needle_view)) continue;

    for (const auto &cell : record->cells) {
      const int32_t year_idx = cell.Year() - kYearBase;
      if (cell.Nation() >= 0 && cell.Nation() < kDenseNations &&
          year_idx >= 0 && year_idx < kDenseYears) {
        dense_profit[cell.Nation()][year_idx] += cell.profit;
        dense_valid[cell.Nation()][year_idx] = true;
        nation_seen[cell.Nation()] = true;
      } else {
        Q9CellKey key{cell.Nation(), cell.Year()};
        sparse[key] += cell.profit;
      }
    }
  }

  std::vector<std::pair<std::string, int32_t>> fallback_order;
  if (state.nation_emit_order.empty()) {
    fallback_order.reserve(state.nation_names.size());
    for (const auto &[nk, name] : state.nation_names)
      fallback_order.emplace_back(name, nk);
    std::sort(fallback_order.begin(), fallback_order.end());
  }
  const auto &nations = state.nation_emit_order.empty() ? fallback_order
                                                        : state.nation_emit_order;

  bool has_rows = !sparse.empty();
  if (!has_rows) {
    for (int32_t nk = 0; nk < kDenseNations && !has_rows; ++nk) {
      if (!nation_seen[nk]) continue;
      for (int32_t yi = 0; yi < kDenseYears; ++yi) {
        if (dense_valid[nk][yi] && dense_profit[nk][yi] != 0) {
          has_rows = true;
          break;
        }
      }
    }
  }

  if (!has_rows) {
    sink.Begin(core::OutputSchema{});
    sink.End();
    return;
  }

  sink.Begin(core::OutputSchema{});
  for (const auto &[nation_name, nk] : nations) {
    if (nk < 0 || nk >= kDenseNations || !nation_seen[nk]) continue;
    for (int32_t yi = kDenseYears - 1; yi >= 0; --yi) {
      if (!dense_valid[nk][yi]) continue;
      const int64_t profit = dense_profit[nk][yi];
      if (profit == 0) continue;
      core::AuxRow out;
      out.values.reserve(3);
      out.values.push_back(
          core::AuxValue::Of(core::AuxType::String, nation_name));
      out.values.push_back(core::AuxValue::Of(
          core::AuxType::Int64, static_cast<int64_t>(kYearBase + yi)));
      out.values.push_back(core::AuxValue::Of(
          core::AuxType::ScaledDecimal, core::ScaledDecimal{profit, 4}));
      sink.WriteRow(out);
    }
  }

  if (!sparse.empty()) {
    const std::string empty_str;
    std::vector<std::pair<Q9CellKey, int64_t>> sparse_entries;
    sparse_entries.reserve(sparse.size());
    for (const auto &[key, profit] : sparse) {
      if (profit != 0)
        sparse_entries.emplace_back(key, profit);
    }
    std::sort(sparse_entries.begin(), sparse_entries.end(),
              [&state, &empty_str](const auto &a, const auto &b) {
                auto na = state.nation_names.find(a.first.nationkey);
                auto nb = state.nation_names.find(b.first.nationkey);
                const std::string &name_a = (na != state.nation_names.end())
                                                ? na->second
                                                : empty_str;
                const std::string &name_b = (nb != state.nation_names.end())
                                                ? nb->second
                                                : empty_str;
                if (name_a != name_b) return name_a < name_b;
                return a.first.year > b.first.year;
              });
    for (const auto &[key, profit] : sparse_entries) {
      auto nation_it = state.nation_names.find(key.nationkey);
      if (nation_it == state.nation_names.end()) continue;
      core::AuxRow out;
      out.values.reserve(3);
      out.values.push_back(
          core::AuxValue::Of(core::AuxType::String, nation_it->second));
      out.values.push_back(core::AuxValue::Of(
          core::AuxType::Int64, static_cast<int64_t>(key.year)));
      out.values.push_back(core::AuxValue::Of(
          core::AuxType::ScaledDecimal, core::ScaledDecimal{profit, 4}));
      sink.WriteRow(out);
    }
  }
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query