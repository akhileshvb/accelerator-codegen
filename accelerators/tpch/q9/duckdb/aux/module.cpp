#include "tpch/q9/duckdb/aux/module.hpp"
#include "tpch/q9/calibration_writes.hpp"
#include "substrate/aux/core/byte_stream.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"
#include "tpch/common/typed_row_batch.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

using core::ByteReader;
using core::ByteWriter;

// Profit slots per build slice: a slot holds ~1000 parts (~0.1 ms each), so 16 pay for a task.
constexpr std::size_t kCellsGrain = 16;
// Candidate parts per read slice: a hash lookup, substring test and cells, ~0.1 us each.
constexpr std::size_t kCandidateGrain = 8192;
// Profit slots per read slice: ~1000 name substring tests each, ~0.1 ms per slot.
constexpr std::size_t kSlotGrain = 16;
// Build: pair aggregates buffered before they are applied, and slots per slice when applying.
constexpr std::size_t kPairSlotGrain = 4;

// One (part, supplier, order year) lineitem aggregate: rev = SUM(extendedprice_raw
// * (100 - discount_raw)), qty = SUM(quantity_raw).
struct PairAgg {
  int64_t partkey, suppkey;
  int32_t year;
  int64_t rev, qty;
};
// Build: profit slots per slice when summing the token cube.
constexpr std::size_t kCubeSlotGrain = 8;
// Maintenance: slots whose profit-partition pointer vector is copied per slice (~10 us each).
constexpr std::size_t kSlotCopyGrain = 8;
// Maintenance: slots whose lineitem deltas are applied per slice (~0.1 ms each at RF size).
constexpr std::size_t kSlotApplyGrain = 16;
// Maintenance: slots whose net cell deltas are applied per slice.
constexpr std::size_t kNetSlotGrain = 8;
constexpr uint32_t kVersion = 7;
constexpr uint32_t kCompleteStateMagic = 0x43395141; // Q9AC
constexpr uint32_t kMetadataMagic = 0x4D395141;      // Q9AM
constexpr uint32_t kMaintenanceSideMagic = 0x53395141; // Q9AS
constexpr uint32_t kProfitSlotMagic = 0x50395141;    // Q9SP
constexpr uint32_t kCubeMagic = 0x43555141;          // Q9UC
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

std::shared_ptr<const TpchQ9StateImage>
MakeCubeImage(const std::shared_ptr<const TpchQ9State> &state) {
  auto image = std::make_shared<TpchQ9StateImage>();
  image->kind = TpchQ9ImageKind::Cube;
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
  for (std::size_t chunk_idx = 0; chunk_idx < state.supplier_nations.NumChunks();
       ++chunk_idx) {
    const auto *page = state.supplier_nations.ChunkAt(chunk_idx);
    if (!page)
      continue;
    const auto &chunk = *page;
    const int64_t base = state.supplier_nations.ChunkBase(chunk_idx);
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
  for (std::size_t chunk_idx = 0; chunk_idx < state.order_years.NumChunks();
       ++chunk_idx) {
    const auto *page = state.order_years.ChunkAt(chunk_idx);
    if (!page)
      continue;
    const auto &chunk = *page;
    const int64_t base = state.order_years.ChunkBase(chunk_idx);
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
  for (std::size_t chunk_idx = 0; chunk_idx < state.partsupp_rows.NumChunks();
       ++chunk_idx) {
    const auto *page = state.partsupp_rows.ChunkAt(chunk_idx);
    if (!page)
      continue;
    const auto &chunk = *page;
    const int64_t base = state.partsupp_rows.ChunkBase(chunk_idx);
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

// Sorted insert (postings stay sorted outside bulk-build mode).
void PostingInsertSorted(std::vector<int32_t> &posting, int64_t partkey) {
  if (partkey < std::numeric_limits<int32_t>::min() ||
      partkey > std::numeric_limits<int32_t>::max())
    return;
  const auto key = static_cast<int32_t>(partkey);
  auto it = std::lower_bound(posting.begin(), posting.end(), key);
  if (it != posting.end() && *it == key)
    return;
  posting.insert(it, key);
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

// Order year for a lineitem delta: this batch's new orders first, then the
// pre-batch order map, then (last resort) a point lookup.
bool ResolveOrderYear(const TpchQ9State &side, TpchQ9InitialScan *scan,
                      int64_t orderkey, int32_t &year,
                      const std::unordered_map<int64_t, int32_t> &overlay) {
  const auto it = overlay.find(orderkey);
  if (it != overlay.end()) {
    year = it->second;
    return true;
  }
  if (side.LookupOrderYear(orderkey, year))
    return true;
  return scan && scan->LookupOrderYear(orderkey, year);
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
  constexpr std::size_t kTargetPartsPerSlot = 1024;
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
std::vector<std::string> TpchQ9State::NameTokensOf(const std::string &normalized) {
  return NameTokens(normalized);
}

void TpchQ9State::CubeAdd(const std::vector<std::string> &tokens,
                          int32_t nation, int32_t year, int64_t delta) {
  if (delta == 0)
    return;
  for (const auto &token : tokens) {
    auto &slot = cube[token];
    if (!slot)
      slot = std::make_shared<Q9TokenCube>();
    else if (slot.use_count() > 1)
      slot = std::make_shared<Q9TokenCube>(*slot);
    const int32_t yi = year - Q9TokenCube::kYearBase;
    if (nation >= 0 && nation < Q9TokenCube::kNations && yi >= 0 &&
        yi < Q9TokenCube::kYears) {
      slot->dense[nation][yi] += delta;
      continue;
    }
    auto &sp = slot->sparse;
    bool found = false;
    for (auto it = sp.begin(); it != sp.end(); ++it) {
      if (it->Nation() != nation || it->Year() != year)
        continue;
      it->profit += delta;
      if (it->profit == 0)
        sp.erase(it);
      found = true;
      break;
    }
    if (!found)
      sp.push_back(Q9ProfitCell::Make(nation, year, delta));
  }
}

void TpchQ9State::CubeApplyRecord(const Q9PartRecord &record, int sign) {
  if (record.cells.empty())
    return;
  const auto tokens = NameTokens(record.normalized_name);
  if (tokens.empty())
    return;
  for (const auto &cell : record.cells)
    CubeAdd(tokens, cell.Nation(), cell.Year(), sign * cell.profit);
}

void TpchQ9State::BuildCube(const core::AuxEngineServices &services) {
  cube.clear();
  cube_active = true;
  // Slices fill their own cube (states used as cube holders); the cubes are
  // added into this one in slice order.
  core::ParallelReduce(
      services, profit_partitions.size(), kCubeSlotGrain, *this,
      [&](TpchQ9State &into, std::size_t begin, std::size_t end) {
        for (std::size_t p = begin; p < end; ++p)
          for (const auto &entry : profit_partitions[p].parts)
            into.CubeApplyRecord(*entry.second, +1);
      },
      [](TpchQ9State &into, TpchQ9State &&from) {
        for (const auto &[token, part] : from.cube) {
          const std::vector<std::string> one{token};
          for (int32_t n = 0; n < Q9TokenCube::kNations; ++n)
            for (int32_t y = 0; y < Q9TokenCube::kYears; ++y)
              into.CubeAdd(one, n, Q9TokenCube::kYearBase + y, part->dense[n][y]);
          for (const auto &cell : part->sparse)
            into.CubeAdd(one, cell.Nation(), cell.Year(), cell.profit);
        }
      });
}

void TpchQ9State::FinalizePostings() {
  for (auto &[token, posting] : name_postings) {
    (void)token;
    std::sort(posting->begin(), posting->end());
    posting->erase(std::unique(posting->begin(), posting->end()),
                   posting->end());
  }
  postings_bulk = false;
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
  record.exists = true;
  for (const auto &token : NameTokens(record.normalized_name)) {
    auto &slot = name_postings[token];
    if (!slot)
      slot = std::make_shared<Posting>();
    else if (slot.use_count() > 1)
      slot = std::make_shared<Posting>(*slot); // COW: shared with an image
    if (postings_bulk) {
      if (partkey >= std::numeric_limits<int32_t>::min() &&
          partkey <= std::numeric_limits<int32_t>::max())
        slot->push_back(static_cast<int32_t>(partkey));
    } else {
      PostingInsertSorted(*slot, partkey);
    }
  }
}

void TpchQ9State::RemoveNamePostings(int64_t partkey,
                                      const std::string &raw_name) {
  (void)raw_name;
  const auto *record = FindPart(partkey);
  if (!record) return;
  for (const auto &token : NameTokens(record->normalized_name)) {
    auto post_it = name_postings.find(token);
    if (post_it == name_postings.end())
      continue;
    if (post_it->second.use_count() > 1)
      post_it->second = std::make_shared<Posting>(*post_it->second);
    PostingRemove(*post_it->second, partkey);
    if (post_it->second->empty())
      name_postings.erase(post_it);
  }
}

// ── Pair aggregates ─────────────────────────────────────────────────────────
bool TpchQ9State::AddPairAgg(Q9PartRecord &rec, int32_t suppkey, int32_t year,
                             int64_t rev, int64_t qty) {
  const int32_t yi = year - Q9Pair::kYearBase;
  if (yi >= 0 && yi < Q9Pair::kYears) {
    bool created = false;
    auto *pair = FindPair(rec, suppkey);
    if (!pair) {
      rec.pairs.emplace_back();
      pair = &rec.pairs.back();
      pair->suppkey = suppkey;
      created = true;
    }
    pair->rev[yi] += rev;
    pair->qty[yi] += qty;
    return created;
  }
  // The pair must exist (possibly all-zero) so supplier/part changes find it.
  bool created = false;
  if (!FindPair(rec, suppkey)) {
    rec.pairs.emplace_back();
    rec.pairs.back().suppkey = suppkey;
    created = true;
  }
  for (auto &e : rec.extras) {
    if (e.suppkey == suppkey && e.year == year) {
      e.rev += rev;
      e.qty += qty;
      return created;
    }
  }
  rec.extras.push_back({suppkey, year, rev, qty});
  return created;
}

void TpchQ9State::AdjustCell(Q9PartRecord &rec,
                             const std::vector<std::string> &tokens,
                             int32_t nation, int32_t year, int64_t delta) {
  if (delta == 0)
    return;
  const auto probe = Q9ProfitCell::Make(nation, year, 0);
  const int32_t n = probe.Nation();
  const int32_t y = probe.Year();
  auto &cells = rec.cells;
  bool found = false;
  for (auto it = cells.begin(); it != cells.end(); ++it) {
    if (it->Nation() != n || it->Year() != y)
      continue;
    it->profit += delta;
    if (it->profit == 0)
      cells.erase(it);
    found = true;
    break;
  }
  if (!found)
    cells.push_back(Q9ProfitCell::Make(nation, year, delta));
  if (cube_active)
    CubeAdd(tokens, n, y, delta);
}

void TpchQ9State::ApplyPairContribution(int64_t partkey, Q9PartRecord &rec,
                                        const std::vector<std::string> &tokens,
                                        int32_t suppkey, int sign,
                                        const TpchQ9State &side) {
  if (!rec.exists)
    return;
  const auto *pair = FindPair(rec, suppkey);
  if (!pair)
    return;
  int32_t nation = 0;
  if (!side.LookupSupplierNation(suppkey, nation))
    return;
  int64_t cost = 0;
  if (!side.LookupPartsuppCost(partkey, suppkey, cost))
    return;
  for (int32_t yi = 0; yi < Q9Pair::kYears; ++yi) {
    if (pair->rev[yi] == 0 && pair->qty[yi] == 0)
      continue;
    AdjustCell(rec, tokens, nation, Q9Pair::kYearBase + yi,
               sign * (pair->rev[yi] - cost * pair->qty[yi]));
  }
  // `pair` may dangle only if rec.pairs changed; AdjustCell never touches it.
  for (const auto &e : rec.extras) {
    if (e.suppkey != suppkey)
      continue;
    AdjustCell(rec, tokens, nation, e.year, sign * (e.rev - cost * e.qty));
  }
}

bool TpchQ9State::PairSnapshot(int64_t partkey, const Q9PartRecord &rec,
                               int32_t suppkey, int sign,
                               const TpchQ9State &side, PairSnap &snap,
                               std::vector<CellDelta> &extras_out) {
  if (!rec.exists)
    return false;
  const auto *pair = FindPair(rec, suppkey);
  if (!pair)
    return false;
  int32_t nation = 0;
  if (!side.LookupSupplierNation(suppkey, nation))
    return false;
  int64_t cost = 0;
  if (!side.LookupPartsuppCost(partkey, suppkey, cost))
    return false;
  // Cells key nations through Q9ProfitCell::Make (negative nations fold to 0).
  snap.nation = Q9ProfitCell::Make(nation, 0, 0).Nation();
  for (int32_t yi = 0; yi < Q9Pair::kYears; ++yi)
    snap.v[yi] = pair->rev[yi] - cost * pair->qty[yi];
  for (const auto &e : rec.extras) {
    if (e.suppkey != suppkey)
      continue;
    const int64_t d = sign * (e.rev - cost * e.qty);
    if (d != 0)
      extras_out.push_back({partkey, snap.nation, e.year, d});
  }
  return true;
}

void TpchQ9State::ApplyNetCellDeltas(
    const core::AuxEngineServices &services,
    std::vector<std::vector<CellDelta>> &chunks) {
  // Bucket by slot: slots are independent, so each is netted and applied by
  // one task (cells of its parts) while the cube deltas are summed per slice.
  std::vector<std::vector<CellDelta>> by_slot(profit_slot_count);
  for (auto &chunk : chunks) {
    for (const auto &d : chunk)
      by_slot[HashPartition(d.partkey, profit_slot_count)].push_back(d);
    std::vector<CellDelta>().swap(chunk);
  }
  std::vector<core::PartitionId> active;
  for (core::PartitionId slot = 0; slot < profit_slot_count; ++slot)
    if (!by_slot[slot].empty())
      active.push_back(slot);

  // Cube deltas aggregated per token before touching the (COW) cube. Token
  // views point into part names, which outlive this call (the records are held
  // by the slot maps and are never renamed here).
  constexpr int kDense = Q9TokenCube::kNations * Q9TokenCube::kYears;
  struct TokenAcc {
    std::unordered_map<std::string_view, std::size_t> index;
    std::vector<std::string_view> names;
    std::vector<std::array<int64_t, kDense>> acc;
    std::vector<CellDelta> sparse; // outside the dense range (token in .partkey)
    std::vector<std::string_view> sparse_tokens;
  };
  const auto merge_tokens = [](TokenAcc &into, TokenAcc &&from) {
    for (std::size_t t = 0; t < from.names.size(); ++t) {
      auto it = into.index.find(from.names[t]);
      if (it == into.index.end()) {
        it = into.index.emplace(from.names[t], into.acc.size()).first;
        into.names.push_back(from.names[t]);
        into.acc.emplace_back();
        into.acc.back().fill(0);
      }
      for (int c = 0; c < kDense; ++c)
        into.acc[it->second][c] += from.acc[t][c];
    }
    into.sparse.insert(into.sparse.end(), from.sparse.begin(), from.sparse.end());
    into.sparse_tokens.insert(into.sparse_tokens.end(),
                              from.sparse_tokens.begin(),
                              from.sparse_tokens.end());
  };

  const auto apply_slot = [&](core::PartitionId slot, TokenAcc &ta) {
    auto &deltas = by_slot[slot];
    std::sort(deltas.begin(), deltas.end(),
              [](const CellDelta &a, const CellDelta &b) {
                if (a.partkey != b.partkey)
                  return a.partkey < b.partkey;
                if (a.nation != b.nation)
                  return a.nation < b.nation;
                return a.year < b.year;
              });
    std::vector<CellDelta> net;
    std::vector<std::string_view> views;
    std::size_t i = 0;
    while (i < deltas.size()) {
      const int64_t partkey = deltas[i].partkey;
      std::size_t group_end = i;
      while (group_end < deltas.size() && deltas[group_end].partkey == partkey)
        ++group_end;
      net.clear();
      for (std::size_t j = i; j < group_end;) {
        CellDelta cur = deltas[j];
        std::size_t k = j + 1;
        while (k < group_end && deltas[k].nation == cur.nation &&
               deltas[k].year == cur.year)
          cur.delta += deltas[k++].delta;
        if (cur.delta != 0)
          net.push_back(cur);
        j = k;
      }
      i = group_end;
      if (net.empty())
        continue;
      auto *rec = GetPartition(partkey).parts.Mutable(partkey);
      if (!rec)
        continue;
      for (const auto &c : net) {
        auto &cells = rec->cells;
        bool found = false;
        for (auto it = cells.begin(); it != cells.end(); ++it) {
          if (it->Nation() != c.nation || it->Year() != c.year)
            continue;
          it->profit += c.delta;
          if (it->profit == 0)
            cells.erase(it);
          found = true;
          break;
        }
        if (!found)
          cells.push_back(Q9ProfitCell::Make(c.nation, c.year, c.delta));
      }
      if (!cube_active)
        continue;
      // Distinct whitespace-separated tokens of the name (as NameTokens).
      views.clear();
      const std::string &name = rec->normalized_name;
      std::size_t p = 0;
      while (p < name.size()) {
        while (p < name.size() &&
               std::isspace(static_cast<unsigned char>(name[p])))
          ++p;
        if (p >= name.size())
          break;
        const std::size_t start = p;
        while (p < name.size() &&
               !std::isspace(static_cast<unsigned char>(name[p])))
          ++p;
        const std::string_view v(name.data() + start, p - start);
        if (std::find(views.begin(), views.end(), v) == views.end())
          views.push_back(v);
      }
      for (const auto v : views) {
        auto it = ta.index.find(v);
        if (it == ta.index.end()) {
          it = ta.index.emplace(v, ta.acc.size()).first;
          ta.names.push_back(v);
          ta.acc.emplace_back();
          ta.acc.back().fill(0);
        }
        auto &acc = ta.acc[it->second];
        for (const auto &c : net) {
          const int32_t yi = c.year - Q9TokenCube::kYearBase;
          if (c.nation >= 0 && c.nation < Q9TokenCube::kNations && yi >= 0 &&
              yi < Q9TokenCube::kYears) {
            acc[c.nation * Q9TokenCube::kYears + yi] += c.delta;
          } else {
            ta.sparse.push_back(c);
            ta.sparse_tokens.push_back(v);
          }
        }
      }
    }
  };
  TokenAcc ta;
  core::ParallelReduce(
      services, active.size(), kNetSlotGrain, ta,
      [&](TokenAcc &acc, std::size_t begin, std::size_t end) {
        for (std::size_t t = begin; t < end; ++t)
          apply_slot(active[t], acc);
      },
      merge_tokens);

  for (std::size_t t = 0; t < ta.names.size(); ++t) {
    const std::vector<std::string> one{std::string(ta.names[t])};
    const auto &acc = ta.acc[t];
    for (int n = 0; n < Q9TokenCube::kNations; ++n)
      for (int y = 0; y < Q9TokenCube::kYears; ++y)
        if (acc[n * Q9TokenCube::kYears + y] != 0)
          CubeAdd(one, n, Q9TokenCube::kYearBase + y,
                  acc[n * Q9TokenCube::kYears + y]);
  }
  for (std::size_t k = 0; k < ta.sparse.size(); ++k)
    CubeAdd({std::string(ta.sparse_tokens[k])}, ta.sparse[k].nation,
            ta.sparse[k].year, ta.sparse[k].delta);
}

void TpchQ9State::BuildCellsFromPairs(const core::AuxEngineServices &services) {
  static const std::vector<std::string> kNoTokens;
  // Each record's cells are written only by its own slot; the side maps are read-only.
  core::ParallelSlices(
      services, profit_partitions.size(), kCellsGrain,
      [&](std::size_t, std::size_t begin, std::size_t end) {
        for (std::size_t p = begin; p < end; ++p) {
          for (const auto &entry : profit_partitions[p].parts) {
            auto &rec = *entry.second; // build state: records are unshared
            if (!rec.exists || rec.pairs.empty())
              continue;
            for (const auto &pair : rec.pairs)
              ApplyPairContribution(entry.first, rec, kNoTokens, pair.suppkey,
                                    +1, *this);
          }
        }
      });
}

void TpchQ9State::BuildSuppPairsIndex() {
  std::unordered_map<int32_t, std::vector<int64_t>> by_supp;
  for (const auto &partition : profit_partitions) {
    for (const auto &entry : partition.parts) {
      for (const auto &pair : entry.second->pairs)
        by_supp[pair.suppkey].push_back(entry.first);
    }
  }
  supp_pairs.Clear();
  for (auto &[suppkey, parts] : by_supp) {
    std::sort(parts.begin(), parts.end());
    Q9BoundedSet(supp_pairs, suppkey,
                 std::shared_ptr<const std::vector<int64_t>>(
                     std::make_shared<std::vector<int64_t>>(std::move(parts))));
  }
}

// ── Record codec ──
namespace {
void WriteQ9Record(ByteWriter &writer, int64_t partkey,
                   const Q9PartRecord &record) {
  writer.I64(partkey);
  writer.String(record.normalized_name);
  writer.U32(record.exists ? 1u : 0u);
  writer.U32(static_cast<uint32_t>(record.cells.size()));
  for (const auto &cell : record.cells) {
    writer.I32(cell.Nation());
    writer.I32(cell.Year());
    writer.I64(cell.profit);
  }
  writer.U32(static_cast<uint32_t>(record.pairs.size()));
  for (const auto &pair : record.pairs) {
    writer.I32(pair.suppkey);
    for (int32_t y = 0; y < Q9Pair::kYears; ++y) {
      writer.I64(pair.rev[y]);
      writer.I64(pair.qty[y]);
    }
  }
  writer.U32(static_cast<uint32_t>(record.extras.size()));
  for (const auto &e : record.extras) {
    writer.I32(e.suppkey);
    writer.I32(e.year);
    writer.I64(e.rev);
    writer.I64(e.qty);
  }
}

bool ReadQ9Record(ByteReader &reader, int64_t &partkey, Q9PartRecord &record) {
  uint32_t exists = 0, cell_count = 0, pair_count = 0, extra_count = 0;
  if (!reader.I64(partkey) || !reader.String(record.normalized_name) ||
      !reader.U32(exists) || !reader.U32(cell_count))
    return false;
  record.exists = exists != 0;
  record.cells.reserve(cell_count);
  for (uint32_t j = 0; j < cell_count; ++j) {
    int32_t nationkey = 0, year = 0;
    int64_t profit = 0;
    if (!reader.I32(nationkey) || !reader.I32(year) || !reader.I64(profit))
      return false;
    record.cells.push_back(Q9ProfitCell::Make(nationkey, year, profit));
  }
  if (!reader.U32(pair_count))
    return false;
  record.pairs.resize(pair_count);
  for (auto &pair : record.pairs) {
    if (!reader.I32(pair.suppkey))
      return false;
    for (int32_t y = 0; y < Q9Pair::kYears; ++y)
      if (!reader.I64(pair.rev[y]) || !reader.I64(pair.qty[y]))
        return false;
  }
  if (!reader.U32(extra_count))
    return false;
  record.extras.resize(extra_count);
  for (auto &e : record.extras)
    if (!reader.I32(e.suppkey) || !reader.I32(e.year) || !reader.I64(e.rev) ||
        !reader.I64(e.qty))
      return false;
  return true;
}

} // namespace

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
                         static_cast<uint64_t>(partkeys.size()));
  for (const auto partkey : partkeys) {
    const auto *record = state.FindPart(partkey);
    if (!record) continue;
    WriteQ9Record(writer, partkey, *record);
  }

  // ── Name postings ───────────────────────────────────────────────────────
  writer.U32(static_cast<uint32_t>(state.name_postings.size()));
  std::vector<std::string> posting_keys;
  for (const auto &entry : state.name_postings)
    posting_keys.push_back(entry.first);
  std::sort(posting_keys.begin(), posting_keys.end());
  for (const auto &token : posting_keys) {
    writer.String(token);
    const auto &partkeys = *state.name_postings.at(token);
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
  writer.U64(order_keys.size());
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
    const auto &partkeys = *state.name_postings.at(token);
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
  writer.U64(order_keys.size());
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

  // suppkey -> partkeys with lineitem-aggregate pairs
  std::vector<std::pair<int64_t, std::shared_ptr<const std::vector<int64_t>>>>
      sp;
  for (std::size_t ci = 0; ci < state.supp_pairs.NumChunks(); ++ci) {
    const auto *page = state.supp_pairs.ChunkAt(ci);
    if (!page)
      continue;
    for (std::size_t slot = 0; slot < page->size(); ++slot)
      if ((*page)[slot])
        sp.emplace_back(state.supp_pairs.ChunkBase(ci) +
                            static_cast<int64_t>(slot),
                        (*page)[slot]);
  }
  writer.U32(static_cast<uint32_t>(sp.size()));
  for (const auto &[suppkey, parts] : sp) {
    writer.I64(suppkey);
    writer.U32(static_cast<uint32_t>(parts->size()));
    for (const auto pk : *parts)
      writer.I64(pk);
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
    auto &slot = state.name_postings[token];
    slot = std::make_shared<TpchQ9State::Posting>();
    auto &partkeys = *slot;
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

  uint64_t order_count = 0;
  if (!reader.U64(order_count))
    return false;
  state.order_years.Clear();
  state.order_year_overflow.clear();
  for (uint64_t i = 0; i < order_count; ++i) {
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

  if (!reader.U32(count))
    return false;
  state.supp_pairs.Clear();
  for (uint32_t i = 0; i < count; ++i) {
    int64_t suppkey = 0;
    uint32_t n = 0;
    if (!reader.I64(suppkey) || !reader.U32(n))
      return false;
    auto parts = std::make_shared<std::vector<int64_t>>();
    parts->reserve(n);
    for (uint32_t j = 0; j < n; ++j) {
      int64_t pk = 0;
      if (!reader.I64(pk))
        return false;
      parts->push_back(pk);
    }
    Q9BoundedSet(state.supp_pairs, suppkey,
                 std::shared_ptr<const std::vector<int64_t>>(std::move(parts)));
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
    WriteQ9Record(writer, partkey, *partition.parts.Find(partkey));
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
    Q9PartRecord record;
    if (!ReadQ9Record(reader, partkey, record))
      return false;
    partition.parts.Emplace(partkey, std::move(record));
  }
  return true;
}

std::vector<uint8_t> EncodeTpchQ9Cube(const TpchQ9State &state) {
  ByteWriter writer;
  core::WriteImageHeader(writer, kCubeMagic, kVersion,
                         static_cast<uint64_t>(state.cube.size()));
  std::vector<std::string> tokens;
  tokens.reserve(state.cube.size());
  for (const auto &entry : state.cube)
    tokens.push_back(entry.first);
  std::sort(tokens.begin(), tokens.end());
  for (const auto &token : tokens) {
    const auto &c = *state.cube.at(token);
    writer.String(token);
    for (int32_t n = 0; n < Q9TokenCube::kNations; ++n)
      for (int32_t y = 0; y < Q9TokenCube::kYears; ++y)
        writer.I64(c.dense[n][y]);
    writer.U32(static_cast<uint32_t>(c.sparse.size()));
    for (const auto &cell : c.sparse) {
      writer.I32(cell.Nation());
      writer.I32(cell.Year());
      writer.I64(cell.profit);
    }
  }
  return std::move(writer.bytes);
}

bool DecodeTpchQ9Cube(const std::vector<uint8_t> &bytes, TpchQ9State &state) {
  ByteReader reader{bytes};
  uint64_t count = 0;
  if (!core::ReadImageHeader(reader, kCubeMagic, kVersion, count))
    return false;
  state.cube.clear();
  for (uint32_t i = 0; i < count; ++i) {
    std::string token;
    if (!reader.String(token))
      return false;
    auto c = std::make_shared<Q9TokenCube>();
    for (int32_t n = 0; n < Q9TokenCube::kNations; ++n)
      for (int32_t y = 0; y < Q9TokenCube::kYears; ++y)
        if (!reader.I64(c->dense[n][y]))
          return false;
    uint32_t sparse_count = 0;
    if (!reader.U32(sparse_count))
      return false;
    for (uint32_t j = 0; j < sparse_count; ++j) {
      int32_t nation = 0, year = 0;
      int64_t profit = 0;
      if (!reader.I32(nation) || !reader.I32(year) || !reader.I64(profit))
        return false;
      c->sparse.push_back(Q9ProfitCell::Make(nation, year, profit));
    }
    state.cube.emplace(std::move(token), std::move(c));
  }
  state.cube_active = true;
  return reader.offset == bytes.size();
}

std::vector<uint8_t> EncodeTpchQ9Metadata(const TpchQ9State &state) {
  ByteWriter writer;
  core::WriteImageHeader(writer, kMetadataMagic, kVersion,
                         static_cast<uint64_t>(state.profit_slot_count));
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
  uint64_t slot_count = 0;
  if (!core::ReadImageHeader(reader, kMetadataMagic, kVersion, slot_count))
    return false;
  state.profit_partitions.clear();
  state.InitSlots(slot_count);
  if (!ReadTpchQ9MetadataBody(reader, state))
    return false;
  if (state.nation_emit_order.empty())
    state.RebuildNationEmitOrder();
  return reader.offset == bytes.size();
}

bool DecodeTpchQ9MaintenanceSide(const std::vector<uint8_t> &bytes,
                                 TpchQ9State &state) {
  ByteReader reader{bytes};
  uint64_t reserved = 0;
  if (!core::ReadImageHeader(reader, kMaintenanceSideMagic, kVersion, reserved))
    return false;
  if (!ReadTpchQ9MaintenanceSideBody(reader, state))
    return false;
  return reader.offset == bytes.size();
}

bool DecodeTpchQ9ProfitSlot(const std::vector<uint8_t> &bytes,
                            TpchQ9State::PartProfitPartition &partition) {
  ByteReader reader{bytes};
  uint64_t reserved = 0;
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
  uint64_t parts = 0;
  uint32_t count = 0;

  if (!core::ReadImageHeader(reader, kCompleteStateMagic, kVersion, parts))
    return false;

  // ── Parts (name + profit cells) ─────────────────────────────────────────
  state.profit_partitions.clear();
  state.InitPartitions(parts);
  for (uint32_t i = 0; i < parts; ++i) {
    int64_t partkey = 0;
    Q9PartRecord record;
    if (!ReadQ9Record(reader, partkey, record))
      return false;
    state.GetPartition(partkey).parts.Emplace(partkey, std::move(record));
  }

  // ── Name postings ───────────────────────────────────────────────────────
  if (!reader.U32(count)) return false;
  state.name_postings.clear();
  for (uint32_t i = 0; i < count; ++i) {
    std::string trigram;
    uint32_t pk_count = 0;
    if (!reader.String(trigram) || !reader.U32(pk_count))
      return false;
    auto &pk_slot = state.name_postings[trigram];
    pk_slot = std::make_shared<TpchQ9State::Posting>();
    auto &pk_vec = *pk_slot;
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
  uint64_t order_count = 0;
  if (!reader.U64(order_count)) return false;
  state.order_years.Clear();
  state.order_year_overflow.clear();
  for (uint64_t i = 0; i < order_count; ++i) {
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
  state.BuildSuppPairsIndex();

  return reader.offset == bytes.size();
}

// ── TxnBuffer ───────────────────────────────────────────────────────────────
bool TpchQ9TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ9TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
  order_deltas.clear();
  supplier_deltas.clear();
  partsupp_deltas.clear();
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
  supplier_deltas.clear();
  partsupp_deltas.clear();
  part_name_deltas.clear();
}
void TpchQ9TxnBuffer::AddSupplierDelta(TpchQ9SupplierDelta d) {
  pending = true;
  supplier_deltas.push_back(d);
}
void TpchQ9TxnBuffer::AddPartsuppDelta(TpchQ9PartsuppDelta d) {
  pending = true;
  partsupp_deltas.push_back(d);
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
  return {{config.part_table, {0, 1}, {0, 1}},
          {config.supplier_table, {0, 1}, {0, 3}},
          {config.nation_table, {0, 1}, {0, 1}},
          {config.orders_table, {0, 1}, {0, 4}},
          {config.partsupp_table, {0, 1, 2}, {0, 1, 3}},
          {config.lineitem_table, {0, 1, 2, 3, 4, 5}, {1, 2, 0, 4, 5, 6}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ9Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ9TxnBuffer>();
}

void TpchQ9Module::BuildStateInto(const core::AuxEngineServices &services,
                                  TpchQ9State &state, bool full) const {
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
  state.BeginBulkParts();
  state.postings_bulk = true;
  for (const auto &[partkey, name] : parts)
    state.AddNamePostings(partkey, name);
  parts.clear();
  parts.shrink_to_fit();

  if (!full) {
    // Fallback answer only: per-part cells straight from the pushdown when
    // available, otherwise from pair aggregates.
    const bool used_sql = initial_scan->ScanProfitContributions(
        [&](const TpchQ9ProfitContribution &input) {
          if (input.profit_raw == 0)
            return;
          static const std::vector<std::string> kNoTokens;
          state.AdjustCell(state.PartRecord(input.partkey), kNoTokens,
                           input.nationkey, input.year, input.profit_raw);
        });
    if (used_sql) {
      state.FinishBulkParts();
      state.FinalizePostings();
      state.RebuildNationEmitOrder();
      return;
    }
  }

  // Join-side maps (kept in state: maintenance resolves dimension and RF
  // writes against them, never point SQL).
  initial_scan->ScanSuppliers([&](const TpchQ9SupplierInput &input) {
    state.SetSupplierNation(input.suppkey, input.nationkey);
  });
  initial_scan->ScanOrders([&](const TpchQ9OrdersInput &input) {
    state.SetOrderYear(input.orderkey, TpchQ9ExtractYear(input.orderdate_days));
  });
  initial_scan->ScanPartsupps([&](const TpchQ9PartsuppInput &input) {
    state.SetPartsuppCost(input.partkey, input.suppkey, input.supplycost_raw);
  });

  // Lineitems become per-(part, supplier, order year) aggregates, bucketed by
  // profit slot: partitions fill their own buckets, then every slot (which
  // owns disjoint part records) is applied by one worker.
  using SlotPairs = std::vector<std::vector<PairAgg>>;
  const auto slots = state.profit_slot_count;
  SlotPairs pairs = core::PartitionedBuild<SlotPairs>(
      services, [slots] { return SlotPairs(slots); },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanLineitems(services, parts,
                                    [&](std::size_t p, const TpchQ9LineitemInput &input) {
                                      sink(p, TypedRowBatch<TpchQ9LineitemInput>{input});
                                    });
      },
      [&](SlotPairs &partial, const TpchQ9LineitemInput &input) {
        int32_t year = 0;
        if (input.suppkey < 1 || input.suppkey > static_cast<int64_t>(INT32_MAX) ||
            !state.LookupOrderYear(input.orderkey, year))
          return;
        partial[TpchQ9State::HashPartition(input.partkey, slots)].push_back(
            {input.partkey, input.suppkey, year,
             input.extendedprice_raw * (static_cast<int64_t>(100) - input.discount_raw),
             input.quantity_raw});
      },
      [](SlotPairs &into, SlotPairs &&from) {
        for (std::size_t slot = 0; slot < into.size(); ++slot) {
          into[slot].insert(into[slot].end(), from[slot].begin(), from[slot].end());
          std::vector<PairAgg>().swap(from[slot]);
        }
      });
  core::ParallelEach(services, pairs.size(), kPairSlotGrain, [&](std::size_t slot) {
    for (const auto &agg : pairs[slot])
      TpchQ9State::AddPairAgg(state.PartRecord(agg.partkey), static_cast<int32_t>(agg.suppkey),
                              agg.year, agg.rev, agg.qty);
    std::vector<PairAgg>().swap(pairs[slot]);
  });

  state.FinishBulkParts();
  state.FinalizePostings();
  state.RebuildNationEmitOrder();
  state.BuildCellsFromPairs(services);
  if (full) {
    state.BuildSuppPairsIndex();
    state.BuildCube(services);
  }
}

void TpchQ9Module::Build(core::AuxEngineServices &services,
                         core::StateBuilder &builder) {
  auto state = std::make_shared<TpchQ9State>();
  BuildStateInto(services, *state);

  // Metadata keeps name index + nation labels; profit cells live in slot
  // images. Everything is moved out of the build state so no published image
  // pins the whole build.
  auto metadata_state = std::make_shared<TpchQ9State>();
  metadata_state->nation_names = std::move(state->nation_names);
  metadata_state->name_postings = std::move(state->name_postings);
  metadata_state->nation_emit_order = std::move(state->nation_emit_order);
  metadata_state->profit_slot_count = state->profit_slot_count;
  metadata_state->profit_partitions.resize(state->profit_slot_count);
  builder.Write(core::StateImage::Make(
      TpchQ9Layout::kAccelerator, TpchQ9Layout::kCompleteState, 0,
      MakeMetadataImage(metadata_state)));

  // Dense join maps live on kMaintenanceSide; Execute never reads it.
  auto side_state = std::make_shared<TpchQ9State>();
  side_state->CopySideFrom(*state);
  state->supplier_nations.Clear();
  state->order_years.Clear();
  state->partsupp_rows.Clear();
  builder.Write(core::StateImage::Make(
      TpchQ9Layout::kAccelerator, TpchQ9Layout::kMaintenanceSide, 0,
      MakeMaintenanceSideImage(side_state)));

  auto cube_state = std::make_shared<TpchQ9State>();
  cube_state->cube = std::move(state->cube);
  cube_state->cube_active = true;
  cube_state->profit_slot_count = state->profit_slot_count;
  builder.Write(core::StateImage::Make(
      TpchQ9Layout::kAccelerator, TpchQ9Layout::kCube, 0,
      MakeCubeImage(cube_state)));

  for (core::PartitionId slot = 0; slot < state->profit_slot_count; ++slot) {
    auto slot_state = std::make_shared<TpchQ9State>();
    // Slot images hold exactly one partition (index 0).
    slot_state->profit_slot_count = state->profit_slot_count;
    slot_state->profit_partitions.resize(1);
    slot_state->profit_partitions[0] =
        std::move(state->profit_partitions[slot]);
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
          // Key change = delete old part + insert new part (pairs follow the
          // part record, so no rebuild is needed).
          buffer.AddPartNamePosting(old_pk, old_name, "");
          buffer.AddPartNamePosting(new_pk, "", new_name);
        } else if (old_name != new_name) {
          buffer.AddPartNamePosting(old_pk, old_name, new_name);
        }
      }
      break;
    }
    return;
  }

  // Supplier mutations: incremental over the supplier's (part, supp) pairs.
  if (mutation.table == config.supplier_table) {
    auto add = [&](const core::AuxRow &row, int8_t sign) {
      int64_t suppkey = 0, nationkey = 0;
      if (!ReadSupplier(row, suppkey, nationkey)) {
        buffer.MarkRebuild();
        return false;
      }
      buffer.AddSupplierDelta({suppkey, nationkey, sign});
      return true;
    };
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows)
        if (!add(row, 1)) return;
      break;
    case core::MutationKind::Delete:
      for (const auto &row : mutation.old_rows)
        if (!add(row, -1)) return;
      break;
    case core::MutationKind::Update:
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        int64_t ok = 0, onat = 0, nk = 0, nnat = 0;
        if (ReadSupplier(mutation.old_rows[i], ok, onat) &&
            ReadSupplier(mutation.new_rows[i], nk, nnat) && ok == nk &&
            onat == nnat)
          continue; // non-join column update: no-op for Q9
        if (!add(mutation.old_rows[i], -1) || !add(mutation.new_rows[i], 1))
          return;
      }
      break;
    }
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

  // Partsupp mutations: incremental per (part, supp) pair.
  if (mutation.table == config.partsupp_table) {
    auto add = [&](const core::AuxRow &row, int8_t sign) {
      int64_t partkey = 0, suppkey = 0, cost = 0;
      if (!ReadPartsupp(row, partkey, suppkey, cost)) {
        buffer.MarkRebuild();
        return false;
      }
      buffer.AddPartsuppDelta({partkey, suppkey, cost, sign});
      return true;
    };
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows)
        if (!add(row, 1)) return;
      break;
    case core::MutationKind::Delete:
      for (const auto &row : mutation.old_rows)
        if (!add(row, -1)) return;
      break;
    case core::MutationKind::Update:
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        int64_t opk = 0, osk = 0, oc = 0, npk = 0, nsk = 0, nc = 0;
        if (ReadPartsupp(mutation.old_rows[i], opk, osk, oc) &&
            ReadPartsupp(mutation.new_rows[i], npk, nsk, nc) && opk == npk &&
            osk == nsk && oc == nc)
          continue; // e.g. ps_availqty only
        if (!add(mutation.old_rows[i], -1) || !add(mutation.new_rows[i], 1))
          return;
      }
      break;
    }
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

  // An order whose year changes moves the profit of its existing lineitems
  // between years, which the deltas cannot do: rebuild.
  {
    std::unordered_map<int64_t, int32_t> removed_year;
    for (const auto &delta : buffer.OrderDeltas())
      if (delta.sign < 0)
        removed_year[delta.orderkey] = delta.order_year;
    for (const auto &delta : buffer.OrderDeltas()) {
      if (delta.sign <= 0)
        continue;
      const auto it = removed_year.find(delta.orderkey);
      if (it != removed_year.end() && it->second != delta.order_year) {
        core::FinishRebuildAll(outcome);
        return outcome;
      }
    }
  }

  // Try incremental apply for lineitem deltas + part name deltas + order years.
  // RF1: orders Insert must be buffered before lineitems in the same txn so
  // order_years is updated before ResolveProfit.
  uint64_t total_changes =
      static_cast<uint64_t>(buffer.Deltas().size() +
                             buffer.PartNameDeltas().size() +
                             buffer.OrderDeltas().size() +
                             buffer.SupplierDeltas().size() +
                             buffer.PartsuppDeltas().size());

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

  // Load metadata, the side/cube images, and only the profit slots whose parts
  // this batch can affect. Snapshots stay alive for the whole function.
  const auto visibility = services.CurrentVisibility();
  core::Version metadata_version = 0, side_version = 0, cube_version = 0;
  const TpchQ9StateImage *metadata_image = nullptr;
  const auto meta_snapshot = services.StateStore().ReadPartitions(
      visibility, {TpchQ9Layout::kCompleteState});
  for (const auto &image : meta_snapshot.partitions) {
    if (image.partition != TpchQ9Layout::kCompleteState)
      continue;
    metadata_image = AsQ9Image(image);
    metadata_version = image.version;
    break;
  }
  auto rebuild_all = [&]() {
    outcome.updates.clear();
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  };
  if (!metadata_image ||
      metadata_image->kind == TpchQ9ImageKind::CompleteState)
    return rebuild_all();

  const bool touch_names = !buffer.PartNameDeltas().empty();

  auto new_state = std::make_shared<TpchQ9State>();
  new_state->profit_slot_count = metadata_image->state->profit_slot_count;
  new_state->InitSlots(new_state->profit_slot_count);
  const auto slot_count = new_state->profit_slot_count;

  const TpchQ9State *join_side = nullptr;
  const TpchQ9State *cube_src = nullptr;
  const auto aux_snapshot = services.StateStore().ReadPartitions(
      visibility, {TpchQ9Layout::kMaintenanceSide, TpchQ9Layout::kCube});
  for (const auto &image : aux_snapshot.partitions) {
    const auto *typed = AsQ9Image(image);
    if (!typed)
      continue;
    if (image.partition == TpchQ9Layout::kMaintenanceSide &&
        typed->kind == TpchQ9ImageKind::MaintenanceSide) {
      join_side = typed->state.get();
      side_version = image.version;
    } else if (image.partition == TpchQ9Layout::kCube &&
               typed->kind == TpchQ9ImageKind::Cube) {
      cube_src = typed->state.get();
      cube_version = image.version;
    }
  }
  if (!join_side || !cube_src)
    return rebuild_all();
  new_state->cube = cube_src->cube; // shared_ptr copies; cloned on write
  new_state->cube_active = true;

  if (touch_names) {
    new_state->nation_names = metadata_image->state->nation_names;
    new_state->name_postings = metadata_image->state->name_postings; // COW
    new_state->nation_emit_order = metadata_image->state->nation_emit_order;
  }

  // Keys that do not fit the int32 pair index are not supported incrementally.
  auto key_ok = [](int64_t partkey, int64_t suppkey) {
    (void)partkey;
    return suppkey >= 1 && suppkey <= static_cast<int64_t>(INT32_MAX);
  };

  // ── Affected pairs + slots ──────────────────────────────────────────────
  std::set<core::PartitionId> touched_slots;
  std::vector<std::pair<int64_t, int32_t>> affected;
  auto touch = [&](int64_t partkey) {
    touched_slots.insert(TpchQ9State::HashPartition(partkey, slot_count));
  };
  for (const auto &d : buffer.Deltas()) {
    if (!key_ok(d.partkey, d.suppkey))
      return rebuild_all();
    touch(d.partkey);
    affected.emplace_back(d.partkey, static_cast<int32_t>(d.suppkey));
  }
  for (const auto &d : buffer.PartsuppDeltas()) {
    if (!key_ok(d.partkey, d.suppkey))
      return rebuild_all();
    touch(d.partkey);
    affected.emplace_back(d.partkey, static_cast<int32_t>(d.suppkey));
  }
  for (const auto &d : buffer.SupplierDeltas()) {
    if (!key_ok(0, d.suppkey))
      return rebuild_all();
    if (const auto list = join_side->SuppPairs(d.suppkey)) {
      for (const auto pk : *list) {
        touch(pk);
        affected.emplace_back(pk, static_cast<int32_t>(d.suppkey));
      }
    }
  }
  for (const auto &[partkey, _, __] : buffer.PartNameDeltas()) {
    (void)_;
    (void)__;
    if (!key_ok(partkey, 1))
      return rebuild_all();
    touch(partkey);
  }

  std::optional<core::StateSnapshot> slot_snapshot;
  if (!touched_slots.empty()) {
    std::vector<core::PartitionId> slot_partitions;
    slot_partitions.reserve(touched_slots.size());
    for (const auto slot : touched_slots)
      slot_partitions.push_back(TpchQ9Layout::ProfitSlotPartition(slot));
    slot_snapshot.emplace(
        services.StateStore().ReadPartitions(visibility, slot_partitions));
    std::vector<std::pair<core::PartitionId, const TpchQ9State *>> copies;
    for (const auto &image : slot_snapshot->partitions) {
      if (image.partition < TpchQ9Layout::kProfitSlotBase)
        continue;
      const auto *typed = AsQ9Image(image);
      if (!typed || typed->kind != TpchQ9ImageKind::ProfitSlot ||
          typed->state->profit_partitions.empty())
        continue;
      const auto slot = image.partition - TpchQ9Layout::kProfitSlotBase;
      if (slot < slot_count)
        copies.emplace_back(slot, typed->state.get());
    }
    // Slot copies (pointer vectors of shared records) are independent.
    core::ParallelSlices(services, copies.size(), kSlotCopyGrain,
                         [&](std::size_t, std::size_t begin, std::size_t end) {
                           for (std::size_t i = begin; i < end; ++i)
                             new_state->profit_partitions[copies[i].first] =
                                 copies[i].second->profit_partitions[0];
                         });
  }

  // Part changes affect every pair the part has aggregates for.
  for (const auto &[partkey, _, __] : buffer.PartNameDeltas()) {
    (void)_;
    (void)__;
    if (const auto *rec = new_state->FindPart(partkey))
      for (const auto &pair : rec->pairs)
        affected.emplace_back(partkey, pair.suppkey);
  }
  std::sort(affected.begin(), affected.end());
  affected.erase(std::unique(affected.begin(), affected.end()), affected.end());

  // Records are shared with the published slot images: Mutable() clones the
  // ones this write changes.
  auto find_rec = [&](int64_t partkey) -> Q9PartRecord * {
    return new_state->GetPartition(partkey).parts.Mutable(partkey);
  };

  // ── Pass A: remove affected pairs' contributions under the OLD state ────
  // Parts whose name changes keep the immediate per-token path (their tokens
  // differ between the two passes). Every other part's pair contributions are
  // collected as signed cell deltas and netted per (part, nation, year) at the
  // end, so an untouched year/pair costs nothing and the cube is updated once
  // per (token, nation, year) instead of twice per pair cell and token.
  std::unordered_set<int64_t> renamed;
  for (const auto &[partkey, _, __] : buffer.PartNameDeltas()) {
    (void)_;
    (void)__;
    renamed.insert(partkey);
  }
  constexpr std::size_t kChunk = 4096;
  const std::size_t chunk_count = (affected.size() + kChunk - 1) / kChunk;
  std::vector<std::vector<TpchQ9State::CellDelta>> net_chunks(chunk_count);
  std::vector<TpchQ9State::PairSnap> snaps(affected.size());
  std::vector<uint8_t> snap_valid(affected.size(), 0);
  // Read-only over the slots and the published side image: parallel by chunk.
  core::ParallelSlices(
      services, chunk_count, 1,
      [&](std::size_t, std::size_t chunk_begin, std::size_t chunk_end) {
        for (std::size_t c = chunk_begin; c < chunk_end; ++c) {
          const std::size_t end = std::min(affected.size(), (c + 1) * kChunk);
          for (std::size_t idx = c * kChunk; idx < end; ++idx) {
            const auto partkey = affected[idx].first;
            const auto suppkey = affected[idx].second;
            if (renamed.count(partkey))
              continue;
            const auto *rec = new_state->FindPart(partkey);
            if (!rec)
              continue;
            snap_valid[idx] = TpchQ9State::PairSnapshot(
                partkey, *rec, suppkey, -1, *join_side, snaps[idx],
                net_chunks[c]);
          }
        }
      });
  for (const auto &[partkey, suppkey] : affected) {
    if (!renamed.count(partkey))
      continue;
    auto *rec = find_rec(partkey);
    if (!rec || !TpchQ9State::FindPair(*rec, suppkey))
      continue;
    const auto tokens = NameTokens(rec->normalized_name);
    new_state->ApplyPairContribution(partkey, *rec, tokens, suppkey, -1,
                                     *join_side);
  }

  // ── Mutations (join side is a COW copy of the published side image) ─────
  auto side_work = std::make_shared<TpchQ9State>();
  side_work->CopySideFrom(*join_side);
  bool side_dirty = !buffer.OrderDeltas().empty() ||
                    !buffer.SupplierDeltas().empty() ||
                    !buffer.PartsuppDeltas().empty();

  std::unordered_map<int64_t, int32_t> year_overlay;
  for (const auto &delta : buffer.OrderDeltas()) {
    if (delta.sign > 0)
      year_overlay[delta.orderkey] = delta.order_year;
  }

  // Lineitem deltas move the pair aggregates (no cube work here: pass B).
  // Years are resolved first (the scan fallback is not thread-safe); the
  // aggregates are then applied per slot, grouped by part so each record is
  // looked up (and cloned) once.
  {
    const auto &deltas = buffer.Deltas();
    std::vector<int32_t> years(deltas.size());
    for (std::size_t k = 0; k < deltas.size(); ++k)
      if (!ResolveOrderYear(*join_side, initial_scan, deltas[k].orderkey,
                            years[k], year_overlay))
        return rebuild_all();
    std::vector<std::vector<uint32_t>> by_slot(slot_count);
    for (uint32_t k = 0; k < deltas.size(); ++k)
      by_slot[TpchQ9State::HashPartition(deltas[k].partkey, slot_count)]
          .push_back(k);
    std::vector<core::PartitionId> active;
    for (core::PartitionId slot = 0; slot < slot_count; ++slot)
      if (!by_slot[slot].empty())
        active.push_back(slot);
    std::vector<std::vector<std::pair<int64_t, int64_t>>> created(
        active.size());
    // Slots are disjoint; each task owns its slot's records and `created[t]`.
    core::ParallelSlices(
        services, active.size(), kSlotApplyGrain,
        [&](std::size_t, std::size_t begin, std::size_t end) {
          for (std::size_t t = begin; t < end; ++t) {
            auto &order = by_slot[active[t]];
            std::stable_sort(order.begin(), order.end(),
                             [&](uint32_t a, uint32_t b) {
                               return deltas[a].partkey < deltas[b].partkey;
                             });
            Q9PartRecord *rec = nullptr;
            int64_t rec_partkey = 0;
            for (const auto k : order) {
              const auto &delta = deltas[k];
              if (!rec || rec_partkey != delta.partkey) {
                rec = &new_state->PartRecord(delta.partkey);
                rec_partkey = delta.partkey;
              }
              const int64_t sign = delta.sign;
              const int64_t rev =
                  delta.extendedprice_raw *
                  (static_cast<int64_t>(100) - delta.discount_raw);
              if (TpchQ9State::AddPairAgg(*rec,
                                          static_cast<int32_t>(delta.suppkey),
                                          years[k], sign * rev,
                                          sign * delta.quantity_raw))
                created[t].emplace_back(delta.suppkey, delta.partkey);
            }
          }
        });
    for (const auto &list : created)
      for (const auto &[suppkey, partkey] : list) {
        side_work->AddSuppPair(suppkey, partkey);
        side_dirty = true;
      }
  }

  for (const auto &d : buffer.SupplierDeltas()) {
    if (d.sign < 0)
      side_work->EraseSupplierNation(d.suppkey);
    else
      side_work->SetSupplierNation(d.suppkey,
                                   static_cast<int32_t>(d.nationkey));
  }
  for (const auto &d : buffer.PartsuppDeltas()) {
    if (d.sign < 0)
      side_work->ErasePartsupp(d.partkey, d.suppkey);
    else
      side_work->SetPartsuppCost(d.partkey, d.suppkey, d.cost_raw);
  }
  // Orders: deletes then inserts (lineitems already resolved against the old
  // year map above).
  for (const auto &delta : buffer.OrderDeltas())
    if (delta.sign < 0)
      side_work->EraseOrderYear(delta.orderkey);
  for (const auto &delta : buffer.OrderDeltas())
    if (delta.sign > 0)
      side_work->SetOrderYear(delta.orderkey, delta.order_year);

  // Part name / existence changes (all of the part's pairs are in `affected`).
  for (const auto &[partkey, old_name, new_name] : buffer.PartNameDeltas()) {
    if (!old_name.empty()) {
      new_state->RemoveNamePostings(partkey, old_name);
      if (auto *rec = find_rec(partkey)) {
        rec->normalized_name.clear();
        rec->exists = false;
      }
    }
    if (!new_name.empty()) {
      new_state->AddNamePostings(partkey, new_name);
    } else if (auto *rec = find_rec(partkey)) {
      if (rec->pairs.empty() && rec->extras.empty() && rec->cells.empty())
        new_state->GetPartition(partkey).parts.Erase(partkey);
    }
  }

  // ── Pass B: add affected pairs' contributions under the NEW state ───────
  core::ParallelSlices(
      services, chunk_count, 1,
      [&](std::size_t, std::size_t chunk_begin, std::size_t chunk_end) {
        for (std::size_t c = chunk_begin; c < chunk_end; ++c) {
          auto &out = net_chunks[c];
          const std::size_t end = std::min(affected.size(), (c + 1) * kChunk);
          for (std::size_t idx = c * kChunk; idx < end; ++idx) {
            const auto partkey = affected[idx].first;
            const auto suppkey = affected[idx].second;
            if (renamed.count(partkey))
              continue;
            const auto *rec = new_state->FindPart(partkey);
            if (!rec)
              continue;
            TpchQ9State::PairSnap now;
            const bool now_valid = TpchQ9State::PairSnapshot(
                partkey, *rec, suppkey, +1, *side_work, now, out);
            const auto &old = snaps[idx];
            const bool old_valid = snap_valid[idx] != 0;
            if (old_valid && now_valid && old.nation == now.nation) {
              for (int32_t yi = 0; yi < Q9Pair::kYears; ++yi) {
                const int64_t d = now.v[yi] - old.v[yi];
                if (d != 0)
                  out.push_back({partkey, now.nation, Q9Pair::kYearBase + yi, d});
              }
              continue;
            }
            if (old_valid)
              for (int32_t yi = 0; yi < Q9Pair::kYears; ++yi)
                if (old.v[yi] != 0)
                  out.push_back(
                      {partkey, old.nation, Q9Pair::kYearBase + yi, -old.v[yi]});
            if (now_valid)
              for (int32_t yi = 0; yi < Q9Pair::kYears; ++yi)
                if (now.v[yi] != 0)
                  out.push_back(
                      {partkey, now.nation, Q9Pair::kYearBase + yi, now.v[yi]});
          }
        }
      });
  for (const auto &[partkey, suppkey] : affected) {
    if (!renamed.count(partkey))
      continue;
    auto *rec = find_rec(partkey);
    if (!rec || !TpchQ9State::FindPair(*rec, suppkey))
      continue;
    const auto tokens = NameTokens(rec->normalized_name);
    new_state->ApplyPairContribution(partkey, *rec, tokens, suppkey, +1,
                                     *side_work);
  }
  new_state->ApplyNetCellDeltas(services, net_chunks);

  outcome.action = core::MaintenanceAction::ApplyDelta;

  if (touch_names) {
    auto mutable_meta = std::make_shared<TpchQ9State>();
    mutable_meta->nation_names = new_state->nation_names;
    mutable_meta->name_postings = std::move(new_state->name_postings);
    mutable_meta->nation_emit_order = new_state->nation_emit_order;
    mutable_meta->profit_slot_count = slot_count;
    if (mutable_meta->nation_emit_order.empty())
      mutable_meta->RebuildNationEmitOrder();
    mutable_meta->InitSlots(slot_count);
    outcome.updates.push_back(core::StateImage::Make(
        TpchQ9Layout::kAccelerator, TpchQ9Layout::kCompleteState,
        metadata_version, MakeMetadataImage(mutable_meta)));
  }

  for (const auto slot : touched_slots) {
    auto slot_state = std::make_shared<TpchQ9State>();
    slot_state->profit_slot_count = slot_count;
    slot_state->profit_partitions.resize(1);
    // new_state is discarded after this call, so move rather than copy.
    slot_state->profit_partitions[0] =
        std::move(new_state->profit_partitions[slot]);
    outcome.updates.push_back(core::StateImage::Make(
        TpchQ9Layout::kAccelerator, TpchQ9Layout::ProfitSlotPartition(slot), 0,
        MakeProfitSlotImage(slot, slot_state)));
  }

  if (!touched_slots.empty()) {
    auto cube_state = std::make_shared<TpchQ9State>();
    cube_state->cube = std::move(new_state->cube);
    cube_state->cube_active = true;
    cube_state->profit_slot_count = slot_count;
    outcome.updates.push_back(core::StateImage::Make(
        TpchQ9Layout::kAccelerator, TpchQ9Layout::kCube, cube_version,
        MakeCubeImage(cube_state)));
  }
  if (side_dirty) {
    outcome.updates.push_back(core::StateImage::Make(
        TpchQ9Layout::kAccelerator, TpchQ9Layout::kMaintenanceSide,
        side_version, MakeMaintenanceSideImage(side_work)));
  }

  return outcome;
}

bool TpchQ9Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &arguments) const {
  if (arguments.values.empty())
    return false;
  // Validate only the single complete-state partition (was: empty list → 130-slot scan)
  return services.StateStore()
      .CheckValidity({Id(), std::to_string(TpchQ9Layout::kCompleteState)},
                     services.CurrentVisibility())
      .IsValid();
}

namespace {

// Dense accumulator shared by the cube and per-part read paths.
constexpr int32_t kDenseNations = kMaxNationKey + 1;
constexpr int32_t kDenseYears = kYearCount;

struct Q9Accumulator {
  int64_t dense_profit[kDenseNations][kDenseYears] = {};
  bool dense_valid[kDenseNations][kDenseYears] = {};
  bool nation_seen[kDenseNations] = {};
  std::unordered_map<Q9CellKey, int64_t, Q9CellKeyHash> sparse;

  void AddCell(int32_t nation, int32_t year, int64_t profit) {
    const int32_t year_idx = year - kYearBase;
    if (nation >= 0 && nation < kDenseNations && year_idx >= 0 &&
        year_idx < kDenseYears) {
      dense_profit[nation][year_idx] += profit;
      dense_valid[nation][year_idx] = true;
      nation_seen[nation] = true;
    } else {
      sparse[Q9CellKey{nation, year}] += profit;
    }
  }

  void Merge(const Q9Accumulator &from) {
    for (int32_t n = 0; n < kDenseNations; ++n) {
      nation_seen[n] = nation_seen[n] || from.nation_seen[n];
      for (int32_t y = 0; y < kDenseYears; ++y) {
        dense_profit[n][y] += from.dense_profit[n][y];
        dense_valid[n][y] = dense_valid[n][y] || from.dense_valid[n][y];
      }
    }
    for (const auto &[key, profit] : from.sparse)
      sparse[key] += profit;
  }
};

void EmitEmpty(core::AuxRowSink &sink) {
  sink.Begin(core::OutputSchema{});
  sink.End();
}

void EmitAccumulated(const TpchQ9State &state, const Q9Accumulator &acc,
                     core::AuxRowSink &sink) {
  std::vector<std::pair<std::string, int32_t>> fallback_order;
  if (state.nation_emit_order.empty()) {
    fallback_order.reserve(state.nation_names.size());
    for (const auto &[nk, name] : state.nation_names)
      fallback_order.emplace_back(name, nk);
    std::sort(fallback_order.begin(), fallback_order.end());
  }
  const auto &nations = state.nation_emit_order.empty() ? fallback_order
                                                        : state.nation_emit_order;

  bool has_rows = !acc.sparse.empty();
  if (!has_rows) {
    for (int32_t nk = 0; nk < kDenseNations && !has_rows; ++nk) {
      if (!acc.nation_seen[nk]) continue;
      for (int32_t yi = 0; yi < kDenseYears; ++yi) {
        if (acc.dense_valid[nk][yi] && acc.dense_profit[nk][yi] != 0) {
          has_rows = true;
          break;
        }
      }
    }
  }

  if (!has_rows) {
    EmitEmpty(sink);
    return;
  }

  sink.Begin(core::OutputSchema{});
  for (const auto &[nation_name, nk] : nations) {
    if (nk < 0 || nk >= kDenseNations || !acc.nation_seen[nk]) continue;
    for (int32_t yi = kDenseYears - 1; yi >= 0; --yi) {
      if (!acc.dense_valid[nk][yi]) continue;
      const int64_t profit = acc.dense_profit[nk][yi];
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

  if (!acc.sparse.empty()) {
    const std::string empty_str;
    std::vector<std::pair<Q9CellKey, int64_t>> sparse_entries;
    sparse_entries.reserve(acc.sparse.size());
    for (const auto &[key, profit] : acc.sparse) {
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

// Intersect the posting lists of all tokens. Returns false when some token has
// no posting list (needle is then not a whole-token match).
bool IntersectPostings(const TpchQ9State &meta,
                       const std::vector<std::string> &tokens,
                       std::vector<int32_t> &out) {
  out.clear();
  const std::vector<int32_t> *best = nullptr;
  std::vector<const std::vector<int32_t> *> postings;
  postings.reserve(tokens.size());
  for (const auto &token : tokens) {
    auto it = meta.name_postings.find(token);
    if (it == meta.name_postings.end())
      return false;
    const std::vector<int32_t> *posting = it->second.get();
    postings.push_back(posting);
    if (!best || posting->size() < best->size())
      best = posting;
  }
  if (!best)
    return false;
  for (const auto pk : *best) {
    bool in_all = true;
    for (const auto *posting : postings) {
      if (posting == best)
        continue;
      if (!PostingContains(*posting, pk)) {
        in_all = false;
        break;
      }
    }
    if (in_all)
      out.push_back(pk);
  }
  return true;
}

} // namespace

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
           .CheckValidity({Id(), std::to_string(TpchQ9Layout::kCompleteState)},
                          services.CurrentVisibility())
           .IsValid()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  // One snapshot of metadata + cube; everything is read in place from the
  // shared immutable images (no assembly / copying).
  const auto visibility = services.CurrentVisibility();
  const auto snapshot = services.StateStore().ReadPartitions(
      visibility, {TpchQ9Layout::kCompleteState, TpchQ9Layout::kCube});
  const TpchQ9StateImage *meta_image = nullptr;
  const TpchQ9StateImage *cube_image = nullptr;
  for (const auto &image : snapshot.partitions) {
    const auto *typed = AsQ9Image(image);
    if (!typed)
      continue;
    if (image.partition == TpchQ9Layout::kCompleteState)
      meta_image = typed;
    else if (image.partition == TpchQ9Layout::kCube &&
             typed->kind == TpchQ9ImageKind::Cube)
      cube_image = typed;
  }
  if (!meta_image) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  const TpchQ9State &meta = *meta_image->state;
  const std::string needle = NormalizeForIndex(color);
  const auto tokens = NameTokens(needle);

  // Fully assembled legacy image: profit cells live in the image itself.
  if (meta_image->kind == TpchQ9ImageKind::CompleteState) {
    std::vector<const TpchQ9State::PartProfitPartition *> slots;
    slots.reserve(meta.profit_partitions.size());
    for (const auto &partition : meta.profit_partitions)
      slots.push_back(&partition);
    std::vector<int32_t> candidates;
    const bool use_candidates =
        !tokens.empty() && IntersectPostings(meta, tokens, candidates);
    EmitResult(services, meta, slots, color,
               use_candidates ? &candidates : nullptr, sink);
    return;
  }

  if (tokens.empty()) {
    EmitEmpty(sink);
    return;
  }

  // Cube fast path: needle is exactly one whole token present in the index.
  if (cube_image && tokens.size() == 1 && tokens[0] == needle &&
      meta.name_postings.find(needle) != meta.name_postings.end()) {
    Q9Accumulator acc;
    const auto it = cube_image->state->cube.find(needle);
    if (it != cube_image->state->cube.end()) {
      const Q9TokenCube &c = *it->second;
      for (int32_t n = 0; n < Q9TokenCube::kNations; ++n)
        for (int32_t y = 0; y < Q9TokenCube::kYears; ++y) {
          const int64_t v = c.dense[n][y];
          if (v == 0)
            continue;
          acc.dense_profit[n][y] = v;
          acc.dense_valid[n][y] = true;
          acc.nation_seen[n] = true;
        }
      for (const auto &cell : c.sparse)
        acc.AddCell(cell.Nation(), cell.Year(), cell.profit);
    }
    EmitAccumulated(meta, acc, sink);
    return;
  }

  // General path: candidate parts from the posting intersection (or every
  // part when the needle is not a whole indexed token), slots read in place.
  std::vector<int32_t> candidates;
  const bool use_candidates = IntersectPostings(meta, tokens, candidates);
  const auto slot_count = meta.profit_slot_count;
  std::vector<core::PartitionId> slot_partitions;
  if (use_candidates) {
    std::vector<bool> seen(slot_count, false);
    for (const auto pk : candidates) {
      const auto slot = TpchQ9State::HashPartition(pk, slot_count);
      if (!seen[slot]) {
        seen[slot] = true;
        slot_partitions.push_back(TpchQ9Layout::ProfitSlotPartition(slot));
      }
    }
  } else {
    slot_partitions.reserve(slot_count);
    for (core::PartitionId slot = 0; slot < slot_count; ++slot)
      slot_partitions.push_back(TpchQ9Layout::ProfitSlotPartition(slot));
  }

  std::vector<const TpchQ9State::PartProfitPartition *> slots(slot_count,
                                                              nullptr);
  std::optional<core::StateSnapshot> slot_snapshot;
  if (!slot_partitions.empty()) {
    slot_snapshot.emplace(
        services.StateStore().ReadPartitions(visibility, slot_partitions));
    for (const auto &image : slot_snapshot->partitions) {
      if (image.partition < TpchQ9Layout::kProfitSlotBase)
        continue;
      const auto *typed = AsQ9Image(image);
      if (!typed || typed->kind != TpchQ9ImageKind::ProfitSlot ||
          typed->state->profit_partitions.empty())
        continue;
      const auto slot = image.partition - TpchQ9Layout::kProfitSlotBase;
      if (slot < slot_count)
        slots[slot] = &typed->state->profit_partitions[0];
    }
  }
  EmitResult(services, meta, slots, color,
             use_candidates ? &candidates : nullptr, sink);
}

void TpchQ9Module::EmitResult(
    const core::AuxEngineServices &services, const TpchQ9State &meta,
    const std::vector<const TpchQ9State::PartProfitPartition *> &slots,
    const std::string &color, const std::vector<int32_t> *candidates,
    core::AuxRowSink &sink) const {
  const std::string needle = NormalizeForIndex(color);
  const auto tokens = NameTokens(needle);
  if (tokens.empty()) {
    EmitEmpty(sink);
    return;
  }

  Q9Accumulator acc;
  const std::string_view needle_view(needle);
  const auto slot_count = static_cast<core::PartitionId>(slots.size());

  auto visit = [&](Q9Accumulator &into, const Q9PartRecord &record) {
    if (!ContainsSubstring(record.normalized_name, needle_view))
      return;
    for (const auto &cell : record.cells)
      into.AddCell(cell.Nation(), cell.Year(), cell.profit);
  };
  const auto merge = [](Q9Accumulator &into, Q9Accumulator &&from) {
    into.Merge(from);
  };

  if (candidates) {
    core::ParallelReduce(
        services, candidates->size(), kCandidateGrain, acc,
        [&](Q9Accumulator &part, std::size_t begin, std::size_t end) {
          for (std::size_t i = begin; i < end; ++i) {
            const auto pk = (*candidates)[i];
            if (slot_count == 0)
              break;
            const auto *partition =
                slots[TpchQ9State::HashPartition(pk, slot_count)];
            if (!partition)
              continue;
            if (const auto *record = partition->parts.Find(pk))
              visit(part, *record);
          }
        },
        merge);
  } else {
    // Needle is not a whole indexed token: exact substring scan of all parts.
    core::ParallelReduce(
        services, slots.size(), kSlotGrain, acc,
        [&](Q9Accumulator &part, std::size_t begin, std::size_t end) {
          for (std::size_t i = begin; i < end; ++i) {
            if (!slots[i])
              continue;
            for (const auto &entry : slots[i]->parts)
              visit(part, *entry.second);
          }
        },
        merge);
  }

  EmitAccumulated(meta, acc, sink);
}

std::vector<core::CalibrationWrite> TpchQ9Module::CalibrationWrites() const {
  return TpchQ9CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query