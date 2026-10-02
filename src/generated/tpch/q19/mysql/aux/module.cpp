#include "tpch/q19/mysql/aux/module.hpp"
#include "tpch/q19/calibration_writes.hpp"
#include "accelerator_codegen/stateful/core/byte_stream.hpp"

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
  explicit CustomTimer(bool enabled) : start(Clock::now()), active(enabled) {}
  double us() const {
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

bool ReadPartRow(const core::AuxRow &row, TpchQ19PartInput &out) {
  int64_t partkey = 0;
  std::string brand, container;
  int32_t size = 0;
  if (!IntCell(row, 0, partkey) || !StringCell(row, 1, brand) ||
      !StringCell(row, 2, container) || !Int32Cell(row, 3, size))
    return false;
  out.key = partkey;
  out.brand = std::move(brand);
  out.container = std::move(container);
  out.size = size;
  return true;
}

bool ReadLineitemRow(const core::AuxRow &row, TpchQ19LineitemInput &out) {
  int64_t partkey = 0, quantity_raw = 0, extendedprice_raw = 0, discount_raw = 0;
  int32_t qty_scale = 0, ext_scale = 0, disc_scale = 0;
  std::string shipmode, shipinstruct;
  if (!IntCell(row, 0, partkey) ||
      !RawDecimalCell(row, 1, quantity_raw, qty_scale) ||
      !RawDecimalCell(row, 2, extendedprice_raw, ext_scale) ||
      !RawDecimalCell(row, 3, discount_raw, disc_scale) ||
      !StringCell(row, 4, shipmode) ||
      !StringCell(row, 5, shipinstruct))
    return false;
  if (qty_scale != 2 || ext_scale != 2 || disc_scale != 2)
    return false;
  out.partkey = partkey;
  out.quantity_raw = quantity_raw;
  out.extendedprice_raw = extendedprice_raw;
  out.discount_raw = discount_raw;
  out.shipmode = std::move(shipmode);
  out.shipinstruct = std::move(shipinstruct);
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
  auto &vec = aggregates[brand][idx];
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

int64_t TpchQ19State::RangeCount(const std::string &brand, TpchQ19Family family,
                                 int64_t qty_low, int64_t qty_high) const {
  const int idx = FamilyIndex(family);
  if (idx < 0 || idx >= kNumFamilies) return 0;
  const auto brand_it = aggregates.find(brand);
  if (brand_it == aggregates.end()) return 0;
  const auto &vec = brand_it->second[idx];
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
  CustomTimer t_rs(CustomProfilingEnabled());
  (void)size_low; (void)size_high;  // size folded into family
  int idx = FamilyIndex(family);
  if (idx < 0 || idx >= kNumFamilies) return 0;
  const auto brand_it = aggregates.find(brand);
  if (brand_it == aggregates.end()) return 0;
  const auto &vec = brand_it->second[idx];
  if (vec.empty()) return 0;
  int64_t total = 0;
  auto qty_it = std::lower_bound(vec.begin(), vec.end(), qty_low,
                                  [](const QtyRev &qr, int64_t q) { return qr.qty < q; });
  auto qty_end = std::upper_bound(vec.begin(), vec.end(), qty_high,
                                   [](int64_t q, const QtyRev &qr) { return q < qr.qty; });
  for (auto it = qty_it; it != qty_end; ++it)
    total += it->rev;
  EmitCustomProfile2("range_sum", t_rs.us(), 0, total);
  return total;
}

void TpchQ19State::RemoveContribution(const std::string &brand,
                                       TpchQ19Family family, int32_t size,
                                       int64_t quantity_coord, int64_t delta) {
  if (size < 1 || size > BranchSizeLimit(family)) return;
  int idx = FamilyIndex(family);
  if (idx < 0 || idx >= kNumFamilies) return;
  auto brand_it = aggregates.find(brand);
  if (brand_it == aggregates.end()) return;
  auto &vec = brand_it->second[idx];
  auto qty_it = std::lower_bound(vec.begin(), vec.end(), quantity_coord,
                                  [](const QtyRev &qr, int64_t q) { return qr.qty < q; });
  if (qty_it == vec.end() || qty_it->qty != quantity_coord) return;
  qty_it->rev -= delta;
  qty_it->count -= 1;
  if (qty_it->count <= 0)
    vec.erase(qty_it);
  // Look for empty slots to maybe erase this brand key
  bool all_empty = true;
  for (int fi = 0; fi < kNumFamilies; ++fi) {
    if (!brand_it->second[fi].empty()) { all_empty = false; break; }
  }
  if (all_empty)
    aggregates.erase(brand_it);
}






// ── Encode / Decode ─────────────────────────────────────────────────────────
std::vector<uint8_t> EncodeTpchQ19Aggregate(const TpchQ19State &state) {
  ByteWriter w;
  core::WriteImageHeader(w, kAggregateMagic, kVersionV2, 0); // count unused

  // brand count
  w.U32(static_cast<uint32_t>(state.aggregates.size()));
  for (const auto &[brand, family_vecs] : state.aggregates) {
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
  }

  return std::move(w.bytes);
}

bool DecodeTpchQ19Aggregate(const std::vector<uint8_t> &bytes,
                            TpchQ19State &state) {
  ByteReader reader(bytes);
  uint32_t magic = 0;
  uint32_t version = 0;
  uint32_t unused_count = 0;
  if (!reader.U32(magic) || !reader.U32(version) || !reader.U32(unused_count))
    return false;
  if (magic != kAggregateMagic) return false;
  if (version != kVersionV2 && version != kVersionV1_Deprecated) return false;

  state.aggregates.clear();

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
          auto &vec = state.aggregates[brand][idx];
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
  auto state = BuildState(services);
  const auto shared = std::make_shared<const TpchQ19State>(std::move(state));
  builder.Write(core::StateImage::Make(
      Id(), TpchQ19Layout::kAggregatePartition, 0,
      MakeStateImage(shared)));
  // Pre-initialize mutable active state during Build to avoid deep copy on first write.
  active_state_ = std::make_shared<TpchQ19State>(*shared);
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
    for (std::size_t i = 0; i < mutation.new_rows.size(); ++i) {
      if (mutation.kind == core::MutationKind::Insert) {
        TpchQ19Delta d;
        ReadLineitemRow(mutation.new_rows[i], d.new_line);
        d.new_line_valid = ShippingQualifies(d.new_line);
        buf.AddDelta(std::move(d));
} else if (mutation.kind == core::MutationKind::Delete) {
        TpchQ19Delta d;
        ReadLineitemRow(mutation.old_rows[i], d.old_line);
        if (ShippingQualifies(d.old_line))
          d.old_line_valid = true;
        buf.AddDelta(std::move(d));
      } else if (mutation.kind == core::MutationKind::Update) {
        TpchQ19Delta d;
        if (i < mutation.old_rows.size())
          ReadLineitemRow(mutation.old_rows[i], d.old_line);
        if (i < mutation.new_rows.size())
          ReadLineitemRow(mutation.new_rows[i], d.new_line);
        d.old_line_valid = ShippingQualifies(d.old_line);
        d.new_line_valid = ShippingQualifies(d.new_line);
        buf.AddDelta(std::move(d));
      }
    }
    return;
  }

  if (mutation.table == config.part_table) {
    for (std::size_t i = 0; i < mutation.new_rows.size(); ++i) {
      if (mutation.kind == core::MutationKind::Insert) {
        TpchQ19Delta d;
        ReadPartRow(mutation.new_rows[i], d.new_part);
        buf.AddDelta(std::move(d));
      } else if (mutation.kind == core::MutationKind::Delete) {
        TpchQ19Delta d;
        ReadPartRow(mutation.old_rows[i], d.old_part);
        buf.AddDelta(std::move(d));
      } else if (mutation.kind == core::MutationKind::Update) {
        TpchQ19Delta d;
        if (i < mutation.old_rows.size())
          ReadPartRow(mutation.old_rows[i], d.old_part);
        if (i < mutation.new_rows.size())
          ReadPartRow(mutation.new_rows[i], d.new_part);
        d.part_update = true;
        buf.AddDelta(std::move(d));
      }
    }
    return;
  }
}

TpchQ19State TpchQ19Module::BuildState(core::AuxEngineServices &services) const {
  (void)services;
  TpchQ19State state;

  if (!initial_scan)
    throw std::logic_error("TPC-H Q19 aux requires a typed initial scan source");

  // First scan parts to build dimension cache
  std::unordered_map<int64_t, TpchQ19PartInput> parts;
  initial_scan->ScanParts([&](const TpchQ19PartInput &input) {
    parts[input.key] = input;
    TpchQ19PartDim dim;
    dim.brand = input.brand;
    dim.container = input.container;
    dim.size = input.size;
    dim.family = ClassifyContainerFamily(input.container);
    part_dim_cache[input.key] = std::move(dim);
  });

  // Then scan lineitems, filtering by shipping pred and joining to part
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

    const int64_t revenue = ComputeRevenue(input.extendedprice_raw, input.discount_raw);

    state.AddContribution(part.brand, family, part.size, input.quantity_raw, revenue);

  });

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

  int64_t total_raw = 0;
  total_raw +=
      state.RangeSum(brand1, TpchQ19Family::SM, 1, 5, qty_low1, qty_high1);
  total_raw +=
      state.RangeSum(brand2, TpchQ19Family::MED, 1, 10, qty_low2, qty_high2);
  total_raw +=
      state.RangeSum(brand3, TpchQ19Family::LG, 1, 15, qty_low3, qty_high3);

  const int64_t matches = state.RangeCount(brand1, TpchQ19Family::SM, qty_low1, qty_high1) +
                          state.RangeCount(brand2, TpchQ19Family::MED, qty_low2, qty_high2) +
                          state.RangeCount(brand3, TpchQ19Family::LG, qty_low3, qty_high3);

  sink.Begin({{{"revenue", core::AuxType::ScaledDecimal, true}}});
  if (matches == 0) {
    // The reference is COALESCE(SUM(...), 0): 0 when nothing matches.
    sink.WriteRow({{core::AuxValue::Of(core::AuxType::ScaledDecimal, core::ScaledDecimal{0, 4})}});
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
    active_state_.reset();
    outcome.action = core::MaintenanceAction::RebuildAll;
    EmitCustomProfile("stage_maintenance_rebuild", t_sm.us());
    return outcome;
  }

  // Try delta application
  const auto &deltas = buf.Deltas();
  bool can_delta = !deltas.empty();

  if (can_delta) {
    const auto decision = services.ChooseMaintenance(
        {static_cast<uint64_t>(deltas.size()), 1, true, false, true, false,
         false, core::MaintenanceAction::NoOp,
         services.ExpectedQueriesBeforeNextWrite()});
    if (decision.action != core::MaintenanceAction::ApplyDelta) {
      active_state_.reset();
      outcome.action =
          decision.action == core::MaintenanceAction::RebuildPartition
              ? core::MaintenanceAction::RebuildAll
              : decision.action;
      EmitCustomProfile("stage_maintenance_rebuild_selected", t_sm.us());
      return outcome;
    }
    // Ensure active state is loaded (lazy init, zero-copy from now on)
    if (!active_state_) {
      std::shared_ptr<const TpchQ19State> current_state;
      if (!LoadState(services, current_state) || !current_state) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        EmitCustomProfile("stage_maintenance_rebuild_noload", t_sm.us());
        return outcome;
      }
      active_state_ = std::make_shared<TpchQ19State>(*current_state);
    }

    auto &s = *active_state_;

    // ── Handle part dimension cache maintenance ────────────────────
    for (const auto &delta : deltas) {
      // Part insert: populate cache
      if (delta.new_part.key != 0 && !delta.part_update && !delta.lineitem_update && !delta.new_line_valid && !delta.old_line_valid) {
        TpchQ19PartDim dim;
        dim.brand = delta.new_part.brand;
        dim.container = delta.new_part.container;
        dim.size = delta.new_part.size;
        dim.family = ClassifyContainerFamily(delta.new_part.container);
        part_dim_cache[delta.new_part.key] = std::move(dim);
      }
      // Part delete: remove from cache
      if (delta.old_part.key != 0 && !delta.part_update && !delta.lineitem_update && !delta.new_line_valid && !delta.old_line_valid) {
        part_dim_cache.erase(delta.old_part.key);
      }
    }

    // ── Handle part updates first ──────────────────────────────────
    for (const auto &delta : deltas) {
      if (!delta.part_update) continue;

      const TpchQ19Family new_family = ClassifyContainerFamily(delta.new_part.container);
      const TpchQ19Family old_family = ClassifyContainerFamily(delta.old_part.container);
      
      // Update part dimension cache
      TpchQ19PartDim new_dim;
      new_dim.brand = delta.new_part.brand;
      new_dim.container = delta.new_part.container;
      new_dim.size = delta.new_part.size;
      new_dim.family = new_family;
      part_dim_cache[delta.new_part.key] = new_dim;

    }

    // ── Helper to resolve part dimensions ─────────────────────────
    auto resolve_part_dim = [&](int64_t partkey) -> const TpchQ19PartDim* {
      auto it = part_dim_cache.find(partkey);
      if (it != part_dim_cache.end()) return &it->second;
      if (!initial_scan) return nullptr;
      TpchQ19PartInput part;
      if (!initial_scan->LookupPart(partkey, part)) return nullptr;
      TpchQ19PartDim dim;
      dim.brand = part.brand;
      dim.container = part.container;
      dim.size = part.size;
      dim.family = ClassifyContainerFamily(part.container);
      auto [ins, _] = part_dim_cache.emplace(partkey, std::move(dim));
      return &ins->second;
    };

    // ── Handle lineitem deltas ────────────────────────────────────
    for (const auto &delta : deltas) {
      if (delta.part_update) continue;
      // Skip pure part inserts/deletes (no lineitem data)
      if (!delta.lineitem_update && !delta.new_line_valid && !delta.old_line_valid) continue;

      if (delta.new_line_valid) {
        if (const auto *dim = resolve_part_dim(delta.new_line.partkey)) {
          if (dim->family != TpchQ19Family::None) {
            const int64_t revenue =
                ComputeRevenue(delta.new_line.extendedprice_raw,
                               delta.new_line.discount_raw);
            s.AddContribution(dim->brand, dim->family, dim->size,
                                       delta.new_line.quantity_raw, revenue);
          }
        }
      }

      if (delta.old_line_valid) {
        if (const auto *dim = resolve_part_dim(delta.old_line.partkey)) {
          if (dim->family != TpchQ19Family::None) {
            const int64_t revenue =
                ComputeRevenue(delta.old_line.extendedprice_raw,
                               delta.old_line.discount_raw);
            s.RemoveContribution(dim->brand, dim->family, dim->size,
                                          delta.old_line.quantity_raw,
                                          revenue);
          }
        }
      }
    }

    // Publish updated state as const shared_ptr
    outcome.action = core::MaintenanceAction::ApplyDelta;
    outcome.updates.push_back(core::StateImage::Make(
        Id(), TpchQ19Layout::kAggregatePartition, 0,
        MakeStateImage(active_state_)));
    EmitCustomProfile("stage_maintenance_delta", t_sm.us());
    return outcome;
  }

  outcome.action = core::MaintenanceAction::RebuildAll;
  EmitCustomProfile("stage_maintenance_fallback", t_sm.us());
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
    cached_state_.reset();
    cached_partition_version_ = 0;
    ExecuteFallback(services, arguments, sink);
    return;
  }
  if (cached_state_ && cached_partition_version_ == validity.visible_version) {
    // State was valid at this partition version and the version has not
    // changed. Skip ReadPartitions slot scan; immutable state is safe.
    state = cached_state_;
    EmitCustomProfile("load_state_cached", 0.0);
  } else {
    if (!LoadState(services, state)) {
      cached_state_.reset();
      cached_partition_version_ = 0;
      ExecuteFallback(services, arguments, sink);
      return;
    }
    cached_state_ = state;
    cached_partition_version_ = validity.visible_version;
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
  auto state = BuildState(services);

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
