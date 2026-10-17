#include "tpch/q20/mysql/aux/module.hpp"
#include "tpch/q20/calibration_writes.hpp"
#include "prims/sql_date.hpp"
#include "prims/sql_like.hpp"
#include "substrate/aux/core/parallel_slices.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

namespace like = core::sql_like;
namespace sql_date = core::sql_date;

// String semantics of the target DBMS (LIKE and `n_name = ...`): PostgreSQL is
// case-sensitive; the MySQL copy of this file sets this to true (default utf8mb4
// collations are case-insensitive). This is the only intentional difference
// between the postgres/ and mysql/ copies. '_' matches one UTF-8 character.
constexpr bool kCaseInsensitive = true;

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

bool Int32Cell(const core::AuxRow &row, std::size_t index, int32_t &out) {
  int64_t value = 0;
  if (!IntCell(row, index, value) || value < std::numeric_limits<int32_t>::min() ||
      value > std::numeric_limits<int32_t>::max())
    return false;
  out = static_cast<int32_t>(value);
  return true;
}

// Quantity in hundredths (the engine hands columns over as scaled decimals).
bool QuantityCell(const core::AuxRow &row, std::size_t index, int64_t &raw) {
  if (index >= row.values.size() || row.values[index].is_null ||
      !std::holds_alternative<core::ScaledDecimal>(row.values[index].payload))
    return false;
  const auto decimal = std::get<core::ScaledDecimal>(row.values[index].payload);
  raw = decimal.value;
  for (int32_t s = decimal.scale; s < 2; ++s)
    if (__builtin_mul_overflow(raw, int64_t{10}, &raw))
      throw std::overflow_error("Q20 quantity overflows int64");
  for (int32_t s = decimal.scale; s > 2; --s) {
    if (raw % 10 != 0)
      throw std::domain_error("Q20 quantity has more than 2 decimals");
    raw /= 10;
  }
  return true;
}

// A NULL in any column the query joins or filters on means the row can never
// contribute, so these readers return false and the row is ignored.
bool ReadLineitem(const core::AuxRow &row, int32_t &shipdate_days,
                  int64_t &partkey, int64_t &suppkey, int64_t &quantity_raw) {
  return Int32Cell(row, 0, shipdate_days) && IntCell(row, 1, partkey) &&
         IntCell(row, 2, suppkey) && QuantityCell(row, 3, quantity_raw);
}

bool ReadPart(const core::AuxRow &row, int64_t &partkey, std::string &name) {
  return IntCell(row, 0, partkey) && StringCell(row, 1, name);
}

bool ReadPartsupp(const core::AuxRow &row, int64_t &partkey, int64_t &suppkey,
                  int64_t &availqty) {
  return IntCell(row, 0, partkey) && IntCell(row, 1, suppkey) &&
         IntCell(row, 2, availqty);
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

// ps_availqty > 0.5 * SUM(l_quantity), exactly: availqty * 200 > sum in
// hundredths, in 128-bit arithmetic.
bool AvailQtyGreaterThanHalfSum(int64_t availqty, __int128 sum_qty_raw) {
  return static_cast<__int128>(availqty) * 200 > sum_qty_raw;
}

bool NameEquals(const std::string &a, const std::string &b) {
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (kCaseInsensitive) {
      x = static_cast<char>(x >= 'A' && x <= 'Z' ? x + 32 : x);
      y = static_cast<char>(y >= 'A' && y <= 'Z' ? y + 32 : y);
    }
    if (x != y)
      return false;
  }
  return true;
}

bool NameLess(const std::string &a, const std::string &b) {
  if (!kCaseInsensitive)
    return a < b;
  const std::size_t n = std::min(a.size(), b.size());
  for (std::size_t i = 0; i < n; ++i) {
    char x = a[i], y = b[i];
    x = static_cast<char>(x >= 'A' && x <= 'Z' ? x + 32 : x);
    y = static_cast<char>(y >= 'A' && y <= 'Z' ? y + 32 : y);
    if (x != y)
      return static_cast<unsigned char>(x) < static_cast<unsigned char>(y);
  }
  return a.size() < b.size();
}

// ── Small-value packing: any int64 stays exact ──────────────────────────────
// Values in [-2^30, 2^30) are stored as is; others are stored in the block's
// `wide` table and referenced as INT32_MIN + index (always < -2^30).
constexpr int64_t kNarrowMin = -(int64_t{1} << 30);
constexpr int64_t kNarrowMax = (int64_t{1} << 30) - 1;

int32_t PackValue(int64_t v, std::vector<int64_t> &wide) {
  if (v >= kNarrowMin && v <= kNarrowMax)
    return static_cast<int32_t>(v);
  if (wide.size() >= (std::size_t{1} << 30))
    throw std::length_error("Q20 block wide-value table overflow");
  wide.push_back(v);
  return static_cast<int32_t>(std::numeric_limits<int32_t>::min() +
                              static_cast<int64_t>(wide.size() - 1));
}

int64_t UnpackValue(int32_t code, const std::vector<int64_t> &wide) {
  return code >= kNarrowMin
             ? static_cast<int64_t>(code)
             : wide[static_cast<std::size_t>(static_cast<int64_t>(code) -
                                             std::numeric_limits<int32_t>::min())];
}

// ── Editable form of a block ────────────────────────────────────────────────
// Used for the bulk build and for write batches: a block is decoded, edited
// (cheap appends / searches over a handful of pairs) and re-encoded once.
struct RawPair {
  uint8_t slot = 0; // part index inside the block
  int64_t suppkey = 0;
  bool has_avail = false;
  int64_t avail = 0;
};

struct RawLine {
  uint32_t pair = 0;
  int32_t shipdate = 0;
  int32_t qty = 0; // packed against RawBlock::wide
};

struct RawBlock {
  std::vector<RawPair> pairs;
  std::vector<uint32_t> order; // pair indices sorted by (slot, suppkey)
  std::vector<RawLine> lines;
  std::vector<int64_t> wide;

  // Index of the pair, or kNone.
  static constexpr uint32_t kNone = 0xFFFFFFFFu;
  std::size_t LowerBound(uint8_t slot, int64_t suppkey) const {
    std::size_t lo = 0, hi = order.size();
    while (lo < hi) {
      const std::size_t mid = (lo + hi) / 2;
      const RawPair &p = pairs[order[mid]];
      if (p.slot < slot || (p.slot == slot && p.suppkey < suppkey))
        lo = mid + 1;
      else
        hi = mid;
    }
    return lo;
  }
  uint32_t Find(uint8_t slot, int64_t suppkey) const {
    const std::size_t pos = LowerBound(slot, suppkey);
    if (pos < order.size()) {
      const RawPair &p = pairs[order[pos]];
      if (p.slot == slot && p.suppkey == suppkey)
        return order[pos];
    }
    return kNone;
  }
  uint32_t FindOrAdd(uint8_t slot, int64_t suppkey) {
    const std::size_t pos = LowerBound(slot, suppkey);
    if (pos < order.size()) {
      const RawPair &p = pairs[order[pos]];
      if (p.slot == slot && p.suppkey == suppkey)
        return order[pos];
    }
    pairs.push_back({slot, suppkey, false, 0});
    const auto idx = static_cast<uint32_t>(pairs.size() - 1);
    order.insert(order.begin() + static_cast<std::ptrdiff_t>(pos), idx);
    return idx;
  }

  void AddLine(uint8_t slot, int64_t suppkey, int32_t shipdate, int64_t qty) {
    const uint32_t pair = FindOrAdd(slot, suppkey);
    lines.push_back({pair, shipdate, PackValue(qty, wide)});
  }
  // Removes one matching lineitem entry; a missing one is ignored (the row was
  // never tracked, e.g. it had a NULL column).
  void RemoveLine(uint8_t slot, int64_t suppkey, int32_t shipdate, int64_t qty) {
    const uint32_t pair = Find(slot, suppkey);
    if (pair == kNone)
      return;
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (lines[i].pair == pair && lines[i].shipdate == shipdate &&
          UnpackValue(lines[i].qty, wide) == qty) {
        lines[i] = lines.back();
        lines.pop_back();
        return;
      }
    }
  }
  void SetAvail(uint8_t slot, int64_t suppkey, int64_t availqty) {
    RawPair &p = pairs[FindOrAdd(slot, suppkey)];
    p.has_avail = true;
    p.avail = availqty;
  }
  void ClearAvail(uint8_t slot, int64_t suppkey) {
    const uint32_t pair = Find(slot, suppkey);
    if (pair != kNone)
      pairs[pair].has_avail = false;
  }
};

RawBlock DecodeBlock(const TpchQ20PartBlock *block) {
  RawBlock raw;
  if (!block)
    return raw;
  raw.wide = block->wide;
  for (std::size_t slot = 0; slot < kTpchQ20BlockParts; ++slot) {
    for (uint32_t j = block->pair_begin[slot]; j < block->pair_begin[slot + 1]; ++j) {
      const TpchQ20Pair &p = block->pairs[j];
      RawPair rp;
      rp.slot = static_cast<uint8_t>(slot);
      rp.suppkey = p.suppkey;
      rp.has_avail = (p.entry_begin & TpchQ20Pair::kHasAvail) != 0;
      rp.avail = rp.has_avail ? UnpackValue(p.availqty, block->wide) : 0;
      raw.pairs.push_back(rp);
      raw.order.push_back(static_cast<uint32_t>(raw.pairs.size() - 1));
      const uint32_t begin = p.entry_begin & ~TpchQ20Pair::kHasAvail;
      const uint32_t end = j + 1 < block->pairs.size()
                               ? (block->pairs[j + 1].entry_begin & ~TpchQ20Pair::kHasAvail)
                               : static_cast<uint32_t>(block->entries.size());
      for (uint32_t e = begin; e < end; ++e)
        raw.lines.push_back({static_cast<uint32_t>(raw.pairs.size() - 1),
                             block->entries[e].shipdate, block->entries[e].qty});
    }
  }
  return raw;
}

// Pairs with neither a partsupp row nor lineitems are dropped; an empty block
// becomes nullptr.
std::shared_ptr<TpchQ20PartBlock> EncodeBlock(const RawBlock &raw) {
  std::vector<uint32_t> count(raw.pairs.size(), 0);
  for (const RawLine &line : raw.lines)
    ++count[line.pair];
  std::vector<uint32_t> live_order;
  live_order.reserve(raw.order.size());
  for (const uint32_t idx : raw.order)
    if (raw.pairs[idx].has_avail || count[idx] > 0)
      live_order.push_back(idx);
  if (live_order.empty())
    return nullptr;
  if (raw.lines.size() >= (std::size_t{1} << 31))
    throw std::length_error("Q20 block entry count overflow");

  auto block = std::make_shared<TpchQ20PartBlock>();
  block->pairs.reserve(live_order.size());
  block->entries.resize(raw.lines.size());
  std::vector<uint32_t> start(raw.pairs.size(), 0);
  std::size_t slot = 0;
  uint32_t entry_pos = 0;
  block->pair_begin[0] = 0;
  for (const uint32_t idx : live_order) {
    const RawPair &rp = raw.pairs[idx];
    while (slot < rp.slot) {
      ++slot;
      block->pair_begin[slot] = static_cast<uint32_t>(block->pairs.size());
    }
    TpchQ20Pair pair;
    pair.suppkey = rp.suppkey;
    pair.entry_begin = entry_pos | (rp.has_avail ? TpchQ20Pair::kHasAvail : 0u);
    if (rp.has_avail)
      pair.availqty = PackValue(rp.avail, block->wide);
    start[idx] = entry_pos;
    entry_pos += count[idx];
    block->pairs.push_back(pair);
  }
  while (slot < kTpchQ20BlockParts) {
    ++slot;
    block->pair_begin[slot] = static_cast<uint32_t>(block->pairs.size());
  }
  // Counting-sort the lines into their pair's range, then order by date.
  std::vector<uint32_t> fill(start);
  for (const RawLine &line : raw.lines) {
    const int64_t qty = UnpackValue(line.qty, raw.wide);
    block->entries[fill[line.pair]++] = {line.shipdate, PackValue(qty, block->wide)};
  }
  for (const uint32_t idx : live_order)
    std::sort(block->entries.begin() + start[idx],
              block->entries.begin() + start[idx] + count[idx],
              [&](const TpchQ20Entry &a, const TpchQ20Entry &b) {
                return a.shipdate != b.shipdate
                           ? a.shipdate < b.shipdate
                           : UnpackValue(a.qty, block->wide) < UnpackValue(b.qty, block->wide);
              });
  block->entries.shrink_to_fit();
  block->pairs.shrink_to_fit();
  block->wide.shrink_to_fit();
  return block;
}

constexpr int64_t kBlockParts = static_cast<int64_t>(kTpchQ20BlockParts);
static_assert((kBlockParts & (kBlockParts - 1)) == 0);
// Floor division / modulo by the (power of two) block size: exact for negative keys.
constexpr int64_t BlockOf(int64_t key) { return key >> __builtin_ctzll(static_cast<uint64_t>(kBlockParts)); }
constexpr std::size_t SlotOf(int64_t key) { return static_cast<std::size_t>(key & (kBlockParts - 1)); }

// Blocks per encode slice: each sorts a few thousand lineitem entries (~0.1 ms).
constexpr std::size_t kBlocksGrain = 8;

// Blocks being edited, keyed by block index; re-encoded into the state by Flush.
class BlockEditor {
public:
  explicit BlockEditor(TpchQ20State &state) : state_(state) {}
  RawBlock &For(int64_t partkey) {
    const int64_t bi = BlockOf(partkey);
    auto it = edited_.find(bi);
    if (it == edited_.end())
      it = edited_.emplace(bi, DecodeBlock(state_.FindBlock(partkey))).first;
    return it->second;
  }
  // Blocks encode independently; they enter the state in block order.
  void Flush(const core::AuxEngineServices &services) {
    std::vector<const std::pair<const int64_t, RawBlock> *> edits;
    edits.reserve(edited_.size());
    for (const auto &entry : edited_)
      edits.push_back(&entry);
    std::vector<std::shared_ptr<TpchQ20PartBlock>> encoded(edits.size());
    core::ParallelSlices(services, edits.size(), kBlocksGrain,
                         [&](std::size_t, std::size_t begin, std::size_t end) {
                           for (std::size_t i = begin; i < end; ++i)
                             encoded[i] = EncodeBlock(edits[i]->second);
                         });
    for (std::size_t i = 0; i < edits.size(); ++i)
      if (encoded[i] || state_.blocks.Get(edits[i]->first))
        state_.blocks.Set(edits[i]->first, encoded[i]);
    edited_.clear();
  }
private:
  TpchQ20State &state_;
  std::map<int64_t, RawBlock> edited_;
};

} // namespace

// ── TxnBuffer ───────────────────────────────────────────────────────────────
bool TpchQ20TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ20TxnBuffer::Clear() {
  pending = false;
  lineitem_deltas.clear();
  partsupp_deltas.clear();
  part_deltas.clear();
  supplier_deltas.clear();
  nation_deltas.clear();
}
void TpchQ20TxnBuffer::AddLineitemDelta(TpchQ20LineitemDelta delta) {
  pending = true;
  lineitem_deltas.push_back(delta);
}
void TpchQ20TxnBuffer::AddPartsuppDelta(TpchQ20PartsuppDelta delta) {
  pending = true;
  partsupp_deltas.push_back(delta);
}
void TpchQ20TxnBuffer::AddPartDelta(TpchQ20PartDelta delta) {
  pending = true;
  part_deltas.push_back(std::move(delta));
}
void TpchQ20TxnBuffer::AddSupplierDelta(TpchQ20SupplierDelta delta) {
  pending = true;
  supplier_deltas.push_back(std::move(delta));
}
void TpchQ20TxnBuffer::AddNationDelta(TpchQ20NationDelta delta) {
  pending = true;
  nation_deltas.push_back(std::move(delta));
}

// ── Module ──────────────────────────────────────────────────────────────────
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
  const auto state = BuildState(services);
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

  std::function<void(const core::AuxRow &, int8_t)> add;
  if (mutation.table == config.lineitem_table) {
    add = [&](const core::AuxRow &row, int8_t sign) {
      TpchQ20LineitemDelta d;
      d.sign = sign;
      if (ReadLineitem(row, d.shipdate_days, d.partkey, d.suppkey, d.quantity_raw) )
        buffer.AddLineitemDelta(d);
    };
  } else if (mutation.table == config.partsupp_table) {
    add = [&](const core::AuxRow &row, int8_t sign) {
      TpchQ20PartsuppDelta d;
      d.sign = sign;
      if (ReadPartsupp(row, d.partkey, d.suppkey, d.availqty))
        buffer.AddPartsuppDelta(d);
    };
  } else if (mutation.table == config.part_table) {
    add = [&](const core::AuxRow &row, int8_t sign) {
      TpchQ20PartDelta d;
      d.sign = sign;
      if (ReadPart(row, d.partkey, d.name))
        buffer.AddPartDelta(std::move(d));
    };
  } else if (mutation.table == config.supplier_table) {
    add = [&](const core::AuxRow &row, int8_t sign) {
      TpchQ20SupplierDelta d;
      d.sign = sign;
      if (ReadSupplier(row, d.suppkey, d.supplier.name, d.supplier.address,
                       d.supplier.nationkey))
        buffer.AddSupplierDelta(std::move(d));
    };
  } else if (mutation.table == config.nation_table) {
    add = [&](const core::AuxRow &row, int8_t sign) {
      TpchQ20NationDelta d;
      d.sign = sign;
      if (ReadNation(row, d.nationkey, d.name))
        buffer.AddNationDelta(std::move(d));
    };
  } else {
    return;
  }

  // An update is "all old rows removed, then all new rows added": identical to
  // pairwise handling, and immune to key swaps within one statement.
  if (mutation.kind != core::MutationKind::Insert)
    for (const auto &row : mutation.old_rows)
      add(row, -1);
  if (mutation.kind != core::MutationKind::Delete)
    for (const auto &row : mutation.new_rows)
      add(row, 1);
}

core::StagedMaintenanceOutcome TpchQ20Module::StageMaintenance(
    core::AuxEngineServices &services, const core::InvalidationPlan &,
    core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ20TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;

  const uint64_t delta_count =
      buffer.LineitemDeltas().size() + buffer.PartsuppDeltas().size() +
      buffer.PartDeltas().size() + buffer.SupplierDeltas().size() +
      buffer.NationDeltas().size();
  if (delta_count == 0)
    return outcome;

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
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {TpchQ20Layout::kFullState});
  std::shared_ptr<TpchQ20State> updated;
  for (const auto &image : snapshot.partitions) {
    if (image.partition != TpchQ20Layout::kFullState)
      continue;
    const auto *typed = AsQ20Image(image);
    if (!typed)
      return outcome;
    // Copy-on-write: shares every block / name segment / supplier shard with
    // the published state; only the touched ones are cloned below.
    updated = std::make_shared<TpchQ20State>(*typed->state);
    existing_version = image.version;
    break;
  }
  if (!updated)
    return outcome;

  BlockEditor editor(*updated);
  for (const auto &d : buffer.LineitemDeltas()) {
    auto &raw = editor.For(d.partkey);
    const auto slot = SlotOf(d.partkey);
    if (d.sign > 0)
      raw.AddLine(slot, d.suppkey, d.shipdate_days, d.quantity_raw);
    else
      raw.RemoveLine(slot, d.suppkey, d.shipdate_days, d.quantity_raw);
  }
  for (const auto &d : buffer.PartsuppDeltas()) {
    auto &raw = editor.For(d.partkey);
    const auto slot = SlotOf(d.partkey);
    if (d.sign > 0)
      raw.SetAvail(slot, d.suppkey, d.availqty);
    else
      raw.ClearAvail(slot, d.suppkey);
  }
  editor.Flush(services);

  for (const auto &d : buffer.PartDeltas()) {
    if (d.sign > 0)
      updated->part_names.Set(d.partkey, d.name);
    else
      updated->part_names.Erase(d.partkey);
  }
  for (const auto &d : buffer.SupplierDeltas()) {
    if (d.sign > 0)
      updated->suppliers.Upsert(d.suppkey, d.supplier);
    else
      updated->suppliers.Erase(d.suppkey);
  }
  for (const auto &d : buffer.NationDeltas()) {
    if (d.sign > 0)
      updated->nations[d.nationkey] = d.name;
    else
      updated->nations.erase(d.nationkey);
  }

  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.updates.push_back(core::StateImage::Make(
      TpchQ20Layout::kAccelerator, TpchQ20Layout::kFullState, existing_version,
      MakeStateImage(std::static_pointer_cast<const TpchQ20State>(updated))));
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
  EmitResult(services, *state, arguments, sink);
}

void TpchQ20Module::ExecuteFallback(core::AuxEngineServices &services,
                                    const core::QueryArguments &arguments,
                                    core::AuxRowSink &sink) {
  const auto state = BuildState(services);
  EmitResult(services, *state, arguments, sink);
}

std::shared_ptr<TpchQ20State>
TpchQ20Module::BuildState(core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error(
        "TPC-H Q20 aux requires a typed initial scan source");
  auto state = std::make_shared<TpchQ20State>();

  initial_scan->ScanNations([&](const TpchQ20NationInput &input) {
    state->nations[input.key] = input.name;
  });
  initial_scan->ScanSuppliers([&](const TpchQ20SupplierInput &input) {
    state->suppliers.Upsert(input.key, {input.name, input.address, input.nationkey});
  });
  initial_scan->ScanParts([&](const TpchQ20PartInput &input) {
      state->part_names.Set(input.key, input.name);
  });

  // Pair / lineitem data is gathered per block in editable form and encoded at
  // the end; the raw form is 12 B/lineitem (transient peak ~1.5x the final).
  BlockEditor editor(*state);
  initial_scan->ScanPartsupps([&](const TpchQ20PartsuppInput &input) {
      editor.For(input.partkey)
          .SetAvail(SlotOf(input.partkey), input.suppkey,
                    input.availqty);
  });
  initial_scan->ScanLineitems([&](const TpchQ20LineitemInput &input) {
      editor.For(input.partkey)
          .AddLine(SlotOf(input.partkey), input.suppkey,
                   input.shipdate_days, input.quantity_raw);
  });
  editor.Flush(services);
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

void TpchQ20Module::EmitResult(const core::AuxEngineServices &services, const TpchQ20State &state,
                               const core::QueryArguments &arguments,
                               core::AuxRowSink &sink) const {
  auto empty = [&] {
    sink.Begin(core::OutputSchema{});
    sink.End();
  };

  // Any NULL argument makes a predicate NULL (never true): empty result.
  if (arguments.values.size() < 3)
    return empty();
  for (std::size_t i = 0; i < 3; ++i)
    if (arguments.values[i].is_null)
      return empty();
  int64_t start_days = 0;
  const auto &date_payload = arguments.values[0].payload;
  if (std::holds_alternative<core::AuxDate>(date_payload))
    start_days = std::get<core::AuxDate>(date_payload).days_since_epoch;
  else if (std::holds_alternative<int64_t>(date_payload))
    start_days = std::get<int64_t>(date_payload);
  else
    return empty();
  if (!std::holds_alternative<std::string>(arguments.values[1].payload) ||
      !std::holds_alternative<std::string>(arguments.values[2].payload))
    return empty();
  const auto &pattern = std::get<std::string>(arguments.values[1].payload);
  const auto &nation_name = std::get<std::string>(arguments.values[2].payload);

  // l_shipdate >= D AND l_shipdate < D + INTERVAL '1' YEAR (calendar year,
  // end-of-month clamped: Feb 29 + 1 year = Feb 28).
  const int64_t window_end = sql_date::AddYears(start_days, 1);

  std::vector<int32_t> target_nations;
  for (const auto &[key, name] : state.nations)
    if (NameEquals(name, nation_name))
      target_nations.push_back(key);
  if (target_nations.empty())
    return empty();

  const auto matcher = like::LikeMatcher::Compile(pattern, '\\', kCaseInsensitive,
                                                  /*utf8_underscore=*/true);
  const auto max_threads = static_cast<unsigned>(services.WorkerCount());
  std::vector<std::vector<int64_t>> qualifying(max_threads);

  // Parts whose name matches (SIMD / prefix scan of the name column), then for
  // each of their partsupp pairs a binary search for the shipdate window.
  state.part_names.ForEachMatch(matcher, core::Parallelism::OnPool(services), [&](unsigned worker, int64_t partkey) {
    const TpchQ20PartBlock *block = state.FindBlock(partkey);
    if (!block)
      return;
    const std::size_t slot = SlotOf(partkey);
    for (uint32_t j = block->pair_begin[slot]; j < block->pair_begin[slot + 1]; ++j) {
      const TpchQ20Pair &pair = block->pairs[j];
      if (!(pair.entry_begin & TpchQ20Pair::kHasAvail))
        continue;
      const uint32_t begin = pair.entry_begin & ~TpchQ20Pair::kHasAvail;
      const uint32_t end = j + 1 < block->pairs.size()
                               ? (block->pairs[j + 1].entry_begin & ~TpchQ20Pair::kHasAvail)
                               : static_cast<uint32_t>(block->entries.size());
      const auto *first = block->entries.data() + begin;
      const auto *last = block->entries.data() + end;
      const auto *lo = std::lower_bound(first, last, start_days,
                                        [](const TpchQ20Entry &e, int64_t d) { return e.shipdate < d; });
      const auto *hi = std::lower_bound(lo, last, window_end,
                                        [](const TpchQ20Entry &e, int64_t d) { return e.shipdate < d; });
      if (lo == hi)
        continue; // SUM over no rows is NULL: the comparison is not true
      __int128 sum = 0;
      for (const auto *e = lo; e != hi; ++e)
        sum += UnpackValue(e->qty, block->wide);
      if (AvailQtyGreaterThanHalfSum(UnpackValue(pair.availqty, block->wide), sum))
        qualifying[worker].push_back(pair.suppkey);
    }
  });

  std::vector<int64_t> suppkeys;
  for (auto &v : qualifying)
    suppkeys.insert(suppkeys.end(), v.begin(), v.end());
  std::sort(suppkeys.begin(), suppkeys.end());
  suppkeys.erase(std::unique(suppkeys.begin(), suppkeys.end()), suppkeys.end());

  std::vector<TpchQ20OutputRow> output_rows;
  for (const int64_t suppkey : suppkeys) {
    const TpchQ20Supplier *supplier = state.suppliers.Find(suppkey);
    if (!supplier ||
        std::find(target_nations.begin(), target_nations.end(), supplier->nationkey) ==
            target_nations.end())
      continue;
    output_rows.push_back({supplier->name, supplier->address});
  }
  std::sort(output_rows.begin(), output_rows.end(),
            [](const TpchQ20OutputRow &a, const TpchQ20OutputRow &b) {
              return NameLess(a.name, b.name);
            });

  sink.Begin(core::OutputSchema{});
  for (const auto &row : output_rows) {
    core::AuxRow out;
    out.values.reserve(2);
    out.values.push_back(core::AuxValue::Of(core::AuxType::String, row.name));
    out.values.push_back(core::AuxValue::Of(core::AuxType::String, row.address));
    sink.WriteRow(out);
  }
  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ20Module::CalibrationWrites() const {
  return TpchQ20CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
