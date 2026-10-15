#include "tpch/q19/duckdb/aux/module.hpp"
#include "tpch/q19/calibration_writes.hpp"
#include "substrate/aux/core/byte_stream.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>


namespace accelerator_codegen::aux_state::query {
namespace {

using core::ByteReader;
using core::ByteWriter;

constexpr uint32_t kAggregateMagic = 0x51313941; // Q19A
constexpr uint32_t kVersionV2 = 2;
static constexpr uint32_t kVersionV1_Deprecated = 1;

// ── Custom profiling (no-op unless ACCEL_AUX_CUSTOM_PROFILE is set) ─────────
struct CustomTimer {
  using Clock = std::chrono::high_resolution_clock;
  Clock::time_point start;
  bool active;
  explicit CustomTimer(bool enabled)
      : start(enabled ? Clock::now() : Clock::time_point()), active(enabled) {}
  double us() const {
    if (!active) return 0.0;
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count()) / 1000.0;
  }
};

static bool CustomProfilingEnabled() {
  static const bool enabled = []() {
    const char *v = std::getenv("ACCEL_AUX_CUSTOM_PROFILE");
    return v && v[0] != '\0';
  }();
  return enabled;
}

static void EmitCustomProfile(const char *phase, double us_val) {
  if (!CustomProfilingEnabled()) return;
  fprintf(stderr, "ACCEL_CUSTOM_PROFILE {\"phase\":\"%s\",\"us\":%.3f}\n", phase, us_val);
}

static void EmitCustomProfile2(const char *phase, double us_val,
                               int64_t groups, int64_t rows) {
  if (!CustomProfilingEnabled()) return;
  fprintf(stderr,
          "ACCEL_CUSTOM_PROFILE {\"phase\":\"%s\",\"us\":%.3f,\"groups\":%ld,\"rows\":%ld}\n",
          phase, us_val, (long)groups, (long)rows);
}

static void EmitCustomProfile3(const char *phase, double us_val,
                               int64_t groups, int64_t rows, int64_t items) {
  if (!CustomProfilingEnabled()) return;
  fprintf(stderr,
          "ACCEL_CUSTOM_PROFILE {\"phase\":\"%s\",\"us\":%.3f,\"groups\":%ld,\"rows\":%ld,\"items\":%ld}\n",
          phase, us_val, (long)groups, (long)rows, (long)items);
}

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

bool Int32Cell(const core::AuxRow &row, std::size_t index, int32_t &out) {
  int64_t value = 0;
  if (!IntCell(row, index, value))
    return false;
  out = static_cast<int32_t>(value);
  return true;
}

const std::string *StringCellPtr(const core::AuxRow &row, std::size_t index) {
  if (index >= row.values.size() || row.values[index].is_null)
    return nullptr;
  return std::get_if<std::string>(&row.values[index].payload);
}

// Parses one lineitem row; sets `qualifies` when the shipping predicate holds
// (no string copies). Returns false when the row cannot be interpreted.
bool ReadLineitemRow(const core::AuxRow &row, int64_t &partkey,
                     int64_t &quantity_raw, int64_t &extendedprice_raw,
                     int64_t &discount_raw, bool &qualifies) {
  int32_t qty_scale = 0, ext_scale = 0, disc_scale = 0;
  const std::string *shipmode = StringCellPtr(row, 4);
  const std::string *shipinstruct = StringCellPtr(row, 5);
  if (!IntCell(row, 0, partkey) ||
      !RawDecimalCell(row, 1, quantity_raw, qty_scale) ||
      !RawDecimalCell(row, 2, extendedprice_raw, ext_scale) ||
      !RawDecimalCell(row, 3, discount_raw, disc_scale) || !shipmode ||
      !shipinstruct)
    return false;
  if (qty_scale != 2 || ext_scale != 2 || disc_scale != 2)
    return false;
  qualifies = (*shipmode == "AIR" || *shipmode == "AIR REG") &&
              *shipinstruct == "DELIVER IN PERSON";
  return true;
}

// ── Q19 family classification ───────────────────────────────────────────────
TpchQ19Family ClassifyContainerFamily(const std::string &container) {
  if (container == "SM CASE" || container == "SM BOX" || container == "SM PACK" ||
      container == "SM PKG")
    return TpchQ19Family::SM;
  if (container == "MED BAG" || container == "MED BOX" ||
      container == "MED PKG" || container == "MED PACK")
    return TpchQ19Family::MED;
  if (container == "LG CASE" || container == "LG BOX" || container == "LG PACK" ||
      container == "LG PKG")
    return TpchQ19Family::LG;
  return TpchQ19Family::None;
}

int32_t BranchSizeLimit(TpchQ19Family family) {
  switch (family) {
  case TpchQ19Family::SM: return 5;
  case TpchQ19Family::MED: return 10;
  case TpchQ19Family::LG: return 15;
  default: return 0;
  }
}

// ── Shipping predicate ──────────────────────────────────────────────────────
bool ShippingQualifies(const TpchQ19LineitemInput &li) {
  return (li.shipmode == "AIR" || li.shipmode == "AIR REG") &&
         li.shipinstruct == "DELIVER IN PERSON";
}

// ── Revenue computation ─────────────────────────────────────────────────────
int64_t ComputeRevenue(int64_t extendedprice_raw, int64_t discount_raw) {
  return TpchQ19RevenueRaw(extendedprice_raw, discount_raw);
}
} // namespace



// ── State accessors ─────────────────────────────────────────────────────────
void TpchQ19State::AddContribution(const std::string &brand,
                                    TpchQ19Family family, int32_t size,
                                    int64_t quantity_coord, int64_t delta) {
  // Pre-filter out-of-family-size rows so the flat layout can fold size
  // into the family branch.
  if (size < 1 || size > BranchSizeLimit(family)) return;
  int idx = FamilyIndex(family);
  if (idx < 0 || idx >= kNumFamilies) return;
  auto &vec = aggregates.Mut(brand)[idx];
  // Binary search for qty
  auto it = std::lower_bound(vec.begin(), vec.end(), quantity_coord,
                             [](const QtyRev &qr, int64_t q) { return qr.qty < q; });
  if (it != vec.end() && it->qty == quantity_coord) {
    it->rev += delta;
    it->count += 1;
  } else {
    vec.insert(it, QtyRev{quantity_coord, delta, 1});
  }
}

void TpchQ19State::AddAggregate(const std::string &brand, TpchQ19Family family,
                                int64_t quantity_coord, int64_t rev,
                                int64_t count) {
  const int idx = FamilyIndex(family);
  if (idx < 0 || idx >= kNumFamilies || count <= 0) return;
  auto &vec = aggregates.Mut(brand)[idx];
  auto it = std::lower_bound(vec.begin(), vec.end(), quantity_coord,
                             [](const QtyRev &qr, int64_t q) { return qr.qty < q; });
  if (it != vec.end() && it->qty == quantity_coord) {
    it->rev += rev;
    it->count += count;
  } else {
    vec.insert(it, QtyRev{quantity_coord, rev, count});
  }
}

int64_t TpchQ19State::RangeSumCount(const std::string &brand,
                                    TpchQ19Family family, int64_t qty_low,
                                    int64_t qty_high, int64_t &count) const {
  count = 0;
  const int idx = FamilyIndex(family);
  if (idx < 0 || idx >= kNumFamilies) return 0;
  const BrandAgg *brand_agg = aggregates.Find(brand);
  if (!brand_agg) return 0;
  const auto &vec = (*brand_agg)[idx];
  auto lo = std::lower_bound(vec.begin(), vec.end(), qty_low,
                             [](const QtyRev &qr, int64_t q) { return qr.qty < q; });
  int64_t total = 0;
  for (auto it = lo; it != vec.end() && it->qty <= qty_high; ++it) {
    total += it->rev;
    count += it->count;
  }
  return total;
}

int64_t TpchQ19State::RangeCount(const std::string &brand, TpchQ19Family family,
                                 int64_t qty_low, int64_t qty_high) const {
  const int idx = FamilyIndex(family);
  if (idx < 0 || idx >= kNumFamilies) return 0;
  const BrandAgg *brand_agg = aggregates.Find(brand);
  if (!brand_agg) return 0;
  const auto &vec = (*brand_agg)[idx];
  auto lo = std::lower_bound(vec.begin(), vec.end(), qty_low,
                             [](const QtyRev &qr, int64_t q) { return qr.qty < q; });
  auto hi = std::upper_bound(vec.begin(), vec.end(), qty_high,
                             [](int64_t q, const QtyRev &qr) { return q < qr.qty; });
  int64_t total = 0;
  for (auto it = lo; it != hi; ++it)
    total += it->count;
  return total;
}

int64_t TpchQ19State::RangeSum(const std::string &brand,
                                TpchQ19Family family, int32_t size_low,
                                int32_t size_high, int64_t qty_low,
                                int64_t qty_high) const {
  (void)size_low; (void)size_high;  // size folded into family
  int idx = FamilyIndex(family);
  if (idx < 0 || idx >= kNumFamilies) return 0;
  const BrandAgg *brand_agg = aggregates.Find(brand);
  if (!brand_agg) return 0;
  const auto &vec = (*brand_agg)[idx];
  if (vec.empty()) return 0;
  int64_t total = 0;
  auto qty_it = std::lower_bound(vec.begin(), vec.end(), qty_low,
                                  [](const QtyRev &qr, int64_t q) { return qr.qty < q; });
  auto qty_end = std::upper_bound(vec.begin(), vec.end(), qty_high,
                                   [](int64_t q, const QtyRev &qr) { return q < qr.qty; });
  for (auto it = qty_it; it != qty_end; ++it)
    total += it->rev;
  return total;
}

void TpchQ19State::RemoveContribution(const std::string &brand,
                                       TpchQ19Family family, int32_t size,
                                       int64_t quantity_coord, int64_t delta) {
  if (size < 1 || size > BranchSizeLimit(family)) return;
  int idx = FamilyIndex(family);
  if (idx < 0 || idx >= kNumFamilies) return;
  if (!aggregates.Find(brand)) return;
  {
    const auto &cvec = (*aggregates.Find(brand))[idx];
    auto cit = std::lower_bound(cvec.begin(), cvec.end(), quantity_coord,
                                [](const QtyRev &qr, int64_t q) { return qr.qty < q; });
    if (cit == cvec.end() || cit->qty != quantity_coord) return;
  }
  auto &vec = aggregates.Mut(brand)[idx];
  auto qty_it = std::lower_bound(vec.begin(), vec.end(), quantity_coord,
                                  [](const QtyRev &qr, int64_t q) { return qr.qty < q; });
  qty_it->rev -= delta;
  qty_it->count -= 1;
  if (qty_it->count <= 0)
    vec.erase(qty_it);
  // (An all-empty brand stays as an empty slot; it is skipped when encoded.)
}






// ── Encode / Decode ─────────────────────────────────────────────────────────
std::vector<uint8_t> EncodeTpchQ19Aggregate(const TpchQ19State &state) {
  ByteWriter w;
  core::WriteImageHeader(w, kAggregateMagic, kVersionV2, 0); // count unused

  // brand count
  w.U32(static_cast<uint32_t>(state.aggregates.NonEmptyCount()));
  state.aggregates.ForEachNonEmpty([&](const std::string &brand,
                                       const TpchQ19State::BrandAgg &family_vecs) {
    w.String(brand);
    // family count (always 3 slots, only write non-empty)
    uint32_t non_empty_families = 0;
    for (int fi = 0; fi < TpchQ19State::kNumFamilies; ++fi) {
      if (!family_vecs[fi].empty()) non_empty_families++;
    }
    w.U32(non_empty_families);
    for (int fi = 0; fi < TpchQ19State::kNumFamilies; ++fi) {
      if (family_vecs[fi].empty()) continue;
      auto family = static_cast<TpchQ19Family>(fi + 1); // SM=1, MED=2, LG=3
      w.U8(static_cast<uint8_t>(family));
      // size count: always 1 (size folded into family)
      w.U32(1);
      w.I32(0); // dummy size (for compat)
      // quantity count
      w.U32(static_cast<uint32_t>(family_vecs[fi].size()));
      for (const auto &qr : family_vecs[fi]) {
        w.I64(qr.qty);
        w.I64(qr.rev);
        w.I64(qr.count);
      }
    }
  });

  return std::move(w.bytes);
}

bool DecodeTpchQ19Aggregate(const std::vector<uint8_t> &bytes,
                            TpchQ19State &state) {
  ByteReader reader(bytes);
  uint64_t unused_count = 0;
  if (!core::ReadImageHeader(reader, kAggregateMagic, kVersionV2, unused_count)) {
    reader.offset = 0;
    if (!core::ReadImageHeader(reader, kAggregateMagic, kVersionV1_Deprecated, unused_count))
      return false;
  }

  state.aggregates.Clear();

  uint32_t brand_count = 0;
  if (!reader.U32(brand_count))
    return false;

  for (uint32_t bi = 0; bi < brand_count; ++bi) {
    std::string brand;
    if (!reader.String(brand))
      return false;

    uint32_t family_count = 0;
    if (!reader.U32(family_count))
      return false;

    for (uint32_t fi = 0; fi < family_count; ++fi) {
      uint8_t family_raw = 0;
      if (!reader.U8(family_raw))
        return false;
      const auto family = static_cast<TpchQ19Family>(family_raw);
      int idx = TpchQ19State::FamilyIndex(family);
      if (idx < 0 || idx >= TpchQ19State::kNumFamilies) return false;

      uint32_t size_count = 0;
      if (!reader.U32(size_count))
        return false;

      for (uint32_t si = 0; si < size_count; ++si) {
        int32_t size = 0;
        if (!reader.I32(size))
          return false;
        (void)size;  // size dummy, folded into family

        uint32_t qty_count = 0;
        if (!reader.U32(qty_count))
          return false;

        for (uint32_t qi = 0; qi < qty_count; ++qi) {
          int64_t qty = 0, rev = 0, count = 0;
          if (!reader.I64(qty) || !reader.I64(rev) || !reader.I64(count))
            return false;
          auto &vec = state.aggregates.Mut(brand)[idx];
          TpchQ19State::QtyRev qr{qty, rev, count};
          vec.insert(std::lower_bound(vec.begin(), vec.end(), qty,
                     [](const TpchQ19State::QtyRev &qr, int64_t q) { return qr.qty < q; }), qr);
        }
      }
    }
  }

  return true;
}

// ── State image helpers ─────────────────────────────────────────────────────
std::shared_ptr<const TpchQ19StateImage>
MakeStateImage(const std::shared_ptr<const TpchQ19State> &state) {
  auto image = std::make_shared<TpchQ19StateImage>();
  image->kind = TpchQ19ImageKind::Aggregate;
  image->state = state;
  return image;
}

const TpchQ19StateImage *
AsQ19Image(const core::StateImage &image, TpchQ19ImageKind kind) {
  const auto *payload = image.As<TpchQ19StateImage>();
  return payload && payload->kind == kind && payload->state ? payload : nullptr;
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

// ── Part index / dimension codes ────────────────────────────────────────────
std::vector<TpchQ19State::IndexEnt>
TpchQ19State::IndexEntries(int64_t partkey) const {
  std::vector<IndexEnt> out;
  if (const auto *ov = index_overlay.Find(partkey))
    return *ov;
  const auto &b = *index_base;
  const auto it = std::lower_bound(b.keys.begin(), b.keys.end(), partkey);
  if (it == b.keys.end() || *it != partkey)
    return out;
  const std::size_t i = static_cast<std::size_t>(it - b.keys.begin());
  for (uint32_t j = b.off[i]; j < b.off[i + 1]; ++j)
    out.push_back({b.ents[j].qty, b.ents[j].rev, b.ents[j].count});
  return out;
}

uint32_t TpchQ19State::DimGet(int64_t partkey) const {
  if (partkey >= 0 && partkey < kDimDenseLimit) {
    const std::size_t page = static_cast<std::size_t>(partkey) / kDimPage;
    if (page < dim_pages.size() && dim_pages[page])
      return dim_pages[page]->codes[static_cast<std::size_t>(partkey) % kDimPage];
    return 0u;
  }
  if (!dim_sparse)
    return 0u;
  const auto it = dim_sparse->find(partkey);
  return it == dim_sparse->end() ? 0u : it->second;
}

uint32_t TpchQ19State::EncodeDim(const std::string &brand, int family) {
  if (family < 1 || family > 3)
    return 1u;
  std::size_t i = 0;
  while (i < brands.size() && brands[i] != brand)
    ++i;
  if (i == brands.size())
    brands.push_back(brand);
  return (static_cast<uint32_t>(i + 1) << 2) | static_cast<uint32_t>(family);
}

void TpchQ19State::RemoveAggregate(const std::string &brand,
                                   TpchQ19Family family,
                                   int64_t quantity_coord, int64_t rev,
                                   int64_t count) {
  const int idx = FamilyIndex(family);
  if (idx < 0 || idx >= kNumFamilies)
    return;
  const BrandAgg *cur = aggregates.Find(brand);
  if (!cur)
    return;
  {
    const auto &cvec = (*cur)[idx];
    const auto cit = std::lower_bound(
        cvec.begin(), cvec.end(), quantity_coord,
        [](const QtyRev &qr, int64_t q) { return qr.qty < q; });
    if (cit == cvec.end() || cit->qty != quantity_coord)
      return;
  }
  auto &vec = aggregates.Mut(brand)[idx];
  auto it = std::lower_bound(vec.begin(), vec.end(), quantity_coord,
                             [](const QtyRev &qr, int64_t q) { return qr.qty < q; });
  it->rev -= rev;
  it->count -= count;
  if (it->count <= 0)
    vec.erase(it);
  // (An all-empty brand stays as an empty slot; it is skipped when encoded.)
}

namespace {

// Staging bookkeeping for copy-on-write dimension pages (fresh = Build).
struct DimStage {
  bool fresh = false;
  std::unordered_set<std::size_t> pages;
  bool sparse_owned = false;
};

void DimSet(TpchQ19State &s, DimStage &st, int64_t key, uint32_t code) {
  if (key >= 0 && key < TpchQ19State::kDimDenseLimit) {
    const std::size_t page = static_cast<std::size_t>(key) / TpchQ19State::kDimPage;
    if (s.dim_pages.size() <= page)
      s.dim_pages.resize(page + 1);
    auto &pp = s.dim_pages[page];
    if (!pp) {
      pp = std::make_shared<TpchQ19State::DimPage>();
      st.pages.insert(page);
    } else if (!st.fresh && st.pages.insert(page).second) {
      pp = std::make_shared<TpchQ19State::DimPage>(*pp);
    }
    pp->codes[static_cast<std::size_t>(key) % TpchQ19State::kDimPage] = code;
    return;
  }
  if (!st.fresh && !st.sparse_owned) {
    s.dim_sparse = std::make_shared<std::unordered_map<int64_t, uint32_t>>(
        s.dim_sparse ? *s.dim_sparse
                     : std::unordered_map<int64_t, uint32_t>{});
    st.sparse_owned = true;
  }
  if (code == 0)
    s.dim_sparse->erase(key);
  else
    (*s.dim_sparse)[key] = code;
}

// Dimension code of a part row under Q19's three-branch predicate.
uint32_t PartCode(TpchQ19State &s, const std::string &brand,
                  const std::string &container, int32_t size) {
  const auto family = ClassifyContainerFamily(container);
  const bool ok = family != TpchQ19Family::None && size >= 1 &&
                  size <= BranchSizeLimit(family);
  return s.EncodeDim(brand, ok ? static_cast<int>(family) : 0);
}

bool ReadPartRow(const core::AuxRow &row, int64_t &partkey, std::string &brand,
                 std::string &container, int32_t &size) {
  const std::string *b = StringCellPtr(row, 1);
  const std::string *c = StringCellPtr(row, 2);
  if (!IntCell(row, 0, partkey) || !b || !c || !Int32Cell(row, 3, size))
    return false;
  brand = *b;
  container = *c;
  return true;
}

} // namespace

// ── TxnBuffer ───────────────────────────────────────────────────────────────
bool TpchQ19TxnBuffer::HasPendingChanges() const { return pending; }

void TpchQ19TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
}

bool TpchQ19TxnBuffer::RequiresRebuild() const { return requires_rebuild; }

const std::vector<TpchQ19Delta> &TpchQ19TxnBuffer::Deltas() const {
  return deltas;
}

void TpchQ19TxnBuffer::MarkRebuild() { pending = requires_rebuild = true; }

void TpchQ19TxnBuffer::AddDelta(TpchQ19Delta delta) {
  pending = true;
  deltas.push_back(std::move(delta));
}

// ── Module methods ──────────────────────────────────────────────────────────
core::AcceleratorId TpchQ19Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ19Module::Dependencies() const {
  std::vector<core::TableDependency> deps;
  deps.push_back({config.lineitem_table,
                   {0, 1, 2, 3, 4, 5},
                   {1, 4, 5, 6, 14, 13}});
  deps.push_back({config.part_table, {0, 1, 2, 3}, {0, 3, 6, 5}});
  return deps;
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ19Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ19TxnBuffer>();
}

void TpchQ19Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  auto state = BuildState(services, true);
  const auto shared = std::make_shared<const TpchQ19State>(std::move(state));
  builder.Write(core::StateImage::Make(
      Id(), TpchQ19Layout::kAggregatePartition, 0,
      MakeStateImage(shared)));
  builder.Finish();
}

core::InvalidationPlan
TpchQ19Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table || mutation.table == config.part_table)
    return All();
  return {};
}

void TpchQ19Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &services,
                                       core::AuxTxnBuffer &txn_buf) {
  auto &buf = static_cast<TpchQ19TxnBuffer &>(txn_buf);

  if (mutation.table == config.lineitem_table) {
    // Deletes carry only old rows, inserts only new ones.
    const auto emit = [&](const core::AuxRow &row, int8_t sign) {
      int64_t partkey = 0, qty = 0, ext = 0, disc = 0;
      bool qualifies = false;
      if (!ReadLineitemRow(row, partkey, qty, ext, disc, qualifies)) {
        buf.MarkRebuild();
        return;
      }
      if (!qualifies) return;
      buf.AddDelta({partkey, qty, ComputeRevenue(ext, disc), sign});
    };
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows) emit(row, 1);
      break;
    case core::MutationKind::Delete:
      for (const auto &row : mutation.old_rows) emit(row, -1);
      break;
    case core::MutationKind::Update: {
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buf.MarkRebuild();
        break;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        emit(mutation.new_rows[i], 1);
        emit(mutation.old_rows[i], -1);
      }
      break;
    }
    }
    return;
  }

  if (mutation.table == config.part_table) {
    // Part writes move that part's qualifying lineitems (kept in the per-part
    // index) between (brand, family) aggregates.
    const auto make = [&](const core::AuxRow &row, int8_t op) {
      TpchQ19Delta d;
      d.part_op = op;
      if (!ReadPartRow(row, d.partkey, d.new_brand, d.new_container,
                       d.new_size)) {
        buf.MarkRebuild();
        return;
      }
      buf.AddDelta(std::move(d));
    };
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows) make(row, 1);
      break;
    case core::MutationKind::Delete:
      for (const auto &row : mutation.old_rows) make(row, 2);
      break;
    case core::MutationKind::Update:
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buf.MarkRebuild();
        break;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        int64_t ok = 0, nk = 0;
        std::string ob, oc, nb, nc;
        int32_t os = 0, ns = 0;
        if (!ReadPartRow(mutation.old_rows[i], ok, ob, oc, os) ||
            !ReadPartRow(mutation.new_rows[i], nk, nb, nc, ns) || ok != nk) {
          buf.MarkRebuild();
          break;
        }
        if (ob == nb && oc == nc && os == ns)
          continue; // untracked column only
        make(mutation.new_rows[i], 3);
      }
      break;
    }
    return;
  }
}

TpchQ19State TpchQ19Module::BuildState(core::AuxEngineServices &services,
                                        bool full) const {
  (void)services;
  TpchQ19State state;

  if (!initial_scan)
    throw std::logic_error("TPC-H Q19 aux requires a typed initial scan source");

  const auto family_of_index = [](int f) {
    return static_cast<TpchQ19Family>(f);
  };

  if (!full) {
    // Read-only fallback: aggregates only.
    if (initial_scan->ScanQualifyingAggregates(
            [&](const std::string &brand, int family, int64_t qty, int64_t rev,
                int64_t count) {
              state.AddAggregate(brand, family_of_index(family), qty, rev,
                                 count);
            }))
      return state;
    std::unordered_map<int64_t, TpchQ19PartInput> parts;
    initial_scan->ScanParts([&](const TpchQ19PartInput &input) {
      parts[input.key] = input;
    });
    initial_scan->ScanLineitems([&](const TpchQ19LineitemInput &input) {
      if (!ShippingQualifies(input))
        return;
      const auto part_it = parts.find(input.partkey);
      if (part_it == parts.end())
        return;
      const auto &part = part_it->second;
      const auto family = ClassifyContainerFamily(part.container);
      if (family == TpchQ19Family::None)
        return;
      state.AddContribution(part.brand, family, part.size, input.quantity_raw,
                            ComputeRevenue(input.extendedprice_raw,
                                           input.discount_raw));
    });
    return state;
  }

  // Part dimension codes for every part.
  DimStage dim_stage;
  dim_stage.fresh = true;
  if (!initial_scan->ScanPartFamilies(
          [&](int64_t key, const std::string &brand, int family) {
            DimSet(state, dim_stage, key, state.EncodeDim(brand, family));
          })) {
    initial_scan->ScanParts([&](const TpchQ19PartInput &input) {
      DimSet(state, dim_stage, input.key,
             PartCode(state, input.brand, input.container, input.size));
    });
  }

  // Per-part index; each closed part also feeds the (brand, family, qty)
  // aggregates under its current qualification.
  auto base = std::make_shared<TpchQ19State::IndexBase>();
  base->off.push_back(0);
  std::vector<TpchQ19State::IndexEnt> pending;
  int64_t cur_key = 0;
  bool has_cur = false;
  const auto close_part = [&]() {
    if (!has_cur)
      return;
    std::sort(pending.begin(), pending.end(),
              [](const auto &a, const auto &b) { return a.qty < b.qty; });
    std::vector<TpchQ19State::IndexEnt> merged;
    for (const auto &e : pending) {
      if (!merged.empty() && merged.back().qty == e.qty) {
        merged.back().rev += e.rev;
        merged.back().count += e.count;
      } else {
        merged.push_back(e);
      }
    }
    pending.clear();
    has_cur = false;
    const uint32_t code = state.DimGet(cur_key);
    if (code >= 4) {
      const auto family = static_cast<TpchQ19Family>(code & 3u);
      const std::string &brand = state.brands[(code >> 2) - 1];
      for (const auto &e : merged)
        state.AddAggregate(brand, family, e.qty, e.rev, e.count);
    }
    bool fits = true;
    for (const auto &e : merged)
      if (e.qty < std::numeric_limits<int32_t>::min() ||
          e.qty > std::numeric_limits<int32_t>::max() ||
          e.count > std::numeric_limits<int32_t>::max())
        fits = false;
    if (!fits) {
      state.index_overlay.Upsert(cur_key, std::move(merged));
      return;
    }
    base->keys.push_back(cur_key);
    for (const auto &e : merged)
      base->ents.push_back({e.rev, static_cast<int32_t>(e.count),
                            static_cast<int32_t>(e.qty)});
    base->off.push_back(static_cast<uint32_t>(base->ents.size()));
  };
  const auto feed = [&](int64_t partkey, int64_t qty, int64_t rev,
                        int64_t count) {
    if (has_cur && partkey != cur_key) {
      if (partkey < cur_key)
        throw std::logic_error("TPC-H Q19 aux: lineitem scan not ordered");
      close_part();
    }
    cur_key = partkey;
    has_cur = true;
    pending.push_back({qty, rev, count});
  };
  if (!initial_scan->ScanQualifyingLineitemsByPart(feed)) {
    struct Row {
      int64_t partkey, qty, rev;
    };
    std::vector<Row> rows;
    initial_scan->ScanLineitems([&](const TpchQ19LineitemInput &input) {
      if (ShippingQualifies(input))
        rows.push_back({input.partkey, input.quantity_raw,
                        ComputeRevenue(input.extendedprice_raw,
                                       input.discount_raw)});
    });
    std::stable_sort(rows.begin(), rows.end(),
                     [](const Row &a, const Row &b) { return a.partkey < b.partkey; });
    for (const auto &r : rows)
      feed(r.partkey, r.qty, r.rev, 1);
  }
  close_part();
  state.index_base = std::move(base);
  return state;
}

bool TpchQ19Module::LoadState(
    core::AuxEngineServices &services,
    std::shared_ptr<const TpchQ19State> &state) const {
  CustomTimer t_load(CustomProfilingEnabled());
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {TpchQ19Layout::kAggregatePartition});
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ19Layout::kAggregatePartition) {
      const auto *typed = AsQ19Image(image, TpchQ19ImageKind::Aggregate);
      if (!typed)
        return false;
      state = typed->state;
      EmitCustomProfile("load_state", t_load.us());
      return true;
    }
  }
  EmitCustomProfile("load_state", t_load.us());
  return false;
}

void TpchQ19Module::EmitResult(const TpchQ19State &state,
                                const std::string &brand1, int64_t qty_low1,
                                const std::string &brand2, int64_t qty_low2,
                                const std::string &brand3, int64_t qty_low3,
                                core::AuxRowSink &sink) const {
  const int64_t qty_high1 = qty_low1 + 1000;
  const int64_t qty_high2 = qty_low2 + 1000;
  const int64_t qty_high3 = qty_low3 + 1000;

  int64_t c1 = 0, c2 = 0, c3 = 0;
  const int64_t total_raw =
      state.RangeSumCount(brand1, TpchQ19Family::SM, qty_low1, qty_high1, c1) +
      state.RangeSumCount(brand2, TpchQ19Family::MED, qty_low2, qty_high2, c2) +
      state.RangeSumCount(brand3, TpchQ19Family::LG, qty_low3, qty_high3, c3);
  const int64_t matches = c1 + c2 + c3;

  sink.Begin({{{"revenue", core::AuxType::ScaledDecimal, true}}});
  if (matches == 0) {
    // SUM over no rows is NULL.
    sink.WriteRow({{core::AuxValue::Null(core::AuxType::ScaledDecimal)}});
  } else {
    sink.WriteRow({{core::AuxValue::Of(core::AuxType::ScaledDecimal,
                                        core::ScaledDecimal{total_raw, 4})}});
  }
  sink.End();
}

core::StagedMaintenanceOutcome
TpchQ19Module::StageMaintenance(core::AuxEngineServices &services,
                                const core::InvalidationPlan &plan,
                                core::AuxTxnBuffer &txn_buf) {
  (void)plan;
  CustomTimer t_sm(CustomProfilingEnabled());
  auto &buf = static_cast<TpchQ19TxnBuffer &>(txn_buf);
  core::StagedMaintenanceOutcome outcome;

  if (!buf.HasPendingChanges()) {
    outcome.action = core::MaintenanceAction::NoOp;
    EmitCustomProfile("stage_maintenance_noop", t_sm.us());
    return outcome;
  }

  if (buf.RequiresRebuild()) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    EmitCustomProfile("stage_maintenance_rebuild", t_sm.us());
    return outcome;
  }

  const auto &deltas = buf.Deltas();
  if (deltas.empty()) {
    // Only non-qualifying lineitems changed: the aggregate is unaffected.
    outcome.action = core::MaintenanceAction::NoOp;
    EmitCustomProfile("stage_maintenance_noop", t_sm.us());
    return outcome;
  }

  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(deltas.size()), 1, true, false, true, false,
       false, core::MaintenanceAction::NoOp,
       services.ExpectedQueriesBeforeNextWrite()});
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    outcome.action =
        decision.action == core::MaintenanceAction::RebuildPartition
            ? core::MaintenanceAction::RebuildAll
            : decision.action;
    EmitCustomProfile("stage_maintenance_rebuild_selected", t_sm.us());
    return outcome;
  }

  // Copy-on-write per commit: a published version is immutable (readers and
  // older snapshots keep it; an aborted commit must leave it unchanged).
  // The state is small (brands x families x quantities).
  std::shared_ptr<const TpchQ19State> current_state;
  if (!LoadState(services, current_state) || !current_state) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    EmitCustomProfile("stage_maintenance_rebuild_noload", t_sm.us());
    return outcome;
  }
  // Index/dimension overlays are bounded: past this, a fresh bulk build is
  // the compaction.
  constexpr std::size_t kMaxIndexOverlay = 300000;
  if (current_state->index_overlay.size() + deltas.size() > kMaxIndexOverlay) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    return outcome;
  }

  std::shared_ptr<TpchQ19State> next;
  next = std::make_shared<TpchQ19State>(*current_state);
  DimStage dim_stage;
  const auto rebuild = [&]() {
    outcome = core::StagedMaintenanceOutcome{};
    outcome.action = core::MaintenanceAction::RebuildAll;
    return outcome;
  };

  const auto mutable_index = [&](int64_t part) -> std::vector<TpchQ19State::IndexEnt> & {
    if (next->index_overlay.Find(part))
      return next->index_overlay[part];
    auto init = next->IndexEntries(part);
    return next->index_overlay.Upsert(part, std::move(init));
  };
  // Adds (+1) / removes (-1) all of a part's indexed lineitems to / from the
  // aggregates under the given dimension code.
  const auto apply_part = [&](int64_t part, uint32_t code, int sign) {
    if (code < 4)
      return;
    const auto family = static_cast<TpchQ19Family>(code & 3u);
    const std::string brand = next->brands[(code >> 2) - 1];
    for (const auto &e : next->IndexEntries(part)) {
      if (sign > 0)
        next->AddAggregate(brand, family, e.qty, e.rev, e.count);
      else
        next->RemoveAggregate(brand, family, e.qty, e.rev, e.count);
    }
  };

  for (const auto &delta : deltas) {
    if (delta.part_op == 0) {
      auto &vec = mutable_index(delta.partkey);
      auto it = std::lower_bound(
          vec.begin(), vec.end(), delta.quantity_raw,
          [](const TpchQ19State::IndexEnt &e, int64_t q) { return e.qty < q; });
      const bool found = it != vec.end() && it->qty == delta.quantity_raw;
      if (delta.sign > 0) {
        if (found) {
          it->rev += delta.revenue_raw;
          it->count += 1;
        } else {
          vec.insert(it, {delta.quantity_raw, delta.revenue_raw, 1});
        }
      } else {
        if (!found || it->count <= 0)
          return rebuild(); // index disagrees with the table
        it->rev -= delta.revenue_raw;
        it->count -= 1;
        if (it->count == 0)
          vec.erase(it);
      }
      const uint32_t code = next->DimGet(delta.partkey);
      if (code >= 4) {
        const auto family = static_cast<TpchQ19Family>(code & 3u);
        const std::string &brand = next->brands[(code >> 2) - 1];
        if (delta.sign > 0)
          next->AddAggregate(brand, family, delta.quantity_raw,
                             delta.revenue_raw, 1);
        else
          next->RemoveAggregate(brand, family, delta.quantity_raw,
                                delta.revenue_raw, 1);
      }
      continue;
    }

    const uint32_t old_code = next->DimGet(delta.partkey);
    if (delta.part_op == 1) { // insert
      if (old_code != 0)
        return rebuild(); // duplicate key
      const uint32_t code =
          PartCode(*next, delta.new_brand, delta.new_container, delta.new_size);
      DimSet(*next, dim_stage, delta.partkey, code);
      apply_part(delta.partkey, code, +1);
    } else if (delta.part_op == 2) { // delete
      if (old_code == 0)
        return rebuild(); // missing key
      apply_part(delta.partkey, old_code, -1);
      DimSet(*next, dim_stage, delta.partkey, 0u);
    } else { // update of brand / container / size
      if (old_code == 0)
        return rebuild();
      const uint32_t code =
          PartCode(*next, delta.new_brand, delta.new_container, delta.new_size);
      if (code != old_code) {
        apply_part(delta.partkey, old_code, -1);
        DimSet(*next, dim_stage, delta.partkey, code);
        apply_part(delta.partkey, code, +1);
      }
    }
  }

  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.updates.push_back(core::StateImage::Make(
      Id(), TpchQ19Layout::kAggregatePartition, 0,
      MakeStateImage(std::static_pointer_cast<const TpchQ19State>(next))));
  EmitCustomProfile("stage_maintenance_delta", t_sm.us());
  return outcome;
}

bool TpchQ19Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &) const {
  CustomTimer t_check(CustomProfilingEnabled());
  bool valid = services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
  EmitCustomProfile("check_validity", t_check.us());
  return valid;
}

void TpchQ19Module::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  CustomTimer t_exec(CustomProfilingEnabled());
  if (arguments.values.size() < 6) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  for (std::size_t i = 0; i < 6; ++i) {
    if (arguments.values[i].is_null) {
      ExecuteFallback(services, arguments, sink);
      return;
    }
  }
  for (std::size_t i = 0; i < 6; ++i) {
    if (i % 2 == 0) {
      if (!std::holds_alternative<std::string>(arguments.values[i].payload)) {
        ExecuteFallback(services, arguments, sink);
        return;
      }
    } else if (!std::holds_alternative<int64_t>(arguments.values[i].payload)) {
      ExecuteFallback(services, arguments, sink);
      return;
    }
  }

  const double parse_start = t_exec.us();
  // Stack extraction avoids std::vector heap allocations on the hot path.
  const auto &brand1 = std::get<std::string>(arguments.values[0].payload);
  const auto &brand2 = std::get<std::string>(arguments.values[2].payload);
  const auto &brand3 = std::get<std::string>(arguments.values[4].payload);
  const int64_t qty1 = std::get<int64_t>(arguments.values[1].payload) * 100;
  const int64_t qty2 = std::get<int64_t>(arguments.values[3].payload) * 100;
  const int64_t qty3 = std::get<int64_t>(arguments.values[5].payload) * 100;
  EmitCustomProfile("execute_parse", t_exec.us() - parse_start);

  std::shared_ptr<const TpchQ19State> state;
  const auto visibility = services.CurrentVisibility();
  const auto validity = services.StateStore().CheckValidity({Id(), {}}, visibility);
  if (!validity.IsValid()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  // Immutable (state, version) pair swapped atomically: safe for concurrent
  // readers and the writer's commits.
  const std::shared_ptr<const CachedRead> cached = std::atomic_load(&cached_read_);
  if (cached && cached->version == validity.visible_version) {
    state = cached->state;
  } else {
    if (!LoadState(services, state)) {
      ExecuteFallback(services, arguments, sink);
      return;
    }
    auto fresh = std::make_shared<CachedRead>();
    fresh->state = state;
    fresh->version = validity.visible_version;
    std::atomic_store(&cached_read_,
                      std::static_pointer_cast<const CachedRead>(fresh));
  }

  double emit_start = t_exec.us();
  EmitResult(*state, brand1, qty1, brand2, qty2, brand3, qty3, sink);
  EmitCustomProfile("execute_emit", t_exec.us() - emit_start);
  EmitCustomProfile("execute_total", t_exec.us());
}

void TpchQ19Module::ExecuteFallback(core::AuxEngineServices &services,
                                    const core::QueryArguments &arguments,
                                    core::AuxRowSink &sink) {
  (void)services;
  auto state = BuildState(services, false);

  if (arguments.values.size() < 6) {
    sink.Begin({{{"revenue", core::AuxType::ScaledDecimal, true}}});
    sink.WriteRow({{core::AuxValue::Null(core::AuxType::ScaledDecimal)}});
    sink.End();
    return;
  }

  std::vector<int64_t> args_int;
  std::vector<std::string> args_str;
  for (std::size_t i = 0; i < 6; ++i) {
    if (arguments.values[i].is_null) {
      args_int.push_back(0);
      args_str.push_back("");
      continue;
    }
    if (i % 2 == 0) {
      if (std::holds_alternative<std::string>(arguments.values[i].payload))
        args_str.push_back(std::get<std::string>(arguments.values[i].payload));
      else
        args_str.push_back("");
    } else {
      if (std::holds_alternative<int64_t>(arguments.values[i].payload))
        args_int.push_back(std::get<int64_t>(arguments.values[i].payload));
      else
        args_int.push_back(0);
    }
  }

  const int64_t qty1 = args_int[0] * 100;
  const int64_t qty2 = args_int[1] * 100;
  const int64_t qty3 = args_int[2] * 100;

  EmitResult(state, args_str[0], qty1, args_str[1], qty2, args_str[2], qty3,
             sink);
}

std::vector<core::CalibrationWrite> TpchQ19Module::CalibrationWrites() const {
  return TpchQ19CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
