#include "tpch/q9/mysql/aux/module.hpp"
#include "tpch/q9/calibration_writes.hpp"
#include "prims/pg_numeric.hpp"
#include "prims/sql_date.hpp"
#include "prims/sql_like.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

namespace like = core::sql_like;

// String semantics of the target DBMS (LIKE and ORDER BY nation): PostgreSQL is
// case-sensitive; the MySQL copy of this file sets this to true (default utf8mb4
// collations are case-insensitive). This is the only intentional difference
// between the postgres/ and mysql/ copies. '_' matches one UTF-8 character.
constexpr bool kCaseInsensitive = true;

constexpr std::size_t kParts = kTpchQ9BlockParts;
// Money / quantity columns as raw hundredths (decimal(15,2)).
bool Decimal2Cell(const core::AuxRow &row, std::size_t index, int64_t &raw) {
  if (index >= row.values.size() || row.values[index].is_null ||
      !std::holds_alternative<core::ScaledDecimal>(row.values[index].payload))
    return false;
  const auto decimal = std::get<core::ScaledDecimal>(row.values[index].payload);
  raw = decimal.value;
  for (int32_t s = decimal.scale; s < 2; ++s)
    if (__builtin_mul_overflow(raw, int64_t{10}, &raw))
      throw std::overflow_error("Q9 decimal overflows int64");
  for (int32_t s = decimal.scale; s > 2; --s) {
    if (raw % 10 != 0)
      throw std::domain_error("Q9 decimal has more than 2 decimals");
    raw /= 10;
  }
  return true;
}

// A NULL in any column the query joins on or computes with means the row can
// never contribute, so these readers return false and the row is ignored.
bool ReadLineitem(const core::AuxRow &row, TpchQ9LineitemDelta &d) {
  return core::IntCell(row, 0, d.partkey) && core::IntCell(row, 1, d.suppkey) &&
         core::IntCell(row, 2, d.orderkey) && Decimal2Cell(row, 3, d.quantity_raw) &&
         Decimal2Cell(row, 4, d.extendedprice_raw) &&
         Decimal2Cell(row, 5, d.discount_raw);
}

std::shared_ptr<const TpchQ9StateImage>
MakeStateImage(const std::shared_ptr<const TpchQ9State> &state) {
  auto image = std::make_shared<TpchQ9StateImage>();
  image->state = state;
  return image;
}

const TpchQ9StateImage *AsQ9Image(const core::StateImage &image) {
  const auto *payload = image.As<TpchQ9StateImage>();
  return payload && payload->state ? payload : nullptr;
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

// Used for the bulk build and for write batches: a block is decoded, edited
// and re-encoded once per batch.
struct EditBlock {
  std::array<std::vector<TpchQ9Cell>, kParts> cells;
  std::array<std::vector<TpchQ9PartsuppEntry>, kParts> ps;

  // cells[slot] += (drows, dprofit) for `group`; a cell whose row count drops to
  // 0 disappears (the group no longer has joined rows).
  void AddCell(std::size_t slot, uint32_t group, int64_t drows, int64_t dprofit) {
    auto &list = cells[slot];
    auto it = std::lower_bound(list.begin(), list.end(), group,
                               [](const TpchQ9Cell &c, uint32_t g) { return c.group < g; });
    if (it == list.end() || it->group != group) {
      if (drows <= 0)
        return; // removal of something never counted
      list.insert(it, TpchQ9Cell{group, static_cast<uint32_t>(drows), dprofit});
      return;
    }
    const int64_t rows = static_cast<int64_t>(it->rows) + drows;
    if (rows <= 0) {
      list.erase(it);
      return;
    }
    if (rows > static_cast<int64_t>(std::numeric_limits<uint32_t>::max()) ||
        __builtin_add_overflow(it->profit, dprofit, &it->profit))
      throw std::overflow_error("Q9 profit cell overflow");
    it->rows = static_cast<uint32_t>(rows);
  }
  // The cells and partsupp rows of `from` (a block of the same parts) added in.
  void Absorb(const EditBlock &from) {
    for (std::size_t slot = 0; slot < kParts; ++slot) {
      for (const auto &cell : from.cells[slot])
        AddCell(slot, cell.group, cell.rows, cell.profit);
      for (const auto &entry : from.ps[slot])
        SetPs(slot, entry.suppkey, entry.cost_raw);
    }
  }
  bool FindPs(std::size_t slot, int64_t suppkey, int64_t &cost) const {
    const auto &list = ps[slot];
    auto it = std::lower_bound(list.begin(), list.end(), suppkey,
                               [](const TpchQ9PartsuppEntry &e, int64_t s) { return e.suppkey < s; });
    if (it == list.end() || it->suppkey != suppkey)
      return false;
    cost = it->cost_raw;
    return true;
  }
  void SetPs(std::size_t slot, int64_t suppkey, int64_t cost) {
    auto &list = ps[slot];
    auto it = std::lower_bound(list.begin(), list.end(), suppkey,
                               [](const TpchQ9PartsuppEntry &e, int64_t s) { return e.suppkey < s; });
    if (it != list.end() && it->suppkey == suppkey)
      it->cost_raw = cost;
    else
      list.insert(it, TpchQ9PartsuppEntry{suppkey, cost});
  }
  void ErasePs(std::size_t slot, int64_t suppkey) {
    auto &list = ps[slot];
    auto it = std::lower_bound(list.begin(), list.end(), suppkey,
                               [](const TpchQ9PartsuppEntry &e, int64_t s) { return e.suppkey < s; });
    if (it != list.end() && it->suppkey == suppkey)
      list.erase(it);
  }
};

bool BlockFindPs(const TpchQ9PartBlock *block, std::size_t slot, int64_t suppkey,
                 int64_t &cost) {
  if (!block)
    return false;
  const auto *first = block->ps.data() + block->ps_begin[slot];
  const auto *last = block->ps.data() + block->ps_begin[slot + 1];
  const auto *it = std::lower_bound(first, last, suppkey,
                                    [](const TpchQ9PartsuppEntry &e, int64_t s) { return e.suppkey < s; });
  if (it == last || it->suppkey != suppkey)
    return false;
  cost = it->cost_raw;
  return true;
}

std::unique_ptr<EditBlock> DecodeBlock(const TpchQ9PartBlock *block) {
  auto edit = std::make_unique<EditBlock>();
  if (!block)
    return edit;
  for (std::size_t slot = 0; slot < kParts; ++slot) {
    edit->cells[slot].assign(block->cells.begin() + block->cell_begin[slot],
                             block->cells.begin() + block->cell_begin[slot + 1]);
    edit->ps[slot].assign(block->ps.begin() + block->ps_begin[slot],
                          block->ps.begin() + block->ps_begin[slot + 1]);
  }
  return edit;
}

// An empty block becomes nullptr.
std::shared_ptr<TpchQ9PartBlock> EncodeBlock(const EditBlock &edit) {
  std::size_t ncells = 0, nps = 0;
  for (std::size_t slot = 0; slot < kParts; ++slot) {
    ncells += edit.cells[slot].size();
    nps += edit.ps[slot].size();
  }
  if (ncells == 0 && nps == 0)
    return nullptr;
  auto block = std::make_shared<TpchQ9PartBlock>();
  block->cells.reserve(ncells);
  block->ps.reserve(nps);
  for (std::size_t slot = 0; slot < kParts; ++slot) {
    block->cell_begin[slot] = static_cast<uint32_t>(block->cells.size());
    block->ps_begin[slot] = static_cast<uint32_t>(block->ps.size());
    block->cells.insert(block->cells.end(), edit.cells[slot].begin(), edit.cells[slot].end());
    block->ps.insert(block->ps.end(), edit.ps[slot].begin(), edit.ps[slot].end());
  }
  block->cell_begin[kParts] = static_cast<uint32_t>(block->cells.size());
  block->ps_begin[kParts] = static_cast<uint32_t>(block->ps.size());
  return block;
}

constexpr int64_t kBlockParts = static_cast<int64_t>(kParts);
static_assert((kBlockParts & (kBlockParts - 1)) == 0);
// Floor division / modulo by the (power of two) block size: exact for negative keys.
constexpr int64_t BlockOf(int64_t key) { return key >> __builtin_ctzll(static_cast<uint64_t>(kBlockParts)); }
constexpr std::size_t SlotOf(int64_t key) { return static_cast<std::size_t>(key & (kBlockParts - 1)); }

// Blocks per encode slice: each copies a few thousand cells and prices (~50 us).
constexpr std::size_t kBlocksGrain = 16;

// Edited blocks by block index: the partial of one partition of a build scan.
class BlockEdits {
public:
  EditBlock &For(int64_t partkey) {
    auto &slot = blocks_[BlockOf(partkey)];
    if (!slot)
      slot = std::make_shared<EditBlock>();
    return *slot;
  }
  // Blocks only `from` has are taken over; the others add its rows to theirs.
  // Neighbouring partitions meet in at most one block, so this is O(blocks).
  void Merge(BlockEdits &&from) {
    for (auto entry : from.blocks_) {
      auto &slot = blocks_[entry.first];
      if (!slot)
        slot = std::move(entry.second);
      else
        slot->Absorb(*entry.second);
    }
    from.blocks_.Clear();
  }
  core::FlatHashMap<std::shared_ptr<EditBlock>> &Blocks() { return blocks_; }

private:
  core::FlatHashMap<std::shared_ptr<EditBlock>> blocks_;
};

// Blocks being edited, keyed by block index; re-encoded into the state by Flush.
class BlockEditor {
public:
  explicit BlockEditor(TpchQ9State &state) : state_(state) {}
  EditBlock &For(int64_t partkey) {
    auto &slot = edited_.Blocks()[BlockOf(partkey)];
    if (!slot)
      slot = DecodeBlock(state_.FindBlock(partkey));
    return *slot;
  }
  // Takes over blocks built from scratch; rows of blocks both have are added.
  void Absorb(BlockEdits &&built) { edited_.Merge(std::move(built)); }
  // Decodes the blocks of `partkeys` on the pool, ahead of the serial edits
  // that would decode them one by one.
  void Prefetch(const core::AuxEngineServices &services, const std::vector<int64_t> &partkeys) {
    std::vector<int64_t> blocks;
    blocks.reserve(partkeys.size());
    for (const int64_t partkey : partkeys)
      blocks.push_back(BlockOf(partkey));
    std::sort(blocks.begin(), blocks.end());
    blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());
    std::vector<std::shared_ptr<EditBlock>> decoded(blocks.size());
    core::ParallelEach(services, blocks.size(), kBlocksGrain, [&](std::size_t i) {
      decoded[i] = DecodeBlock(state_.blocks.Get(blocks[i]).get());
    });
    for (std::size_t i = 0; i < blocks.size(); ++i) {
      auto &slot = edited_.Blocks()[blocks[i]];
      if (!slot)
        slot = std::move(decoded[i]);
    }
  }
  // Blocks encode independently; they enter the state in iteration order.
  void Flush(const core::AuxEngineServices &services) {
    std::vector<std::pair<int64_t, std::shared_ptr<EditBlock>>> edits;
    edits.reserve(edited_.Blocks().size());
    for (auto entry : edited_.Blocks())
      edits.emplace_back(entry.first, entry.second);
    std::vector<std::shared_ptr<TpchQ9PartBlock>> encoded(edits.size());
    core::ParallelSlices(services, edits.size(), kBlocksGrain,
                         [&](std::size_t, std::size_t begin, std::size_t end) {
                           for (std::size_t i = begin; i < end; ++i)
                             encoded[i] = EncodeBlock(*edits[i].second);
                         });
    for (std::size_t i = 0; i < edits.size(); ++i)
      if (encoded[i] || state_.blocks.Get(edits[i].first))
        state_.blocks.Set(edits[i].first, encoded[i]);
    edited_.Blocks().Clear();
  }

private:
  TpchQ9State &state_;
  BlockEdits edited_;
};

// profit = extendedprice * (1 - discount) - supplycost * quantity, in 1e-4
// units (all four inputs are hundredths).
bool ProfitRaw(int64_t extendedprice_raw, int64_t discount_raw, int64_t supplycost_raw,
               int64_t quantity_raw, int64_t &out) {
  const __int128 revenue = static_cast<__int128>(extendedprice_raw) * (100 - static_cast<__int128>(discount_raw));
  const __int128 cost = static_cast<__int128>(supplycost_raw) * quantity_raw;
  const __int128 profit = revenue - cost;
  if (profit > std::numeric_limits<int64_t>::max() || profit < std::numeric_limits<int64_t>::min())
    throw std::overflow_error("Q9 lineitem profit overflows int64");
  out = static_cast<int64_t>(profit);
  return true;
}

// A dimension row whose join value changes (order year, supplier nation,
// supplycost) moves the profit of its existing lineitems between cells, which
// the lineitem deltas cannot express. Net effect per key: the published state
// vs. the last value after the batch.
bool HasJoinValueChange(const TpchQ9State &old, const TpchQ9TxnBuffer &buffer) {
  std::unordered_map<int64_t, std::optional<int32_t>> year_after;
  for (const auto &d : buffer.OrderDeltas())
    year_after[d.orderkey] = d.sign > 0 ? std::optional<int32_t>(d.order_year) : std::nullopt;
  std::unordered_map<int64_t, std::optional<int32_t>> nation_after;
  for (const auto &d : buffer.SupplierDeltas())
    nation_after[d.suppkey] = d.sign > 0 ? std::optional<int32_t>(d.nationkey) : std::nullopt;
  std::map<std::pair<int64_t, int64_t>, std::optional<int64_t>> cost_after;
  for (const auto &d : buffer.PartsuppDeltas())
    cost_after[{d.partkey, d.suppkey}] = d.sign > 0 ? std::optional<int64_t>(d.cost_raw) : std::nullopt;
  bool value_change = false;
  for (const auto &[key, after] : year_after) {
    int32_t before = 0;
    value_change |= after && old.LookupOrderYear(key, before) && before != *after;
  }
  for (const auto &[key, after] : nation_after) {
    int32_t before = 0;
    value_change |= after && old.LookupSupplierNation(key, before) && before != *after;
  }
  for (const auto &[key, after] : cost_after) {
    int64_t before = 0;
    value_change |= after &&
                    BlockFindPs(old.FindBlock(key.first), SlotOf(key.first), key.second, before) &&
                    before != *after;
  }
  return value_change;
}

// The batch's lineitems; inserts and deletes of the same lineitem cancel out.
struct BatchLineitems {
  std::vector<const TpchQ9LineitemDelta *> inserts, deletes;
};

BatchLineitems CancelLineitems(const TpchQ9TxnBuffer &buffer) {
  BatchLineitems lines;
  std::map<std::array<int64_t, 6>, std::vector<std::size_t>> pending;
  std::vector<bool> cancelled;
  for (const auto &d : buffer.LineitemDeltas()) {
    const std::array<int64_t, 6> key = {d.partkey, d.suppkey, d.orderkey, d.quantity_raw,
                                        d.extendedprice_raw, d.discount_raw};
    if (d.sign > 0) {
      pending[key].push_back(lines.inserts.size());
      lines.inserts.push_back(&d);
      cancelled.push_back(false);
    } else {
      auto it = pending.find(key);
      if (it != pending.end() && !it->second.empty()) {
        cancelled[it->second.back()] = true;
        it->second.pop_back();
      } else {
        lines.deletes.push_back(&d);
      }
    }
  }
  std::vector<const TpchQ9LineitemDelta *> kept;
  for (std::size_t i = 0; i < lines.inserts.size(); ++i)
    if (!cancelled[i])
      kept.push_back(lines.inserts[i]);
  lines.inserts.swap(kept);
  return lines;
}

// The blocks of every part edited below, decoded in parallel up front.
void PrepareBlocks(const core::AuxEngineServices &services, const TpchQ9TxnBuffer &buffer,
                   const BatchLineitems &lines, BlockEditor &editor) {
  std::vector<int64_t> partkeys;
  partkeys.reserve(lines.deletes.size() + lines.inserts.size() + buffer.PartsuppDeltas().size());
  for (const auto *d : lines.deletes)
    partkeys.push_back(d->partkey);
  for (const auto *d : lines.inserts)
    partkeys.push_back(d->partkey);
  for (const auto &d : buffer.PartsuppDeltas())
    partkeys.push_back(d.partkey);
  editor.Prefetch(services, partkeys);
}

// Deleted lineitems leave the cells they were counted in (resolved against the
// dimension values of the published state).
void RemoveDeletedLineitems(const TpchQ9State &old, TpchQ9State &updated, BlockEditor &editor,
                            const std::vector<const TpchQ9LineitemDelta *> &deletes) {
  for (const auto *d : deletes) {
    int32_t nationkey = 0, year = 0;
    int64_t cost = 0, profit = 0;
    const auto slot = SlotOf(d->partkey);
    if (!old.LookupSupplierNation(d->suppkey, nationkey) ||
        !old.LookupOrderYear(d->orderkey, year) ||
        !BlockFindPs(old.FindBlock(d->partkey), slot, d->suppkey, cost))
      continue; // never joined: nothing to remove
    ProfitRaw(d->extendedprice_raw, d->discount_raw, cost, d->quantity_raw, profit);
    editor.For(d->partkey).AddCell(slot, updated.GroupFor(nationkey, year), -1, -profit);
  }
}

// Dimension changes (value changes were ruled out by HasJoinValueChange).
void ApplyDimensionChanges(const TpchQ9TxnBuffer &buffer, TpchQ9State &updated,
                           BlockEditor &editor) {
  for (const auto &d : buffer.SupplierDeltas()) {
    if (d.sign > 0)
      updated.SetSupplierNation(d.suppkey, d.nationkey);
    else
      updated.EraseSupplier(d.suppkey);
  }
  for (const auto &d : buffer.OrderDeltas()) {
    if (d.sign > 0)
      updated.SetOrderYear(d.orderkey, d.order_year);
    else
      updated.EraseOrder(d.orderkey);
  }
  for (const auto &d : buffer.PartsuppDeltas()) {
    auto &edit = editor.For(d.partkey);
    const auto slot = SlotOf(d.partkey);
    if (d.sign > 0)
      edit.SetPs(slot, d.suppkey, d.cost_raw);
    else
      edit.ErasePs(slot, d.suppkey);
  }
  for (const auto &d : buffer.PartDeltas()) {
    if (d.sign > 0)
      updated.part_names.Set(d.partkey, d.name);
    else
      updated.part_names.Erase(d.partkey);
  }
  for (const auto &d : buffer.NationDeltas()) {
    if (d.sign > 0)
      updated.nation_names[d.nationkey] = d.name;
    else
      updated.nation_names.erase(d.nationkey);
  }
}

// Inserted lineitems join against the post-batch dimensions.
void AddInsertedLineitems(TpchQ9State &updated, BlockEditor &editor,
                          const std::vector<const TpchQ9LineitemDelta *> &inserts) {
  for (const auto *d : inserts) {
    int32_t nationkey = 0, year = 0;
    int64_t cost = 0, profit = 0;
    auto &edit = editor.For(d->partkey);
    const auto slot = SlotOf(d->partkey);
    if (!updated.LookupSupplierNation(d->suppkey, nationkey) ||
        !updated.LookupOrderYear(d->orderkey, year) || !edit.FindPs(slot, d->suppkey, cost))
      continue; // does not join (yet)
    ProfitRaw(d->extendedprice_raw, d->discount_raw, cost, d->quantity_raw, profit);
    edit.AddCell(slot, updated.GroupFor(nationkey, year), 1, profit);
  }
}

} // namespace
int32_t TpchQ9ExtractYear(int32_t days_since_epoch) {
  return core::sql_date::YearOf(days_since_epoch);
}

static int64_t GroupKey(int32_t nationkey, int32_t year) {
  return (static_cast<int64_t>(nationkey) << 32) | static_cast<uint32_t>(year);
}

const uint32_t *TpchQ9State::FindGroup(int32_t nationkey, int32_t year) const {
  return group_index.Find(GroupKey(nationkey, year));
}

uint32_t TpchQ9State::GroupFor(int32_t nationkey, int32_t year) {
  if (const uint32_t *found = FindGroup(nationkey, year))
    return *found;
  if (groups.size() >= std::numeric_limits<uint32_t>::max())
    throw std::length_error("Q9 group dictionary overflow");
  const auto id = static_cast<uint32_t>(groups.size());
  groups.emplace_back(nationkey, year);
  group_index.Upsert(GroupKey(nationkey, year), id);
  return id;
}

bool TpchQ9State::LookupSupplierNation(int64_t suppkey, int32_t &nationkey) const {
  const int64_t v = supplier_nation.Get(suppkey);
  if (v == kNoNation)
    return false;
  nationkey = static_cast<int32_t>(v);
  return true;
}

void TpchQ9State::SetSupplierNation(int64_t suppkey, int32_t nationkey) {
  supplier_nation.Set(suppkey, nationkey);
}

void TpchQ9State::EraseSupplier(int64_t suppkey) {
  if (supplier_nation.Get(suppkey) != kNoNation)
    supplier_nation.Set(suppkey, kNoNation);
}

bool TpchQ9State::LookupOrderYear(int64_t orderkey, int32_t &year) const {
  const uint8_t code = order_year_code.Get(orderkey);
  if (code != 0) {
    year = year_table[code - 1];
    return true;
  }
  if (const int32_t *overflow = order_year_overflow.Find(orderkey)) {
    year = *overflow;
    return true;
  }
  return false;
}

// Years are interned into a table of at most 255 entries (first come first
// served); orders whose year no longer fits keep it in a hash overflow.
void TpchQ9State::SetOrderYear(int64_t orderkey, int32_t year) {
  EraseOrder(orderkey);
  std::size_t code = 0;
  for (std::size_t i = 0; i < year_table.size(); ++i)
    if (year_table[i] == year)
      code = i + 1;
  if (code == 0 && year_table.size() < kMaxYearCodes) {
    year_table.push_back(year);
    code = year_table.size();
  }
  if (code != 0)
    order_year_code.Set(orderkey, static_cast<uint8_t>(code));
  else
    order_year_overflow.Upsert(orderkey, year);
}

void TpchQ9State::EraseOrder(int64_t orderkey) {
  if (order_year_code.Get(orderkey) != 0)
    order_year_code.Set(orderkey, 0);
  order_year_overflow.Erase(orderkey);
}

bool TpchQ9TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ9TxnBuffer::Clear() {
  pending = false;
  lineitems.clear();
  orders.clear();
  suppliers.clear();
  partsupps.clear();
  parts.clear();
  nations.clear();
}

void TpchQ9TxnBuffer::Add(TpchQ9LineitemDelta delta) {
  pending = true;
  lineitems.push_back(delta);
}

void TpchQ9TxnBuffer::Add(TpchQ9OrderDelta delta) {
  pending = true;
  orders.push_back(delta);
}

void TpchQ9TxnBuffer::Add(TpchQ9SupplierDelta delta) {
  pending = true;
  suppliers.push_back(delta);
}

void TpchQ9TxnBuffer::Add(TpchQ9PartsuppDelta delta) {
  pending = true;
  partsupps.push_back(delta);
}

void TpchQ9TxnBuffer::Add(TpchQ9PartDelta delta) {
  pending = true;
  parts.push_back(std::move(delta));
}

void TpchQ9TxnBuffer::Add(TpchQ9NationDelta delta) {
  pending = true;
  nations.push_back(std::move(delta));
}

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

std::shared_ptr<TpchQ9State>
TpchQ9Module::BuildState(const core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q9 aux requires a typed initial scan source");
  auto state = std::make_shared<TpchQ9State>();
  const std::size_t partitions = core::BuildPartitions(services);

  initial_scan->ScanNations([&](const TpchQ9NationInput &input) {
    state->nation_names[input.nationkey] = input.name;
  });
  std::set<int32_t> nationkeys, years;
  for (const auto &input : core::CollectRows<TpchQ9SupplierInput>(
           services, [&](std::size_t parts, auto &&sink) {
             initial_scan->ScanSuppliersPartitioned(services, parts, sink);
           })) {
    state->SetSupplierNation(input.suppkey, input.nationkey);
    nationkeys.insert(input.nationkey);
  }
  for (const auto &input : core::CollectRows<TpchQ9OrdersInput>(
           services, [&](std::size_t parts, auto &&sink) {
             initial_scan->ScanOrdersPartitioned(services, parts, sink);
           })) {
    const int32_t year = TpchQ9ExtractYear(input.orderdate_days);
    state->SetOrderYear(input.orderkey, year);
    years.insert(year);
  }
  for (const auto &input : core::CollectRows<TpchQ9PartInput>(
           services, [&](std::size_t parts, auto &&sink) {
             initial_scan->ScanPartsPartitioned(services, parts, sink);
           }))
    state->part_names.Set(input.partkey, input.name);
  // Every (nation, year) group is interned up front: the partitions below look
  // groups up without adding any.
  for (const int32_t nationkey : nationkeys)
    for (const int32_t year : years)
      state->GroupFor(nationkey, year);

  // Partsupp rows and profit cells are gathered per block in editable form and
  // encoded at the end (transient: the editable copy of the cells, ~1.3x).
  // Partitions are ranges of the partkey, so they share at most their end blocks.
  BlockEditor editor(*state);
  editor.Absorb(core::PartitionedBuild<BlockEdits>(
      services, [] { return BlockEdits{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanPartsuppsPartitioned(
            services, parts, [&](std::size_t partition, const TpchQ9PartsuppInput &input) {
              sink(partition, core::SingleRowBatch<TpchQ9PartsuppInput>{input});
            });
      },
      [](BlockEdits &into, const TpchQ9PartsuppInput &input) {
        into.For(input.partkey).SetPs(SlotOf(input.partkey), input.suppkey, input.supplycost_raw);
      },
      [](BlockEdits &into, BlockEdits &&from) { into.Merge(std::move(from)); }, partitions));

  bool pushed_down = false;
  editor.Absorb(core::PartitionedBuild<BlockEdits>(
      services, [] { return BlockEdits{}; },
      [&](std::size_t parts, auto &&sink) {
        pushed_down = initial_scan->ScanProfitContributionsPartitioned(
            services, parts, [&](std::size_t partition, const TpchQ9ProfitContribution &input) {
              sink(partition, core::SingleRowBatch<TpchQ9ProfitContribution>{input});
            });
      },
      [&](BlockEdits &into, const TpchQ9ProfitContribution &input) {
        if (input.rows <= 0)
          return;
        const uint32_t *group = state->FindGroup(input.nationkey, input.year);
        if (!group)
          throw std::runtime_error("TPC-H Q9: profit group outside the scanned nations / years");
        into.For(input.partkey).AddCell(SlotOf(input.partkey), *group, input.rows, input.profit_raw);
      },
      [](BlockEdits &into, BlockEdits &&from) { into.Merge(std::move(from)); }, partitions));
  if (!pushed_down) {
    initial_scan->ScanLineitems([&](const TpchQ9LineitemInput &input) {
      int32_t nationkey = 0, year = 0;
      if (!state->LookupSupplierNation(input.suppkey, nationkey) ||
          !state->LookupOrderYear(input.orderkey, year))
        return;
      auto &edit = editor.For(input.partkey);
      const auto slot = SlotOf(input.partkey);
      int64_t cost = 0, profit = 0;
      if (!edit.FindPs(slot, input.suppkey, cost))
        return;
      ProfitRaw(input.extendedprice_raw, input.discount_raw, cost, input.quantity_raw,
                profit);
      edit.AddCell(slot, state->GroupFor(nationkey, year), 1, profit);
    });
  }
  editor.Flush(services);
  return state;
}

void TpchQ9Module::Build(core::AuxEngineServices &services, core::StateBuilder &builder) {
  const auto state = BuildState(services);
  builder.Write(core::StateImage::Make(
      TpchQ9Layout::kAccelerator, TpchQ9Layout::kState, 0,
      MakeStateImage(state)));
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
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  return {};
}

void TpchQ9Module::AccumulateMutation(const core::MutationBatch &mutation,
                                      core::AuxEngineServices &,
                                      core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ9TxnBuffer &>(base_buffer);

  std::function<void(const core::AuxRow &, int8_t)> add;
  if (mutation.table == config.part_table) {
    add = [&](const core::AuxRow &row, int8_t sign) {
      TpchQ9PartDelta d;
      d.sign = sign;
      if (core::IntCell(row, 0, d.partkey) && core::StringCell(row, 1, d.name))
        buffer.Add(std::move(d));
    };
  } else if (mutation.table == config.supplier_table) {
    add = [&](const core::AuxRow &row, int8_t sign) {
      TpchQ9SupplierDelta d;
      d.sign = sign;
      if (core::IntCell(row, 0, d.suppkey) && core::Int32Cell(row, 1, d.nationkey))
        buffer.Add(d);
    };
  } else if (mutation.table == config.nation_table) {
    add = [&](const core::AuxRow &row, int8_t sign) {
      TpchQ9NationDelta d;
      d.sign = sign;
      if (core::Int32Cell(row, 0, d.nationkey) && core::StringCell(row, 1, d.name))
        buffer.Add(std::move(d));
    };
  } else if (mutation.table == config.orders_table) {
    // RF1 inserts orders before their lineitems; the batch applies order
    // changes first, so Stage resolves the new lineitems against them.
    add = [&](const core::AuxRow &row, int8_t sign) {
      TpchQ9OrderDelta d;
      d.sign = sign;
      int32_t days = 0;
      if (core::IntCell(row, 0, d.orderkey) && core::Int32Cell(row, 1, days)) {
        d.order_year = TpchQ9ExtractYear(days);
        buffer.Add(d);
      }
    };
  } else if (mutation.table == config.partsupp_table) {
    add = [&](const core::AuxRow &row, int8_t sign) {
      TpchQ9PartsuppDelta d;
      d.sign = sign;
      if (core::IntCell(row, 0, d.partkey) && core::IntCell(row, 1, d.suppkey) &&
          Decimal2Cell(row, 2, d.cost_raw))
        buffer.Add(d);
    };
  } else if (mutation.table == config.lineitem_table) {
    add = [&](const core::AuxRow &row, int8_t sign) {
      TpchQ9LineitemDelta d;
      d.sign = sign;
      if (ReadLineitem(row, d))
        buffer.Add(d);
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

core::StagedMaintenanceOutcome
TpchQ9Module::StageMaintenance(core::AuxEngineServices &services,
                               const core::InvalidationPlan &,
                               core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ9TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;

  const uint64_t total_changes = buffer.LineitemDeltas().size() + buffer.OrderDeltas().size() +
                                 buffer.SupplierDeltas().size() + buffer.PartsuppDeltas().size() +
                                 buffer.PartDeltas().size() + buffer.NationDeltas().size();
  if (total_changes == 0)
    return outcome;

  const auto decision = services.ChooseMaintenance(
      core::MaintenanceRequest::IncrementalSingleState(
          total_changes, services.ExpectedQueriesBeforeNextWrite()));
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    core::FinishRebuildAll(outcome, decision);
    return outcome;
  }

  std::shared_ptr<const TpchQ9State> old_state;
  core::Version existing_version = 0;
  if (!LoadState(services, old_state, &existing_version) ||
      HasJoinValueChange(*old_state, buffer)) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }
  const TpchQ9State &old = *old_state;
  const BatchLineitems lines = CancelLineitems(buffer);

  // Copy-on-write: shares every block / chunk / shard with the published state;
  // only the touched ones are cloned below.
  auto updated = std::make_shared<TpchQ9State>(old);
  BlockEditor editor(*updated);
  PrepareBlocks(services, buffer, lines, editor);
  RemoveDeletedLineitems(old, *updated, editor, lines.deletes);
  ApplyDimensionChanges(buffer, *updated, editor);
  AddInsertedLineitems(*updated, editor, lines.inserts);
  editor.Flush(services);

  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.updates.push_back(core::StateImage::Make(
      TpchQ9Layout::kAccelerator, TpchQ9Layout::kState, existing_version,
      MakeStateImage(std::static_pointer_cast<const TpchQ9State>(updated))));
  return outcome;
}

bool TpchQ9Module::CheckValidity(core::AuxEngineServices &services,
                                 const core::QueryArguments &arguments) const {
  if (arguments.values.empty())
    return false;
  return services.StateStore()
      .CheckValidity({Id(), std::to_string(TpchQ9Layout::kState)},
                     services.CurrentVisibility())
      .IsValid();
}

void TpchQ9Module::Execute(core::AuxEngineServices &services,
                           const core::QueryArguments &arguments,
                           core::AuxRowSink &sink) {
  if (!CheckValidity(services, arguments)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  std::shared_ptr<const TpchQ9State> state;
  if (!LoadState(services, state)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  EmitResult(services, *state, arguments, sink);
}

bool TpchQ9Module::LoadState(core::AuxEngineServices &services,
                             std::shared_ptr<const TpchQ9State> &state,
                             core::Version *version) const {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {TpchQ9Layout::kState});
  for (const auto &image : snapshot.partitions) {
    if (image.partition != TpchQ9Layout::kState)
      continue;
    const auto *typed = AsQ9Image(image);
    if (!typed)
      return false;
    state = typed->state;
    if (version)
      *version = image.version;
    return true;
  }
  return false;
}

void TpchQ9Module::EmitResult(const core::AuxEngineServices &services, const TpchQ9State &state,
                              const core::QueryArguments &arguments,
                              core::AuxRowSink &sink) const {
  auto empty = [&] {
    sink.Begin(core::OutputSchema{});
    sink.End();
  };
  // p_name LIKE NULL is never true.
  if (arguments.values.empty() || arguments.values[0].is_null ||
      !std::holds_alternative<std::string>(arguments.values[0].payload))
    return empty();
  // The template is `p_name LIKE '%<color>%'`: the argument is spliced into the
  // pattern as is, so any LIKE metacharacter in it keeps its meaning.
  const std::string pattern = "%" + std::get<std::string>(arguments.values[0].payload) + "%";
  const auto matcher = like::LikeMatcher::Compile(pattern, '\\', kCaseInsensitive,
                                                  /*utf8_underscore=*/true);

  struct Acc {
    __int128 profit = 0;
    int64_t rows = 0;
  };
  const std::size_t ngroups = state.groups.size();
  const auto max_threads = static_cast<unsigned>(services.WorkerCount());
  std::vector<std::vector<Acc>> acc(max_threads, std::vector<Acc>(ngroups));

  // Matching parts (SIMD scan of the name column), then their profit cells.
  state.part_names.ForEachMatch(matcher, core::Parallelism::OnPool(services), [&](unsigned worker, int64_t partkey) {
    const TpchQ9PartBlock *block = state.FindBlock(partkey);
    if (!block)
      return;
    const auto slot = SlotOf(partkey);
    auto &mine = acc[worker];
    for (uint32_t i = block->cell_begin[slot]; i < block->cell_begin[slot + 1]; ++i) {
      mine[block->cells[i].group].profit += block->cells[i].profit;
      mine[block->cells[i].group].rows += block->cells[i].rows;
    }
  });

  // GROUP BY n_name, year: groups of nations that share a name merge, and a
  // supplier nation without a nation row joins nothing.
  std::map<std::pair<std::string, int32_t>, __int128> sums;
  for (std::size_t g = 0; g < ngroups; ++g) {
    Acc total;
    for (const auto &worker : acc) {
      total.profit += worker[g].profit;
      total.rows += worker[g].rows;
    }
    if (total.rows == 0)
      continue;
    const auto nation = state.nation_names.find(state.groups[g].first);
    if (nation == state.nation_names.end())
      continue;
    sums[{nation->second, state.groups[g].second}] += total.profit;
  }

  std::vector<std::pair<std::pair<std::string, int32_t>, __int128>> rows(sums.begin(), sums.end());
  std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
    if (a.first.first != b.first.first)
      return NameLess(a.first.first, b.first.first);
    return a.first.second > b.first.second;
  });

  sink.Begin(core::OutputSchema{});
  for (const auto &[key, profit] : rows) {
    core::AuxRow out;
    out.values.reserve(3);
    out.values.push_back(core::AuxValue::Of(core::AuxType::String, key.first));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::Int64, static_cast<int64_t>(key.second)));
    if (profit >= std::numeric_limits<int64_t>::min() &&
        profit <= std::numeric_limits<int64_t>::max())
      out.values.push_back(core::AuxValue::Of(
          core::AuxType::ScaledDecimal, core::ScaledDecimal{static_cast<int64_t>(profit), 4}));
    else // beyond int64: exact numeric text
      out.values.push_back(core::AuxValue::Of(
          core::AuxType::String, accelerator_codegen::pg_numeric::ToText(profit, 4)));
    sink.WriteRow(out);
  }
  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ9Module::CalibrationWrites() const {
  return TpchQ9CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
