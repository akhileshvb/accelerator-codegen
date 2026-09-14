#include "tpch/q2/duckdb/aux/module.hpp"
#include "accelerator_codegen/stateful/core/byte_stream.hpp"

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

constexpr uint32_t kVersion = 1;
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

// Q2 indexes parts/suppliers by TPC-H key in a dense vector. RF leftover rows
// with 9e9-style keys would otherwise try to allocate billions of slots.
constexpr std::size_t kTpchQ2MaxDenseKey = 32'000'000;

bool AcceptDenseKey(int64_t key) {
  return key >= 0 && static_cast<uint64_t>(key) < kTpchQ2MaxDenseKey;
}

} // namespace

std::vector<uint8_t> EncodeTpchQ2State(const TpchQ2State &state) {
  ByteWriter writer;
  const auto &regions = *state.region_names;
  core::WriteImageHeader(writer, kStateMagic, kVersion,
                         static_cast<uint32_t>(regions.size()));
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

  const auto &suppliers = *state.suppliers;
  const auto &supplier_valid = *state.supplier_valid;
  writer.U32(static_cast<uint32_t>(suppliers.size()));
  for (std::size_t key = 0; key < suppliers.size(); ++key) {
    if (!supplier_valid[key])
      continue;
    const auto &supplier = suppliers[key];
    writer.I64(static_cast<int64_t>(key));
    writer.String(supplier.name);
    writer.String(supplier.address);
    writer.I32(supplier.nationkey);
    writer.String(supplier.phone);
    writer.I64(supplier.acctbal_raw);
    writer.String(supplier.comment);
  }

  const auto &parts = *state.parts;
  const auto &part_valid = *state.part_valid;
  writer.U32(static_cast<uint32_t>(parts.size()));
  for (std::size_t key = 0; key < parts.size(); ++key) {
    if (!part_valid[key])
      continue;
    const auto &part = parts[key];
    writer.I64(static_cast<int64_t>(key));
    writer.String(part.mfgr);
    writer.String(part.type);
    writer.I32(part.size);
  }

  // parts_by_size: reconstructable from parts on decode; skip encoding
  // to save serialization time. It's rebuilt during LoadState from BuildState.

  // Encode the effective regional minima: start from the immutable base and
  // apply the overlay (insertions, updates, and tombstones).
  const auto *base = state.regional_minima_base
                         ? state.regional_minima_base.get()
                         : &state.regional_minima;
  std::unordered_map<int64_t, TpchQ2RegionalMinCell> effective;
  effective.reserve(base->size() + state.regional_minima_overlay.size());
  effective.insert(base->begin(), base->end());
  for (const auto &entry : state.regional_minima_overlay) {
    if (entry.second.tied_suppkeys.empty())
      effective.erase(entry.first);
    else
      effective[entry.first] = entry.second;
  }
  writer.U32(static_cast<uint32_t>(effective.size()));
  std::vector<int64_t> cell_keys;
  for (const auto &entry : effective)
    cell_keys.push_back(entry.first);
  std::sort(cell_keys.begin(), cell_keys.end());
  for (const auto key : cell_keys) {
    const auto &cell = effective.at(key);
    writer.I64(key);
    writer.I64(cell.min_supplycost_raw);
    writer.U32(static_cast<uint32_t>(cell.tied_suppkeys.size()));
    for (const auto suppkey : cell.tied_suppkeys)
      writer.I64(suppkey);
  }
  return std::move(writer.bytes);
}

bool DecodeTpchQ2State(const std::vector<uint8_t> &bytes, TpchQ2State &state) {
  ByteReader reader(bytes);
  uint32_t count = 0;
  if (!core::ReadImageHeader(reader, kStateMagic, kVersion, count))
    return false;

  state = TpchQ2State{};
  auto regions = std::make_shared<std::vector<std::string>>();
  auto nations = std::make_shared<std::vector<std::string>>();
  auto nation_regions = std::make_shared<std::vector<int32_t>>();
  auto suppliers = std::make_shared<std::vector<TpchQ2SupplierInfo>>();
  auto supplier_valid = std::make_shared<std::vector<uint8_t>>();
  auto parts = std::make_shared<std::vector<TpchQ2PartInfo>>();
  auto part_valid = std::make_shared<std::vector<uint8_t>>();

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

  uint32_t supplier_count = 0;
  if (!reader.U32(supplier_count))
    return false;
  for (uint32_t i = 0; i < supplier_count; ++i) {
    int64_t key = 0;
    TpchQ2SupplierInfo supplier;
    if (!reader.I64(key) || !reader.String(supplier.name) ||
        !reader.String(supplier.address) || !reader.I32(supplier.nationkey) ||
        !reader.String(supplier.phone) || !reader.I64(supplier.acctbal_raw) ||
        !reader.String(supplier.comment))
      return false;
    if (key < 0 || !AcceptDenseKey(key))
      return false;
    EnsureFlatSize(*suppliers, static_cast<std::size_t>(key) + 1);
    EnsureFlatSize(*supplier_valid, static_cast<std::size_t>(key) + 1);
    (*suppliers)[static_cast<std::size_t>(key)] = std::move(supplier);
    (*supplier_valid)[static_cast<std::size_t>(key)] = 1;
  }

  uint32_t part_count = 0;
  if (!reader.U32(part_count))
    return false;
  for (uint32_t i = 0; i < part_count; ++i) {
    int64_t key = 0;
    TpchQ2PartInfo part;
    int32_t size = 0;
    if (!reader.I64(key) || !reader.String(part.mfgr) ||
        !reader.String(part.type) || !reader.I32(size))
      return false;
    if (key < 0 || !AcceptDenseKey(key))
      return false;
    EnsureFlatSize(*parts, static_cast<std::size_t>(key) + 1);
    EnsureFlatSize(*part_valid, static_cast<std::size_t>(key) + 1);
    part.size = size;
    (*parts)[static_cast<std::size_t>(key)] = std::move(part);
    (*part_valid)[static_cast<std::size_t>(key)] = 1;
    state.parts_by_size[size].push_back(key);
  }

  state.region_names = std::move(regions);
  state.nation_names = std::move(nations);
  state.nation_region = std::move(nation_regions);
  state.suppliers = std::move(suppliers);
  state.supplier_valid = std::move(supplier_valid);
  state.parts = std::move(parts);
  state.part_valid = std::move(part_valid);

  uint32_t cell_count = 0;
  if (!reader.U32(cell_count))
    return false;
  for (uint32_t i = 0; i < cell_count; ++i) {
    int64_t key = 0;
    TpchQ2RegionalMinCell cell;
    uint32_t tie_count = 0;
    if (!reader.I64(key) || !reader.I64(cell.min_supplycost_raw) ||
        !reader.U32(tie_count))
      return false;
    cell.tied_suppkeys.reserve(tie_count);
    for (uint32_t j = 0; j < tie_count; ++j) {
      int64_t suppkey = 0;
      if (!reader.I64(suppkey))
        return false;
      cell.tied_suppkeys.push_back(suppkey);
    }
    state.regional_minima[key] = std::move(cell);
  }
  state.regional_minima_base = std::make_shared<const std::unordered_map<int64_t, TpchQ2RegionalMinCell>>(
      std::move(state.regional_minima));
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
  partsupp_deltas.clear();
}
bool TpchQ2TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ2PartsuppDelta> &
TpchQ2TxnBuffer::PartsuppDeltas() const {
  return partsupp_deltas;
}
void TpchQ2TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
}
void TpchQ2TxnBuffer::AddPartsuppDelta(TpchQ2PartsuppDelta delta) {
  pending = true;
  partsupp_deltas.push_back(std::move(delta));
}

core::AcceleratorId TpchQ2Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ2Module::Dependencies() const {
  return {{config.part_table, {0, 1, 2, 3}},
          {config.supplier_table, {0, 1, 2, 3, 4, 5, 6}},
          {config.partsupp_table, {0, 1, 2}},
          {config.nation_table, {0, 1, 2}},
          {config.region_table, {0, 1}}};
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
  // Partsupp changes can be applied incrementally without full invalidation.
  // Supplier/part/nation/region changes require rebuild since they affect
  // multi-table join semantics that cannot be incrementally patched.
  if (mutation.table == config.partsupp_table) {
    return {};  // incremental — no invalidation needed
  }
  if (mutation.table == config.supplier_table ||
      mutation.table == config.part_table ||
      mutation.table == config.nation_table ||
      mutation.table == config.region_table) {
    return All();
  }
  return {};
}

void TpchQ2Module::AccumulateMutation(const core::MutationBatch &mutation,
                                      core::AuxEngineServices &,
                                      core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ2TxnBuffer &>(base_buffer);
  if (mutation.table == config.partsupp_table) {
    // Collect partsupp deltas for incremental apply — no rebuild needed.
    for (std::size_t i = 0; i < mutation.old_rows.size() && i < mutation.new_rows.size(); ++i) {
      int64_t partkey = 0, suppkey = 0, old_cost = 0, new_cost = 0;
      int32_t scale = 0;
      if (mutation.kind == core::MutationKind::Insert) {
        if (!IntCell(mutation.new_rows[i], 0, partkey) ||
            !IntCell(mutation.new_rows[i], 1, suppkey) ||
            !RawDecimalCell(mutation.new_rows[i], 2, new_cost, scale))
          continue;
        buffer.AddPartsuppDelta({partkey, suppkey, new_cost, 1});
      } else if (mutation.kind == core::MutationKind::Delete) {
        if (!IntCell(mutation.old_rows[i], 0, partkey) ||
            !IntCell(mutation.old_rows[i], 1, suppkey) ||
            !RawDecimalCell(mutation.old_rows[i], 2, old_cost, scale))
          continue;
        buffer.AddPartsuppDelta({partkey, suppkey, old_cost, -1});
      } else if (mutation.kind == core::MutationKind::Update) {
        if (!IntCell(mutation.old_rows[i], 0, partkey) ||
            !IntCell(mutation.old_rows[i], 1, suppkey) ||
            !RawDecimalCell(mutation.old_rows[i], 2, old_cost, scale))
          continue;
        if (!RawDecimalCell(mutation.new_rows[i], 2, new_cost, scale))
          continue;
        buffer.AddPartsuppDelta({partkey, suppkey, old_cost, -1});
        buffer.AddPartsuppDelta({partkey, suppkey, new_cost, 1});
      }
    }
    return;
  }
  if (mutation.table == config.supplier_table ||
      mutation.table == config.part_table ||
      mutation.table == config.nation_table ||
      mutation.table == config.region_table) {
    buffer.MarkRebuild();
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
    // Incremental maintenance: apply partsupp deltas to the regional minima.
    const auto &deltas = buffer.PartsuppDeltas();
    if (deltas.empty()) {
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

    // Copy-on-write state: share immutable dimension maps and the immutable
    // regional_minima_base.  Only regional_minima_overlay is mutated, so the
    // per-write clone is O(overlay), not O(all 590K minima cells).
    auto new_state = std::make_shared<TpchQ2State>();
    new_state->region_names = old_state->region_names;
    new_state->nation_names = old_state->nation_names;
    new_state->nation_region = old_state->nation_region;
    new_state->suppliers = old_state->suppliers;
    new_state->supplier_valid = old_state->supplier_valid;
    new_state->parts = old_state->parts;
    new_state->part_valid = old_state->part_valid;
    new_state->parts_by_size = old_state->parts_by_size;
    new_state->regional_minima_base = old_state->regional_minima_base;
    // Preserve prior ApplyDelta overlay so stacked writes compose correctly.
    new_state->regional_minima_overlay = old_state->regional_minima_overlay;
    if (!new_state->regional_minima_base) {
      new_state->regional_minima_base =
          std::make_shared<const std::unordered_map<int64_t, TpchQ2RegionalMinCell>>(
              old_state->regional_minima);
    }
    bool profiling = CustomProfilingEnabled();
    std::unique_ptr<CustomTimer> clone_timer;
    if (profiling) {
      clone_timer = std::make_unique<CustomTimer>(true);
    }

    // Collect affected (region, part) cells and batch-update minima
    std::unique_ptr<CustomTimer> apply_timer;
    if (profiling) {
      apply_timer = std::make_unique<CustomTimer>(true);
    }
    bool lost_unique_min = false;
    for (const auto &delta : deltas) {
      const auto *supplier = new_state->FindSupplier(delta.suppkey);
      if (!supplier)
        continue;
      if (!new_state->FindPart(delta.partkey))
        continue;
      const auto nationkey = supplier->nationkey;
      if (nationkey < 0 ||
          static_cast<std::size_t>(nationkey) >=
              new_state->nation_region->size())
        continue;
      const int32_t region_key =
          (*new_state->nation_region)[static_cast<std::size_t>(nationkey)];
      const auto cell_key = TpchQ2CellKey(region_key, delta.partkey);

      auto &cell = new_state->regional_minima_overlay[cell_key];
      if (delta.sign < 0) {
        // Remove old entry from tied list
        if (cell.tied_suppkeys.empty()) {
          // Cell wasn't previously materialized in the overlay. Load it from
          // the base so the tombstone/removal is applied to the full list.
          if (new_state->regional_minima_base) {
            const auto base_it = new_state->regional_minima_base->find(cell_key);
            if (base_it != new_state->regional_minima_base->end())
              cell = base_it->second;
          }
        }
        auto &list = cell.tied_suppkeys;
        list.erase(std::remove(list.begin(), list.end(), delta.suppkey),
                   list.end());
        if (list.empty()) {
          // Unique-min delete: the next-best supplycost is not stored on the
          // cell. Rebuild rather than publishing an empty tombstone or the
          // same supplier at a higher cost.
          lost_unique_min = true;
        }
      }
      if (delta.sign > 0) {
        if (cell.tied_suppkeys.empty()) {
          if (new_state->regional_minima_base) {
            const auto base_it = new_state->regional_minima_base->find(cell_key);
            if (base_it != new_state->regional_minima_base->end())
              cell = base_it->second;
          }
        }
        if (cell.tied_suppkeys.empty() ||
            delta.supplycost_raw < cell.min_supplycost_raw) {
          cell.min_supplycost_raw = delta.supplycost_raw;
          cell.tied_suppkeys = {delta.suppkey};
        } else if (delta.supplycost_raw == cell.min_supplycost_raw) {
          cell.tied_suppkeys.push_back(delta.suppkey);
        }
      }
      if (cell.tied_suppkeys.empty()) {
        // Empty overlay entry acts as a tombstone for FindRegionalMin().
        // Keep the empty cell; no erase from overlay is needed.
      }
    }

    if (profiling) {
      double clone_dt = clone_timer->us();
      double apply_dt = apply_timer->us();
      EmitCustomProfile("write_clone", clone_dt,
                        (int64_t)(new_state->regional_minima_base
                                      ? new_state->regional_minima_base->size()
                                      : 0),
                        (int64_t)(new_state->parts
                                      ? new_state->parts->size()
                                      : 0),
                        (int64_t)(new_state->suppliers
                                      ? new_state->suppliers->size()
                                      : 0));
      EmitCustomProfile("write_apply", apply_dt, (int64_t)deltas.size(), 0, 0);
    }
    if (!lost_unique_min) {
      const auto decision = services.ChooseMaintenance(
          {static_cast<uint64_t>(deltas.size()), 1, true, false, true, false,
           false, core::MaintenanceAction::NoOp,
           services.ExpectedQueriesBeforeNextWrite()});
      if (decision.action == core::MaintenanceAction::ApplyDelta) {
        auto image = std::make_shared<TpchQ2StateImage>();
        image->kind = TpchQ2ImageKind::Full;
        image->state = std::move(new_state);
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

  // Full rebuild path for supplier/part/nation/region mutations
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
    int64_t part_count = (int64_t)(state->parts ? state->parts->size() : 0);
    auto size_it = state->parts_by_size.find(part_size);
    int64_t size_matches = size_it != state->parts_by_size.end()
                               ? (int64_t)size_it->second.size()
                               : 0;
    int64_t minima_count =
        state->regional_minima_base
            ? (int64_t)state->regional_minima_base->size()
            : (int64_t)state->regional_minima.size();
    EmitCustomProfile("execute_emit", dt, part_count, size_matches,
                      minima_count);
  }
}

void TpchQ2Module::ExecuteFallback(core::AuxEngineServices &services,
                                   const core::QueryArguments &arguments,
                                   core::AuxRowSink &sink) {
  TpchQ2State state = BuildState(services);
  int32_t part_size = 0;
  std::string part_type_pattern;
  std::string region_name;
  if (arguments.values.size() == 3) {
    if (!arguments.values[0].is_null &&
        std::holds_alternative<int64_t>(arguments.values[0].payload)) {
      part_size = static_cast<int32_t>(std::get<int64_t>(arguments.values[0].payload));
    }
    if (!arguments.values[1].is_null) {
      StringValue(arguments.values[1], part_type_pattern);
    }
    if (!arguments.values[2].is_null) {
      StringValue(arguments.values[2], region_name);
    }
  }
  EmitResult(state, part_size, part_type_pattern, region_name, sink);
}

TpchQ2State
TpchQ2Module::BuildState(core::AuxEngineServices &services) const {
  (void)services;
  TpchQ2State state;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q2 requires a typed initial scan source");

  auto regions = std::make_shared<std::vector<std::string>>();
  auto nations = std::make_shared<std::vector<std::string>>();
  auto nation_regions = std::make_shared<std::vector<int32_t>>();
  auto suppliers = std::make_shared<std::vector<TpchQ2SupplierInfo>>();
  auto supplier_valid = std::make_shared<std::vector<uint8_t>>();
  auto parts = std::make_shared<std::vector<TpchQ2PartInfo>>();
  auto part_valid = std::make_shared<std::vector<uint8_t>>();

  initial_scan->ScanRegions([&](const TpchQ2RegionInput &input) {
    if (!AcceptDenseKey(input.key))
      return;
    EnsureFlatSize(*regions, static_cast<std::size_t>(input.key) + 1);
    (*regions)[static_cast<std::size_t>(input.key)] = input.name;
  });
  initial_scan->ScanNations([&](const TpchQ2NationInput &input) {
    if (!AcceptDenseKey(input.key) || input.regionkey < 0)
      return;
    EnsureFlatSize(*nations, static_cast<std::size_t>(input.key) + 1);
    EnsureFlatSize(*nation_regions, static_cast<std::size_t>(input.key) + 1);
    (*nations)[static_cast<std::size_t>(input.key)] = input.name;
    (*nation_regions)[static_cast<std::size_t>(input.key)] = input.regionkey;
  });
  initial_scan->ScanParts([&](const TpchQ2PartInput &input) {
    if (!AcceptDenseKey(input.key))
      return;
    EnsureFlatSize(*parts, static_cast<std::size_t>(input.key) + 1);
    EnsureFlatSize(*part_valid, static_cast<std::size_t>(input.key) + 1);
    (*parts)[static_cast<std::size_t>(input.key)] = {input.mfgr, input.type,
                                                      input.size};
    (*part_valid)[static_cast<std::size_t>(input.key)] = 1;
    state.parts_by_size[input.size].push_back(input.key);
  });
  initial_scan->ScanSuppliers([&](const TpchQ2SupplierInput &input) {
    if (!AcceptDenseKey(input.key))
      return;
    EnsureFlatSize(*suppliers, static_cast<std::size_t>(input.key) + 1);
    EnsureFlatSize(*supplier_valid, static_cast<std::size_t>(input.key) + 1);
    (*suppliers)[static_cast<std::size_t>(input.key)] = {
        input.name, input.address, input.nationkey, input.phone,
        input.acctbal_raw, input.comment};
    (*supplier_valid)[static_cast<std::size_t>(input.key)] = 1;
  });

  state.region_names = std::move(regions);
  state.nation_names = std::move(nations);
  state.nation_region = std::move(nation_regions);
  state.suppliers = std::move(suppliers);
  state.supplier_valid = std::move(supplier_valid);
  state.parts = std::move(parts);
  state.part_valid = std::move(part_valid);

  // Rebuild the regional minima from the current partsupp facts. The state is
  // intentionally not a final result cache: it stores one (region, part)
  // minimum cell per live supplier/part offer.
  bool profiling = CustomProfilingEnabled();
  std::unique_ptr<CustomTimer> partsupp_timer;
  if (profiling) {
    partsupp_timer = std::make_unique<CustomTimer>(true);
  }
  int64_t partsupp_rows = 0;
  int64_t partsupp_matched = 0;
  const auto &suppliers_ref = *state.suppliers;
  const auto &supplier_valid_ref = *state.supplier_valid;
  const auto &parts_ref = *state.parts;
  const auto &part_valid_ref = *state.part_valid;
  const auto &nation_region_ref = *state.nation_region;
  initial_scan->ScanPartsupps([&](const TpchQ2PartsuppInput &input) {
    partsupp_rows++;
    if (input.suppkey < 0 ||
        static_cast<std::size_t>(input.suppkey) >= supplier_valid_ref.size() ||
        !supplier_valid_ref[static_cast<std::size_t>(input.suppkey)])
      return;
    if (input.partkey < 0 ||
        static_cast<std::size_t>(input.partkey) >= part_valid_ref.size() ||
        !part_valid_ref[static_cast<std::size_t>(input.partkey)])
      return;
    const auto &supplier =
        suppliers_ref[static_cast<std::size_t>(input.suppkey)];
    const auto nationkey = supplier.nationkey;
    if (nationkey < 0 ||
        static_cast<std::size_t>(nationkey) >= nation_region_ref.size())
      return;
    partsupp_matched++;
    const int32_t region_key =
        nation_region_ref[static_cast<std::size_t>(nationkey)];
    const auto key = TpchQ2CellKey(region_key, input.partkey);
    auto &cell = state.regional_minima[key];
    if (cell.tied_suppkeys.empty() ||
        input.supplycost_raw < cell.min_supplycost_raw) {
      cell.min_supplycost_raw = input.supplycost_raw;
      cell.tied_suppkeys = {input.suppkey};
    } else if (input.supplycost_raw == cell.min_supplycost_raw) {
      cell.tied_suppkeys.push_back(input.suppkey);
    }
  });
  state.regional_minima_base =
      std::make_shared<const std::unordered_map<int64_t, TpchQ2RegionalMinCell>>(
          std::move(state.regional_minima));
  if (profiling) {
    double dt = partsupp_timer->us();
    EmitCustomProfile("build_partsupp_scan", dt, partsupp_rows,
                      (int64_t)(state.regional_minima_base
                                    ? state.regional_minima_base->size()
                                    : 0),
                      partsupp_matched);
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

} // namespace accelerator_codegen::aux_state::query
