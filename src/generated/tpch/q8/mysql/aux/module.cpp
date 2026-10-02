#include "tpch/q8/mysql/aux/module.hpp"
#include "tpch/calibration_writes.hpp"
#include "accelerator_codegen/stateful/core/byte_stream.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
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

constexpr uint32_t kCubeMagic = 0x51385143; // Q8C
constexpr uint32_t kVersionV1 = 1;

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
  if (std::holds_alternative<core::AuxDate>(cell.payload)) {
    out = std::get<core::AuxDate>(cell.payload).days_since_epoch;
    return true;
  }
  return false;
}

bool Int32Cell(const core::AuxRow &row, std::size_t index, int32_t &out) {
  int64_t value = 0;
  if (!IntCell(row, index, value) ||
      value < std::numeric_limits<int32_t>::min() ||
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

bool ReadLineitem(const core::AuxRow &row, TpchQ8LineitemInput &out) {
  int64_t partkey = 0, orderkey = 0, suppkey = 0, extended_raw = 0, discount_raw = 0;
  int32_t scale = 0;
  if (!IntCell(row, 0, partkey) ||
      !IntCell(row, 1, orderkey) ||
      !IntCell(row, 2, suppkey) ||
      !RawDecimalCell(row, 3, extended_raw, scale) ||
      !RawDecimalCell(row, 4, discount_raw, scale))
    return false;
  out.partkey = partkey;
  out.orderkey = orderkey;
  out.suppkey = suppkey;
  out.extendedprice_raw = extended_raw;
  out.discount_raw = discount_raw;
  return true;
}

bool ReadPart(const core::AuxRow &row, int64_t &key, std::string &type) {
  int64_t k = 0;
  if (!IntCell(row, 0, k) || !StringCell(row, 1, type))
    return false;
  key = k;
  return true;
}

bool ReadSupplier(const core::AuxRow &row, int64_t &key, int32_t &nationkey) {
  int64_t k = 0;
  if (!IntCell(row, 0, k) || !Int32Cell(row, 1, nationkey))
    return false;
  key = k;
  return true;
}

bool ReadCustomer(const core::AuxRow &row, int64_t &key, int32_t &nationkey) {
  int64_t k = 0;
  if (!IntCell(row, 0, k) || !Int32Cell(row, 1, nationkey))
    return false;
  key = k;
  return true;
}

bool ReadOrders(const core::AuxRow &row, int64_t &key, int64_t &custkey,
                int32_t &orderdate_days) {
  int64_t k = 0, ck = 0;
  if (!IntCell(row, 0, k) || !IntCell(row, 1, ck) || !Int32Cell(row, 2, orderdate_days))
    return false;
  key = k;
  custkey = ck;
  return true;
}

bool ReadNation(const core::AuxRow &row, int32_t &key, std::string &name,
                int32_t &regionkey) {
  if (!Int32Cell(row, 0, key) || !StringCell(row, 1, name) ||
      !Int32Cell(row, 2, regionkey))
    return false;
  return true;
}

bool ReadRegion(const core::AuxRow &row, int32_t &key, std::string &name) {
  if (!Int32Cell(row, 0, key) || !StringCell(row, 1, name))
    return false;
  return true;
}

// ── Image helpers ───────────────────────────────────────────────────────────
std::shared_ptr<const TpchQ8StateImage>
MakeStateImage(TpchQ8ImageKind kind, int32_t year,
               const std::shared_ptr<const TpchQ8State> &state) {
  auto image = std::make_shared<TpchQ8StateImage>();
  image->kind = kind;
  image->year = year;
  image->state = state;
  return image;
}

const TpchQ8StateImage *AsQ8Image(const core::StateImage &image,
                                  TpchQ8ImageKind kind) {
  const auto *payload = image.As<TpchQ8StateImage>();
  return payload && payload->kind == kind && payload->state ? payload : nullptr;
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

core::InvalidationPlan PartitionSet(std::vector<core::PartitionId> partitions) {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
  for (const auto partition : partitions)
    plan.partitions.insert(partition);
  return plan;
}

// ── Dense cube helpers ─────────────────────────────────────────────────────
struct TpchQ8CubeCellLess {
  bool operator()(const TpchQ8CubeCell &a, const TpchQ8CubeCell &b) const {
    if (a.year != b.year) return a.year < b.year;
    if (a.customer_nationkey != b.customer_nationkey)
      return a.customer_nationkey < b.customer_nationkey;
    if (a.supplier_nationkey != b.supplier_nationkey)
      return a.supplier_nationkey < b.supplier_nationkey;
    return a.part_type_index < b.part_type_index;
  }
};

bool CubeCellKeyLess(const TpchQ8CubeCell &cell, int32_t year,
                     int32_t customer_nationkey, int32_t supplier_nationkey,
                     int32_t part_type_index) {
  if (cell.year != year) return cell.year < year;
  if (cell.customer_nationkey != customer_nationkey)
    return cell.customer_nationkey < customer_nationkey;
  if (cell.supplier_nationkey != supplier_nationkey)
    return cell.supplier_nationkey < supplier_nationkey;
  return cell.part_type_index < part_type_index;
}

bool CubeCellKeyEqual(const TpchQ8CubeCell &cell, int32_t year,
                      int32_t customer_nationkey, int32_t supplier_nationkey,
                      int32_t part_type_index) {
  return cell.year == year && cell.customer_nationkey == customer_nationkey &&
         cell.supplier_nationkey == supplier_nationkey &&
         cell.part_type_index == part_type_index;
}

// Find the first cell whose key >= the requested key.  If it does not match,
// returns an insertion point.
size_t CubeCellLowerBound(const std::vector<TpchQ8CubeCell> &cube,
                          int32_t year, int32_t customer_nationkey,
                          int32_t supplier_nationkey, int32_t part_type_index) {
  size_t lo = 0;
  size_t hi = cube.size();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (CubeCellKeyLess(cube[mid], year, customer_nationkey,
                        supplier_nationkey, part_type_index)) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

void SortCube(std::vector<TpchQ8CubeCell> &cube) {
  std::sort(cube.begin(), cube.end(), TpchQ8CubeCellLess());
}

TpchQ8CubeCell *CubeCellFindOrInsert(std::vector<TpchQ8CubeCell> &cube,
                                     int32_t year, int32_t customer_nationkey,
                                     int32_t supplier_nationkey,
                                     int32_t part_type_index) {
  const size_t idx = CubeCellLowerBound(cube, year, customer_nationkey,
                                        supplier_nationkey, part_type_index);
  if (idx >= cube.size() ||
      !CubeCellKeyEqual(cube[idx], year, customer_nationkey,
                        supplier_nationkey, part_type_index)) {
    TpchQ8CubeCell cell;
    cell.year = year;
    cell.customer_nationkey = customer_nationkey;
    cell.supplier_nationkey = supplier_nationkey;
    cell.part_type_index = part_type_index;
    cube.insert(cube.begin() + static_cast<std::ptrdiff_t>(idx), cell);
    return &cube[idx];
  }
  return &cube[idx];
}

// ── Date helpers ────────────────────────────────────────────────────────────
// Use the range-based helper on TpchQ8State. A naive Howard-Hinnant year
// extraction without the Jan/Feb (m <= 2) civil adjustment mis-bins those
// months (e.g. 1995-01 → 1994) and shifts Q8 market share (~0.046 vs 0.0455).
int32_t OrderDateToYear(int32_t days) {
  return TpchQ8State::YearFromDays(days);
}

// ── Volume computation ──────────────────────────────────────────────────────
int64_t ComputeVolumeRaw(int64_t extendedprice_raw, int64_t discount_raw) {
  // volume = l_extendedprice * (1 - l_discount)
  // Both inputs are scaled by 100 (scale=2).
  // So volume_raw = extendedprice_raw * (100 - discount_raw)
  // Result scale is 2+2=4, i.e. raw *= 100 for output scale 4
  return extendedprice_raw * (100 - discount_raw);
}

} // namespace

int64_t TpchQ8VolumeRaw(int64_t extendedprice_raw, int64_t discount_raw) {
  return ComputeVolumeRaw(extendedprice_raw, discount_raw);
}

// ── Encode ──────────────────────────────────────────────────────────────────
std::vector<uint8_t> EncodeTpchQ8AnnualCube(const TpchQ8State &state) {
  ByteWriter writer;
  core::WriteImageHeader(writer, kCubeMagic, kVersionV1,
                         static_cast<uint32_t>(state.cube.size()));
  for (const auto &cell : state.cube) {
    writer.I32(cell.year);
    writer.I32(cell.customer_nationkey);
    writer.I32(cell.supplier_nationkey);
    writer.I32(cell.part_type_index);
    writer.I64(cell.nation_volume_raw);
    writer.I64(cell.total_volume_raw);
  }
  // Encode dimension dictionaries
  writer.U32(static_cast<uint32_t>(state.dims->part_types.size()));
  for (const auto &pt : state.dims->part_types)
    writer.String(pt);
  writer.U32(static_cast<uint32_t>(state.dims->nation_name_to_key.size()));
  for (const auto &[name, nk] : state.dims->nation_name_to_key) {
    writer.String(name);
    writer.I32(nk);
  }
  writer.U32(static_cast<uint32_t>(state.dims->nation_region.size()));
  for (const auto &[nk, rname] : state.dims->nation_region) {
    writer.I32(nk);
    writer.String(rname);
  }
  writer.U32(static_cast<uint32_t>(state.dims->region_nations.size()));
  for (const auto &[rname, nks] : state.dims->region_nations) {
    writer.String(rname);
    writer.U32(static_cast<uint32_t>(nks.size()));
    for (const auto nk : nks)
      writer.I32(nk);
  }
  // Reverse maps for maintenance
  writer.U32(static_cast<uint32_t>(state.dims->part_key_to_type.size()));
  for (const auto &[pk, pt] : state.dims->part_key_to_type) {
    writer.I64(pk);
    writer.String(pt);
  }
  writer.U32(static_cast<uint32_t>(state.dims->suppkey_to_nation.size()));
  for (const auto &[sk, nk] : state.dims->suppkey_to_nation) {
    writer.I64(sk);
    writer.I32(nk);
  }
  writer.U32(static_cast<uint32_t>(state.dims->custkey_to_nation.size()));
  for (const auto &[ck, nk] : state.dims->custkey_to_nation) {
    writer.I64(ck);
    writer.I32(nk);
  }
  writer.U32(static_cast<uint32_t>(state.dims->order_info.size()));
  for (const auto &[ok, oi] : state.dims->order_info) {
    writer.I64(ok);
    writer.I64(oi.custkey);
    writer.I32(oi.orderdate_days);
  }
  return std::move(writer.bytes);
}

// ── Decode ──────────────────────────────────────────────────────────────────
bool DecodeTpchQ8AnnualCube(const std::vector<uint8_t> &bytes,
                            TpchQ8State &state) {
  TpchQ8Dimensions d;
  ByteReader reader{bytes};
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kCubeMagic, kVersionV1, count))
    return false;

  state.cube.clear();
  state.cube.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    TpchQ8CubeCell cell;
    if (!reader.I32(cell.year) || !reader.I32(cell.customer_nationkey) ||
        !reader.I32(cell.supplier_nationkey) || !reader.I32(cell.part_type_index) ||
        !reader.I64(cell.nation_volume_raw) || !reader.I64(cell.total_volume_raw))
      return false;
    state.cube.push_back(cell);
  }
  SortCube(state.cube);

  uint32_t pt_count = 0;
  if (!reader.U32(pt_count))
    return false;
  d.part_types.clear();
  d.part_type_to_index.clear();
  d.part_types.reserve(pt_count);
  for (uint32_t i = 0; i < pt_count; ++i) {
    std::string pt;
    if (!reader.String(pt))
      return false;
    d.part_type_to_index[pt] = static_cast<int32_t>(d.part_types.size());
    d.part_types.push_back(pt);
  }

  uint32_t nn_count = 0;
  if (!reader.U32(nn_count))
    return false;
  d.nation_name_to_key.clear();
  for (uint32_t i = 0; i < nn_count; ++i) {
    std::string name;
    int32_t nk = 0;
    if (!reader.String(name) || !reader.I32(nk))
      return false;
    d.nation_name_to_key[name] = nk;
  }

  uint32_t nr_count = 0;
  if (!reader.U32(nr_count))
    return false;
  d.nation_region.clear();
  for (uint32_t i = 0; i < nr_count; ++i) {
    int32_t nk = 0;
    std::string rname;
    if (!reader.I32(nk) || !reader.String(rname))
      return false;
    d.nation_region[nk] = rname;
  }

  uint32_t rn_count = 0;
  if (!reader.U32(rn_count))
    return false;
  d.region_nations.clear();
  for (uint32_t i = 0; i < rn_count; ++i) {
    std::string rname;
    uint32_t nks_count = 0;
    if (!reader.String(rname) || !reader.U32(nks_count))
      return false;
    auto &nks = d.region_nations[rname];
    nks.reserve(nks_count);
    for (uint32_t j = 0; j < nks_count; ++j) {
      int32_t nk = 0;
      if (!reader.I32(nk))
        return false;
      nks.push_back(nk);
    }
  }

  uint32_t pkt_count = 0;
  if (!reader.U32(pkt_count))
    return false;
  d.part_key_to_type.clear();
  for (uint32_t i = 0; i < pkt_count; ++i) {
    int64_t pk = 0;
    std::string pt;
    if (!reader.I64(pk) || !reader.String(pt))
      return false;
    d.part_key_to_type[pk] = pt;
  }

  uint32_t skn_count = 0;
  if (!reader.U32(skn_count))
    return false;
  d.suppkey_to_nation.clear();
  for (uint32_t i = 0; i < skn_count; ++i) {
    int64_t sk = 0;
    int32_t nk = 0;
    if (!reader.I64(sk) || !reader.I32(nk))
      return false;
    d.suppkey_to_nation[sk] = nk;
  }

  uint32_t ckn_count = 0;
  if (!reader.U32(ckn_count))
    return false;
  d.custkey_to_nation.clear();
  for (uint32_t i = 0; i < ckn_count; ++i) {
    int64_t ck = 0;
    int32_t nk = 0;
    if (!reader.I64(ck) || !reader.I32(nk))
      return false;
    d.custkey_to_nation[ck] = nk;
  }

  uint32_t oi_count = 0;
  if (!reader.U32(oi_count))
    return false;
  d.order_info.clear();
  for (uint32_t i = 0; i < oi_count; ++i) {
    int64_t ok = 0;
    TpchQ8Dimensions::OrderInfo oi;
    if (!reader.I64(ok) || !reader.I64(oi.custkey) || !reader.I32(oi.orderdate_days))
      return false;
    d.order_info[ok] = oi;
  }

  state.dims = std::make_shared<const TpchQ8Dimensions>(std::move(d));
  return reader.FullyConsumed();
}

// ── TxnBuffer ───────────────────────────────────────────────────────────────
bool TpchQ8TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ8TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
  order_info_overlay.clear();
}
bool TpchQ8TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ8LineitemDelta> &TpchQ8TxnBuffer::Deltas() const {
  return deltas;
}
void TpchQ8TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  deltas.clear();
}
void TpchQ8TxnBuffer::AddDelta(TpchQ8LineitemDelta delta) {
  pending = true;
  if (delta.volume_raw != 0)
    deltas.push_back(delta);
}
void TpchQ8TxnBuffer::AddOrderInfo(int64_t orderkey, int64_t custkey,
                                  int32_t orderdate_days) {
  pending = true;
  order_info_overlay[orderkey] = TpchQ8Dimensions::OrderInfo{custkey,
                                                            orderdate_days};
}
const std::unordered_map<int64_t, TpchQ8Dimensions::OrderInfo> &
TpchQ8TxnBuffer::OrderInfoOverlay() const {
  return order_info_overlay;
}

// ── Module identity ─────────────────────────────────────────────────────────
core::AcceleratorId TpchQ8Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ8Module::Dependencies() const {
  return {{config.part_table, {0, 1}, {0, 4}},
          {config.supplier_table, {0, 1}, {0, 3}},
          {config.customer_table, {0, 1}, {0, 3}},
          {config.orders_table, {0, 1, 2}, {0, 1, 4}},
          {config.lineitem_table, {0, 1, 2, 3, 4}, {1, 0, 2, 5, 6}},
          {config.nation_table, {0, 1, 2}, {0, 1, 2}},
          {config.region_table, {0, 1}, {0, 1}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ8Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ8TxnBuffer>();
}

// ── Build ───────────────────────────────────────────────────────────────────
void TpchQ8Module::Build(core::AuxEngineServices &services,
                         core::StateBuilder &builder) {
  const auto state = BuildState(services);
  auto typed_state = std::make_shared<const TpchQ8State>(state);
  // Store full state in both year partitions (same full cube with both years)
  builder.Write(core::StateImage::Make(
      TpchQ8Layout::kAccelerator, TpchQ8Layout::kYear1995, 0,
      MakeStateImage(TpchQ8ImageKind::AnnualCube, 1995, typed_state)));
  builder.Write(core::StateImage::Make(
      TpchQ8Layout::kAccelerator, TpchQ8Layout::kYear1996, 0,
      MakeStateImage(TpchQ8ImageKind::AnnualCube, 1996, typed_state)));
  builder.Finish();
}

// ── InterpretMutation ──────────────────────────────────────────────────────
core::InvalidationPlan
TpchQ8Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table ||
      mutation.table == config.orders_table ||
      mutation.table == config.part_table ||
      mutation.table == config.supplier_table ||
      mutation.table == config.customer_table ||
      mutation.table == config.nation_table ||
      mutation.table == config.region_table) {
    return All();
  }
  return {};
}

// ── AccumulateMutation ─────────────────────────────────────────────────────
void TpchQ8Module::AccumulateMutation(const core::MutationBatch &mutation,
                                      core::AuxEngineServices &,
                                      core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ8TxnBuffer &>(base_buffer);

  // Dimension-table mutations (part, supplier, customer, nation, region:
  // the state caches nation names and regions) → full rebuild
  if (mutation.table == config.part_table ||
      mutation.table == config.supplier_table ||
      mutation.table == config.customer_table ||
      mutation.table == config.nation_table ||
      mutation.table == config.region_table) {
    buffer.MarkRebuild();
    return;
  }

  if (mutation.table == config.orders_table) {
    auto add = [&](const core::AuxRow &row) {
      int64_t orderkey = 0, custkey = 0;
      int32_t orderdate_days = 0;
      if (!ReadOrders(row, orderkey, custkey, orderdate_days)) {
        buffer.MarkRebuild();
        return;
      }
      buffer.AddOrderInfo(orderkey, custkey, orderdate_days);
    };
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows)
        add(row);
      break;
    case core::MutationKind::Delete:
    case core::MutationKind::Update:
      for (const auto &row : mutation.old_rows)
        add(row);
      for (const auto &row : mutation.new_rows)
        add(row);
      break;
    }
    return;
  }

  if (mutation.table != config.lineitem_table)
    return;

  auto add = [&](const core::AuxRow &row, int8_t sign) {
    TpchQ8LineitemInput input;
    if (!ReadLineitem(row, input)) {
      buffer.MarkRebuild();
      return;
    }
    const int64_t volume = ComputeVolumeRaw(input.extendedprice_raw,
                                            input.discount_raw);
    buffer.AddDelta({input.partkey, input.orderkey, input.suppkey,
                     volume, sign});
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

// ── StageMaintenance ───────────────────────────────────────────────────────
core::StagedMaintenanceOutcome
TpchQ8Module::StageMaintenance(core::AuxEngineServices &services,
                               const core::InvalidationPlan &,
                               core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ8TxnBuffer &>(base_buffer);
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
  if (buffer.Deltas().empty())
    return outcome;

  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(buffer.Deltas().size()), 1, true, false, true,
       false, false, core::MaintenanceAction::NoOp,
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

  // Read existing state — dense cube is copied contiguously (memcpy); dimensions are shared (COW on write).
  core::Version existing_version = 0;
  const auto visibility = services.CurrentVisibility();
  const auto snapshot = services.StateStore().ReadPartitions(
      visibility, {TpchQ8Layout::kYear1995});
  std::shared_ptr<TpchQ8State> new_state;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ8Layout::kYear1995) {
      const auto *typed = AsQ8Image(image, TpchQ8ImageKind::AnnualCube);
      if (!typed) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
      new_state = std::make_shared<TpchQ8State>();
      new_state->cube = typed->state->cube;  // vector copy: contiguous, no rehash
      new_state->dims = typed->state->dims;  // shared — cheap
      existing_version = image.version;
      break;
    }
  }
  if (!new_state) {
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }

  // COW helper: only deep-copy dims when a write to them is actually needed.
  const TpchQ8Dimensions *dims = new_state->dims.get();
  std::shared_ptr<TpchQ8Dimensions> mutable_dims;

  auto EnsureMutableDims = [&]() {
    if (!mutable_dims) {
      mutable_dims = std::make_shared<TpchQ8Dimensions>(*dims);
      new_state->dims = mutable_dims;
      dims = mutable_dims.get();
    }
  };

  // Apply each delta to the cube
  for (const auto &delta : buffer.Deltas()) {
    // Resolve dimension keys
    auto pk_it = dims->part_key_to_type.find(delta.partkey);
    if (pk_it == dims->part_key_to_type.end()) {
      if (!initial_scan) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
      TpchQ8PartInput part;
      if (!initial_scan->LookupPart(delta.partkey, part) || part.type.empty()) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
      EnsureMutableDims();
      mutable_dims->part_key_to_type[delta.partkey] = part.type;
      if (mutable_dims->part_type_to_index.find(part.type) ==
          mutable_dims->part_type_to_index.end()) {
        mutable_dims->part_type_to_index[part.type] =
            static_cast<int32_t>(mutable_dims->part_types.size());
        mutable_dims->part_types.push_back(part.type);
      }
      pk_it = mutable_dims->part_key_to_type.find(delta.partkey);
    }
    const auto &part_type = pk_it->second;

    auto sk_it = dims->suppkey_to_nation.find(delta.suppkey);
    if (sk_it == dims->suppkey_to_nation.end()) {
      if (!initial_scan) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
      TpchQ8SupplierInput supplier;
      if (!initial_scan->LookupSupplier(delta.suppkey, supplier)) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
      EnsureMutableDims();
      mutable_dims->suppkey_to_nation[delta.suppkey] = supplier.nationkey;
      sk_it = mutable_dims->suppkey_to_nation.find(delta.suppkey);
    }
    const int32_t supplier_nationkey = sk_it->second;

    // Resolve order → customer → nation
    const TpchQ8Dimensions::OrderInfo *order_info = nullptr;
    const auto overlay_it = buffer.OrderInfoOverlay().find(delta.orderkey);
    if (overlay_it != buffer.OrderInfoOverlay().end()) {
      order_info = &overlay_it->second;
    } else {
      auto oi_it = dims->order_info.find(delta.orderkey);
      if (oi_it != dims->order_info.end())
        order_info = &oi_it->second;
    }
    int32_t customer_nationkey = -1;
    if (order_info) {
      auto ck_it = dims->custkey_to_nation.find(order_info->custkey);
      if (ck_it != dims->custkey_to_nation.end())
        customer_nationkey = ck_it->second;
    }
    if (customer_nationkey < 0) {
      if (!initial_scan) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
      TpchQ8OrdersInput order;
      if (!initial_scan->LookupOrder(delta.orderkey, order)) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
      EnsureMutableDims();
      mutable_dims->order_info[delta.orderkey] =
          TpchQ8Dimensions::OrderInfo{order.custkey, order.orderdate_days};
      auto ck_it = mutable_dims->custkey_to_nation.find(order.custkey);
      if (ck_it == mutable_dims->custkey_to_nation.end()) {
        TpchQ8CustomerInput customer;
        if (!initial_scan->LookupCustomer(order.custkey, customer)) {
          outcome.action = core::MaintenanceAction::RebuildAll;
          outcome.invalidations = All();
          outcome.pending_rebuild = outcome.invalidations;
          return outcome;
        }
        mutable_dims->custkey_to_nation[order.custkey] = customer.nationkey;
        ck_it = mutable_dims->custkey_to_nation.find(order.custkey);
      }
      customer_nationkey = ck_it->second;
      order_info = &mutable_dims->order_info[delta.orderkey];
    }

    const int32_t year = OrderDateToYear(order_info->orderdate_days);
    if (year != 1995 && year != 1996)
      continue;

    const auto pt_idx =
        dims->part_type_to_index.find(part_type);
    if (pt_idx == dims->part_type_to_index.end())
      continue;

    auto *cell = CubeCellFindOrInsert(new_state->cube, year, customer_nationkey,
                                      supplier_nationkey, pt_idx->second);
    const int64_t signed_vol =
        static_cast<int64_t>(delta.sign) * delta.volume_raw;
    cell->total_volume_raw += signed_vol;
    // nation_volume is added for all cells; which nation the volume is
    // attributed to is determined at query time by filtering on supplier_nation
    cell->nation_volume_raw += signed_vol;
  }

  outcome.action = core::MaintenanceAction::ApplyDelta;
  const auto typed_state =
      std::static_pointer_cast<const TpchQ8State>(std::move(new_state));
  // Publish to both year partitions
  outcome.updates.push_back(core::StateImage::Make(
      TpchQ8Layout::kAccelerator, TpchQ8Layout::kYear1995, existing_version,
      MakeStateImage(TpchQ8ImageKind::AnnualCube, 1995, typed_state)));
  outcome.updates.push_back(core::StateImage::Make(
      TpchQ8Layout::kAccelerator, TpchQ8Layout::kYear1996, existing_version,
      MakeStateImage(TpchQ8ImageKind::AnnualCube, 1996, typed_state)));
  return outcome;
}

// ── CheckValidity ──────────────────────────────────────────────────────────
bool TpchQ8Module::CheckValidity(core::AuxEngineServices &services,
                                 const core::QueryArguments &arguments) const {
  if (arguments.values.size() < 3)
    return false;
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

// ── ResolveBindings ────────────────────────────────────────────────────────
bool TpchQ8Module::ResolveBindings(core::AuxEngineServices &services,
                                   const core::QueryArguments &arguments,
                                   std::string &nation_name,
                                   std::string &region_name,
                                   std::string &part_type) const {
  (void)services;
  if (arguments.values.size() < 3)
    return false;
  if (arguments.values[0].is_null || arguments.values[1].is_null ||
      arguments.values[2].is_null)
    return false;
  nation_name = std::get<std::string>(arguments.values[0].payload);
  region_name = std::get<std::string>(arguments.values[1].payload);
  part_type = std::get<std::string>(arguments.values[2].payload);
  return !nation_name.empty() && !region_name.empty() && !part_type.empty();
}

// ── Execute ────────────────────────────────────────────────────────────────
void TpchQ8Module::Execute(core::AuxEngineServices &services,
                           const core::QueryArguments &arguments,
                           core::AuxRowSink &sink) {
  std::string nation_name, region_name, part_type;
  if (!ResolveBindings(services, arguments, nation_name, region_name, part_type)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  if (!services.StateStore()
           .CheckValidity({Id(), {}}, services.CurrentVisibility())
           .IsValid()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  std::shared_ptr<const TpchQ8State> state;
  if (!LoadState(services, state)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  EmitResult(*state, nation_name, region_name, part_type, services, sink);
}

void TpchQ8Module::ExecuteFallback(core::AuxEngineServices &services,
                                   const core::QueryArguments &arguments,
                                   core::AuxRowSink &sink) {
  std::string nation_name, region_name, part_type;
  if (!ResolveBindings(services, arguments, nation_name, region_name, part_type)) {
    sink.Begin(core::OutputSchema{});
    sink.End();
    return;
  }

  EmitResult(BuildState(services), nation_name, region_name, part_type,
             services, sink);
}

// ── BuildState ─────────────────────────────────────────────────────────────
TpchQ8State TpchQ8Module::BuildState(core::AuxEngineServices &services) const {
  (void)services;
  TpchQ8State state;
  TpchQ8Dimensions dims;
  // Build-time aggregation uses a temporary hash map for O(N) inserts.  The
  // finalized state stores a dense, sorted vector so point-write maintenance
  // can copy it with a cheap contiguous memcpy.
  std::unordered_map<TpchQ8CubeKey, TpchQ8State::VolumePair,
                     TpchQ8CubeKeyHash> build_cube;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q8 aux requires a typed initial scan source");

  // Build dimension dictionaries: parts
  std::unordered_map<int64_t, std::string> part_type_map;
  initial_scan->ScanParts([&](const TpchQ8PartInput &input) {
    if (input.key <= 0)
      return;
    part_type_map[input.key] = input.type;
    dims.part_key_to_type[input.key] = input.type;
    if (dims.part_type_to_index.find(input.type) ==
        dims.part_type_to_index.end()) {
      dims.part_type_to_index[input.type] =
          static_cast<int32_t>(dims.part_types.size());
      dims.part_types.push_back(input.type);
    }
  });

  // Build dimension dictionaries: suppliers
  initial_scan->ScanSuppliers([&](const TpchQ8SupplierInput &input) {
    if (input.key <= 0)
      return;
    dims.suppkey_to_nation[input.key] = input.nationkey;
  });

  // Build dimension dictionaries: customers
  initial_scan->ScanCustomers([&](const TpchQ8CustomerInput &input) {
    if (input.key <= 0)
      return;
    dims.custkey_to_nation[input.key] = input.nationkey;
  });

  // Build orders map
  initial_scan->ScanOrders([&](const TpchQ8OrdersInput &input) {
    if (input.key <= 0)
      return;
    dims.order_info[input.key] =
        TpchQ8Dimensions::OrderInfo{input.custkey, input.orderdate_days};
  });

  // Build nation/region dictionaries in one pass each.
  std::unordered_map<int32_t, std::string> regionkey_to_name;
  initial_scan->ScanRegions([&](const TpchQ8RegionInput &input) {
    regionkey_to_name[input.key] = input.name;
  });

  initial_scan->ScanNations([&](const TpchQ8NationInput &input) {
    dims.nation_name_to_key[input.name] = input.key;
    const auto rit = regionkey_to_name.find(input.regionkey);
    if (rit != regionkey_to_name.end()) {
      dims.nation_region[input.key] = rit->second;
      dims.region_nations[rit->second].push_back(input.key);
    }
  });

  // Now scan lineitems and build the cube
  initial_scan->ScanLineitems([&](const TpchQ8LineitemInput &input) {
    if (input.partkey <= 0 || input.orderkey <= 0 || input.suppkey <= 0)
      return;

    // Resolve part type
    const auto pt_it = part_type_map.find(input.partkey);
    if (pt_it == part_type_map.end())
      return;
    const auto &pt = pt_it->second;
    const auto pt_idx_it = dims.part_type_to_index.find(pt);
    if (pt_idx_it == dims.part_type_to_index.end())
      return;

    // Resolve order → customer → nation
    const auto oi_it = dims.order_info.find(input.orderkey);
    if (oi_it == dims.order_info.end())
      return;
    const auto &oi = oi_it->second;
    const auto ckn_it = dims.custkey_to_nation.find(oi.custkey);
    if (ckn_it == dims.custkey_to_nation.end())
      return;
    const int32_t customer_nationkey = ckn_it->second;

    // Resolve supplier nation
    const auto skn_it = dims.suppkey_to_nation.find(input.suppkey);
    if (skn_it == dims.suppkey_to_nation.end())
      return;

    const int32_t year = OrderDateToYear(oi.orderdate_days);
    if (year != 1995 && year != 1996)
      return;

    const int64_t volume = ComputeVolumeRaw(input.extendedprice_raw,
                                            input.discount_raw);

    TpchQ8CubeKey key;
    key.year = year;
    key.customer_nationkey = customer_nationkey;
    key.supplier_nationkey = skn_it->second;
    key.part_type_index = pt_idx_it->second;
    auto &vol = build_cube[key];
    vol.nation_volume_raw += volume;
    vol.total_volume_raw += volume;
  });

  state.cube.reserve(build_cube.size());
  for (auto &[key, vol] : build_cube) {
    state.cube.push_back({key.year, key.customer_nationkey,
                          key.supplier_nationkey, key.part_type_index,
                          vol.nation_volume_raw, vol.total_volume_raw});
  }
  SortCube(state.cube);

  // Keep reverse maps published. PostgreSQL PreCommit cannot safely run SPI
  // InitialScan lookups, and RF inserts/deletes need order/part/supplier
  // resolution without falling back to RebuildAll.

  state.dims = std::make_shared<const TpchQ8Dimensions>(std::move(dims));
  return state;
}

// ── LoadState ──────────────────────────────────────────────────────────────
bool TpchQ8Module::LoadState(core::AuxEngineServices &services,
                             std::shared_ptr<const TpchQ8State> &state) const {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {TpchQ8Layout::kYear1995, TpchQ8Layout::kYear1996});
  for (const auto &image : snapshot.partitions) {
    const auto *typed = AsQ8Image(image, TpchQ8ImageKind::AnnualCube);
    if (!typed)
      continue;
    state = typed->state;
    return true;
  }
  return false;
}

// ── EmitResult ─────────────────────────────────────────────────────────────
void TpchQ8Module::EmitResult(const TpchQ8State &state,
                              const std::string &nation_name,
                              const std::string &region_name,
                              const std::string &part_type,
                              core::AuxEngineServices &services,
                              core::AuxRowSink &sink) const {
  (void)services;

  // Resolve target nationkey
  int32_t target_nationkey = -1;
  const auto nn_it = state.dims->nation_name_to_key.find(nation_name);
  if (nn_it != state.dims->nation_name_to_key.end())
    target_nationkey = nn_it->second;

  // Resolve part type index
  const auto pt_it = state.dims->part_type_to_index.find(part_type);
  if (pt_it == state.dims->part_type_to_index.end()) {
    sink.Begin(core::OutputSchema{});
    sink.End();
    return;
  }
  const int32_t pt_index = pt_it->second;

  // Resolve region nations
  const auto rn_it = state.dims->region_nations.find(region_name);
  if (rn_it == state.dims->region_nations.end() || rn_it->second.empty()) {
    sink.Begin(core::OutputSchema{});
    sink.End();
    return;
  }
  const auto &region_nationkeys = rn_it->second;
  std::unordered_set<int32_t> region_set(region_nationkeys.begin(),
                                         region_nationkeys.end());

  // Aggregate: year → (nation_volume, total_volume)
  struct YearAgg {
    int64_t nation_volume_raw = 0;
    int64_t total_volume_raw = 0;
  };
  YearAgg year_aggs[2];
  static constexpr int32_t kYear1995 = 1995;
  static constexpr int32_t kYear1996 = 1996;

  for (const auto &cell : state.cube) {
    if (cell.year != kYear1995 && cell.year != kYear1996)
      continue;
    if (cell.part_type_index != pt_index)
      continue;
    // Customer nation must be in region
    if (region_set.find(cell.customer_nationkey) == region_set.end())
      continue;

    const int year_idx = cell.year == kYear1995 ? 0 : 1;
    year_aggs[year_idx].total_volume_raw += cell.total_volume_raw;
    if (target_nationkey >= 0 && cell.supplier_nationkey == target_nationkey)
      year_aggs[year_idx].nation_volume_raw += cell.nation_volume_raw;
  }

  // Build output rows sorted by year.
  // SQL GROUP BY only emits years with at least one qualifying fact row;
  // a year with no denominator does not appear in the result set.
  std::vector<TpchQ8OutputRow> output_rows;
  output_rows.reserve(2);
  for (int i = 0; i < 2; ++i) {
    const auto &agg = year_aggs[i];
    if (agg.total_volume_raw == 0)
      continue;
    const int64_t year = i == 0 ? kYear1995 : kYear1996;
    // Compute market share as double
    // nation_volume_raw / total_volume_raw
    // Both are scaled by 4 (extendedprice scale 2 * discount scale 2)
    // The ratio cancels the scale.
    double mkt_share = static_cast<double>(agg.nation_volume_raw) /
                       static_cast<double>(agg.total_volume_raw);
    output_rows.push_back({year, mkt_share});
  }

  // Sort by year
  std::sort(output_rows.begin(), output_rows.end(),
            [](const TpchQ8OutputRow &a, const TpchQ8OutputRow &b) {
              return a.o_year < b.o_year;
            });

  sink.Begin(core::OutputSchema{});
  for (const auto &row : output_rows) {
    core::AuxRow out;
    out.values.reserve(2);
    out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, row.o_year));
    out.values.push_back(core::AuxValue::Of(core::AuxType::Double,
                                            row.mkt_share));
    sink.WriteRow(out);
  }
  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ8Module::CalibrationWrites() const {
  return TpchCalibrationWrites(8);
}

} // namespace accelerator_codegen::aux_state::query
