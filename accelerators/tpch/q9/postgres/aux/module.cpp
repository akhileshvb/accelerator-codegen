#include "tpch/q9/postgres/aux/module.hpp"
#include "tpch/q9/calibration_writes.hpp"
#include "prims/pg_numeric.hpp"
#include "prims/sql_date.hpp"
#include "prims/sql_like.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

namespace like = core::sql_like;

// String semantics of the target DBMS (LIKE and ORDER BY nation): PostgreSQL is
// case-sensitive; the MySQL copy of this file sets this to true (default utf8mb4
// collations are case-insensitive). This is the only intentional difference
// between the postgres/ and mysql/ copies. '_' matches one UTF-8 character.
constexpr bool kCaseInsensitive = false;

constexpr std::size_t kParts = kTpchQ9BlockParts;

// Part blocks per color-index slice: ~16 us per block of 16 parts.
constexpr std::size_t kColorBlockGrain = 128;
// Blocks per slice: ~5 us to decode one, ~8 us to encode one and diff its colors.
constexpr std::size_t kDecodeBlockGrain = 256;
constexpr std::size_t kFlushBlockGrain = 128;
// Lineitems resolved per slice: ~0.3 us each (three lookups).
constexpr std::size_t kResolveGrain = 4096;
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
  uint32_t name_dirty = 0; // slots whose p_name changed in this batch

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

// TPC-H spec 4.2.3 P_NAME words (the Q9 substitution parameter domain).
constexpr std::array<const char *, kTpchQ9Colors> kColors = {
    "almond", "antique", "aquamarine", "azure", "beige", "bisque", "black", "blanched", "blue", "blush", "brown",
    "burlywood", "burnished", "chartreuse", "chiffon", "chocolate", "coral", "cornflower", "cornsilk", "cream",
    "cyan", "dark", "deep", "dim", "dodger", "drab", "firebrick", "floral", "forest", "frosted", "gainsboro",
    "ghost", "goldenrod", "green", "grey", "honeydew", "hot", "indian", "ivory", "khaki", "lace", "lavender",
    "lawn", "lemon", "light", "lime", "linen", "magenta", "maroon", "medium", "metallic", "midnight", "mint",
    "misty", "moccasin", "navajo", "navy", "olive", "orange", "orchid", "pale", "papaya", "peach", "peru", "pink",
    "plum", "powder", "puff", "purple", "red", "rose", "rosy", "royal", "saddle", "salmon", "sandy", "seashell",
    "sienna", "sky", "slate", "smoke", "snow", "spring", "steel", "tan", "thistle", "tomato", "turquoise",
    "violet", "wheat", "white", "yellow"};

// Bit c set <=> kColors[c] occurs in the name (== p_name LIKE '%kColors[c]%':
// the words are lowercase ASCII without LIKE metacharacters; PostgreSQL LIKE is
// case-sensitive, kCaseInsensitive is false in this copy).
using ColorMask = std::array<uint64_t, 2>;
static_assert(kTpchQ9Colors <= 128);

// kColors is sorted, so the words starting with one byte are a contiguous
// range; the tables are constexpr (.rodata, ~1 KB) to stay cheap on a cold
// cache (maintenance computes a mask per changed part).
static_assert([] {
  for (std::size_t c = 1; c < kTpchQ9Colors; ++c)
    if (std::string_view(kColors[c - 1]) >= std::string_view(kColors[c]))
      return false;
  return true;
}());
struct ColorRanges {
  std::array<uint8_t, 256> begin{}, end{};
  std::array<uint8_t, kTpchQ9Colors> length{};
};

constexpr ColorRanges kColorRanges = [] {
  ColorRanges r;
  for (std::size_t c = kTpchQ9Colors; c-- > 0;) {
    const auto first = static_cast<unsigned char>(kColors[c][0]);
    if (r.end[first] == 0)
      r.end[first] = static_cast<uint8_t>(c + 1);
    r.begin[first] = static_cast<uint8_t>(c);
    r.length[c] = static_cast<uint8_t>(std::string_view(kColors[c]).size());
  }
  return r;
}();

ColorMask MaskOfName(std::string_view name) {
  static_assert(!kCaseInsensitive, "color index assumes case-sensitive LIKE");
  ColorMask mask{0, 0};
  const char *data = name.data();
  const std::size_t n = name.size();
  for (std::size_t i = 0; i < n; ++i) {
    const auto b = static_cast<unsigned char>(data[i]);
    for (std::size_t c = kColorRanges.begin[b]; c < kColorRanges.end[b]; ++c) {
      const std::size_t len = kColorRanges.length[c];
      if (n - i >= len && std::memcmp(data + i, kColors[c], len) == 0)
        mask[c >> 6] |= uint64_t{1} << (c & 63);
    }
  }
  return mask;
}

// Absent part (no p_name row): joins no LIKE, contributes to no color.
ColorMask MaskOfPart(const TpchQ9State &state, int64_t partkey) {
  std::string_view name;
  return state.part_names.Get(partkey, name) ? MaskOfName(name) : ColorMask{0, 0};
}

int ColorOf(std::string_view argument) {
  for (std::size_t c = 0; c < kTpchQ9Colors; ++c)
    if (argument == kColors[c])
      return static_cast<int>(c);
  return -1;
}

template <typename F> void ForEachColor(const ColorMask &mask, F &&fn) {
  for (std::size_t w = 0; w < 2; ++w)
    for (uint64_t bits = mask[w]; bits; bits &= bits - 1)
      fn((w << 6) | static_cast<std::size_t>(__builtin_ctzll(bits)));
}

// Net change of the color aggregates of a batch, dense per (group, color), in
// wide sums so that the partial results of slices add up exactly.
class ColorDeltas {
public:
  struct Acc {
    int64_t rows = 0;
    __int128 profit = 0;
  };

  void Reserve(std::size_t groups) {
    if (cells_.empty())
      cells_.resize(groups * kTpchQ9Colors);
  }
  void Add(const ColorMask &mask, uint32_t group, int64_t drows, int64_t dprofit) {
    if (mask[0] == 0 && mask[1] == 0)
      return;
    Acc *row = cells_.data() + std::size_t{group} * kTpchQ9Colors;
    ForEachColor(mask, [&](std::size_t c) {
      row[c].rows += drows;
      row[c].profit += dprofit;
    });
  }
  void Merge(ColorDeltas &&from) {
    if (from.cells_.empty())
      return;
    Reserve(from.cells_.size() / kTpchQ9Colors);
    for (std::size_t i = 0; i < cells_.size(); ++i) {
      cells_[i].rows += from.cells_[i].rows;
      cells_[i].profit += from.cells_[i].profit;
    }
  }
  std::size_t groups() const { return cells_.size() / kTpchQ9Colors; }
  const Acc *Row(std::size_t group) const { return cells_.data() + group * kTpchQ9Colors; }

private:
  std::vector<Acc> cells_;
};

// Applies a batch's color changes; the first write to a group clones its
// record (the published state keeps the old one). A sum beyond int64 drops the
// index.
void ApplyColorDeltas(TpchQ9State &state, const ColorDeltas &deltas) {
  auto &groups = state.color_groups;
  for (std::size_t g = 0; g < deltas.groups(); ++g) {
    const ColorDeltas::Acc *delta = deltas.Row(g);
    if (std::all_of(delta, delta + kTpchQ9Colors,
                    [](const ColorDeltas::Acc &d) { return d.rows == 0 && d.profit == 0; }))
      continue;
    if (g >= groups.size())
      groups.resize(state.groups.size());
    auto record = groups[g] ? std::make_shared<TpchQ9GroupColors>(*groups[g])
                            : std::make_shared<TpchQ9GroupColors>();
    for (std::size_t c = 0; c < kTpchQ9Colors; ++c) {
      const __int128 profit = record->by_color[c].profit + delta[c].profit;
      if (profit > std::numeric_limits<int64_t>::max() ||
          profit < std::numeric_limits<int64_t>::min()) {
        state.color_index = false;
        groups.clear();
        return;
      }
      record->by_color[c].rows += delta[c].rows;
      record->by_color[c].profit = static_cast<int64_t>(profit);
    }
    groups[g] = std::move(record);
  }
}

// Orders, suppliers and parts whose join changes are re-counted through index
// lookups of their lineitems; more than this (a bulk dimension update) rebuild.
constexpr std::size_t kMaxJoinChangeKeys = 200000;

constexpr int64_t kBlockParts = static_cast<int64_t>(kParts);
static_assert((kBlockParts & (kBlockParts - 1)) == 0);
// Floor division / modulo by the (power of two) block size: exact for negative keys.
constexpr int64_t BlockOf(int64_t key) { return key >> __builtin_ctzll(static_cast<uint64_t>(kBlockParts)); }
constexpr std::size_t SlotOf(int64_t key) { return static_cast<std::size_t>(key & (kBlockParts - 1)); }

// Blocks being edited, keyed by block index; re-encoded into the state by Flush.
// With `before` (the published state of a write batch) and a state carrying the
// color index, Flush also applies each edited part's cell diff to the color
// aggregates (parts renamed in the batch must be marked with MarkRenamed).
class BlockEditor {
public:
  explicit BlockEditor(TpchQ9State &state, const TpchQ9State *before = nullptr)
      : state_(state), before_(before) {}
  EditBlock &For(int64_t partkey) {
    auto &slot = edited_[BlockOf(partkey)];
    if (!slot)
      slot = DecodeBlock(state_.FindBlock(partkey));
    return *slot;
  }
  // The block being edited, if any (does not create it).
  EditBlock *Find(int64_t partkey) const {
    const auto *slot = edited_.Find(BlockOf(partkey));
    return slot ? slot->get() : nullptr;
  }
  void MarkRenamed(int64_t partkey) { For(partkey).name_dirty |= uint32_t{1} << SlotOf(partkey); }
  // Takes over a block edited elsewhere (by a single owner).
  void Adopt(int64_t block_index, std::shared_ptr<EditBlock> edit) {
    edited_[block_index] = std::move(edit);
  }
  // Decodes the blocks of `partkeys` in parallel, so that the edits that follow
  // (in slices or not) only find them.
  void Prepare(const core::AuxEngineServices &services, const std::vector<int64_t> &partkeys) {
    std::vector<int64_t> blocks;
    for (const int64_t partkey : partkeys)
      if (!edited_.Contains(BlockOf(partkey)))
        blocks.push_back(BlockOf(partkey));
    std::sort(blocks.begin(), blocks.end());
    blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());
    std::vector<std::shared_ptr<EditBlock>> decoded(blocks.size());
    core::ParallelEach(services, blocks.size(), kDecodeBlockGrain, [&](std::size_t i) {
      decoded[i] = DecodeBlock(state_.blocks.Get(blocks[i]).get());
    });
    for (std::size_t i = 0; i < blocks.size(); ++i)
      Adopt(blocks[i], std::move(decoded[i]));
  }
  // Encodes the edited blocks (and diffs their colors) in parallel slices, then
  // writes them into the state.
  void Flush(const core::AuxEngineServices &services) {
    const bool index = before_ && state_.color_index;
    std::vector<std::pair<int64_t, const EditBlock *>> entries;
    entries.reserve(edited_.size());
    for (auto entry : edited_)
      entries.emplace_back(entry.first, entry.second.get());
    std::vector<std::shared_ptr<TpchQ9PartBlock>> encoded(entries.size());
    ColorDeltas colors;
    core::ParallelReduce(
        services, entries.size(), kFlushBlockGrain, colors,
        [&](ColorDeltas &acc, std::size_t begin, std::size_t end) {
          if (index)
            acc.Reserve(state_.groups.size());
          for (std::size_t i = begin; i < end; ++i) {
            const auto [block_index, edit] = entries[i];
            if (index) // state_.blocks still holds the published block here
              DiffColors(block_index, state_.blocks.Get(block_index).get(), *edit, acc);
            encoded[i] = EncodeBlock(*edit);
          }
        },
        [](ColorDeltas &into, ColorDeltas &&from) { into.Merge(std::move(from)); });
    for (std::size_t i = 0; i < entries.size(); ++i)
      if (encoded[i] || state_.blocks.Get(entries[i].first))
        state_.blocks.Set(entries[i].first, encoded[i]);
    if (index)
      ApplyColorDeltas(state_, colors);
    edited_.Clear();
  }

private:
  void DiffColors(int64_t block_index, const TpchQ9PartBlock *old_block, const EditBlock &edit,
                  ColorDeltas &colors) const {
    for (std::size_t slot = 0; slot < kParts; ++slot) {
      const TpchQ9Cell *ob = old_block ? old_block->cells.data() + old_block->cell_begin[slot] : nullptr;
      const TpchQ9Cell *oe = old_block ? old_block->cells.data() + old_block->cell_begin[slot + 1] : nullptr;
      const auto &now = edit.cells[slot];
      const bool renamed = (edit.name_dirty >> slot) & 1;
      const std::size_t nold = static_cast<std::size_t>(oe - ob);
      if (nold == 0 && now.empty())
        continue;
      if (!renamed && nold == now.size() &&
          std::equal(ob, oe, now.begin(), [](const TpchQ9Cell &a, const TpchQ9Cell &b) {
            return a.group == b.group && a.rows == b.rows && a.profit == b.profit;
          }))
        continue;
      const int64_t partkey = block_index * kBlockPartsI + static_cast<int64_t>(slot);
      const ColorMask old_mask = MaskOfPart(*before_, partkey);
      const ColorMask new_mask = renamed ? MaskOfPart(state_, partkey) : old_mask;
      if (old_mask == new_mask) {
        if (old_mask[0] == 0 && old_mask[1] == 0)
          continue;
        // Merge the two group-sorted lists; apply only the differences.
        auto it = now.begin();
        for (const TpchQ9Cell *o = ob; o != oe || it != now.end();) {
          if (it == now.end() || (o != oe && o->group < it->group)) {
            colors.Add(old_mask, o->group, -static_cast<int64_t>(o->rows), -o->profit);
            ++o;
          } else if (o == oe || it->group < o->group) {
            colors.Add(old_mask, it->group, it->rows, it->profit);
            ++it;
          } else {
            if (o->rows != it->rows || o->profit != it->profit)
              colors.Add(old_mask, o->group,
                         static_cast<int64_t>(it->rows) - static_cast<int64_t>(o->rows),
                         it->profit - o->profit);
            ++o;
            ++it;
          }
        }
      } else {
        for (const TpchQ9Cell *o = ob; o != oe; ++o)
          colors.Add(old_mask, o->group, -static_cast<int64_t>(o->rows), -o->profit);
        for (const auto &c : now)
          colors.Add(new_mask, c.group, c.rows, c.profit);
      }
    }
  }

  static constexpr int64_t kBlockPartsI = static_cast<int64_t>(kParts);
  TpchQ9State &state_;
  const TpchQ9State *before_ = nullptr;
  core::FlatHashMap<std::shared_ptr<EditBlock>> edited_;
};

// Bulk computation of the color aggregates from all blocks.
void BuildColorIndex(const core::AuxEngineServices &services, TpchQ9State &state) {
  std::vector<std::pair<int64_t, const TpchQ9PartBlock *>> blocks;
  state.blocks.ForEach([&](int64_t key, const std::shared_ptr<TpchQ9PartBlock> &block) {
    if (block)
      blocks.emplace_back(key, block.get());
  });
  const std::size_t ngroups = state.groups.size();
  struct Wide {
    int64_t rows = 0;
    __int128 profit = 0;
  };
  using Totals = std::vector<Wide>; // [group * kTpchQ9Colors + color]
  Totals totals;
  core::ParallelReduce(
      services, blocks.size(), kColorBlockGrain, totals,
      [&](Totals &acc, std::size_t begin, std::size_t end) {
        acc.resize(ngroups * kTpchQ9Colors);
        for (std::size_t i = begin; i < end; ++i) {
          const auto [key, block] = blocks[i];
          for (std::size_t slot = 0; slot < kParts; ++slot) {
            if (block->cell_begin[slot] == block->cell_begin[slot + 1])
              continue;
            const ColorMask mask = MaskOfPart(state, key * static_cast<int64_t>(kParts) +
                                                         static_cast<int64_t>(slot));
            if (mask[0] == 0 && mask[1] == 0)
              continue;
            for (uint32_t j = block->cell_begin[slot]; j < block->cell_begin[slot + 1]; ++j) {
              Wide *row = acc.data() + static_cast<std::size_t>(block->cells[j].group) * kTpchQ9Colors;
              ForEachColor(mask, [&](std::size_t c) {
                row[c].rows += block->cells[j].rows;
                row[c].profit += block->cells[j].profit;
              });
            }
          }
        }
      },
      [](Totals &into, Totals &&from) {
        for (std::size_t i = 0; i < from.size(); ++i) {
          into[i].rows += from[i].rows;
          into[i].profit += from[i].profit;
        }
      });
  state.color_groups.assign(ngroups, nullptr);
  for (std::size_t g = 0; g < ngroups; ++g) {
    auto record = std::make_shared<TpchQ9GroupColors>();
    for (std::size_t c = 0; c < kTpchQ9Colors; ++c) {
      const Wide &total = totals[g * kTpchQ9Colors + c];
      if (total.profit > std::numeric_limits<int64_t>::max() ||
          total.profit < std::numeric_limits<int64_t>::min()) {
        state.color_groups.clear(); // beyond int64: no index, reads scan
        return;
      }
      record->by_color[c] = TpchQ9ColorAcc{total.rows, static_cast<int64_t>(total.profit)};
    }
    state.color_groups[g] = std::move(record);
  }
  state.color_index = true;
}

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

// A change of one profit cell of a part, resolved against the dimensions.
struct CellEdit {
  int64_t partkey = 0;
  int32_t nationkey = 0;
  int32_t year = 0;
  int64_t drows = 0;
  int64_t dprofit = 0;
};

using CellEdits = std::vector<CellEdit>;

// Resolves the items against the dimensions in parallel slices (resolve(item,
// edits) only reads), then applies the edits serially in item order, which
// interns the (nation, year) groups in the order of a serial run. The blocks of
// the edits must have been prepared.
template <class Item, class Resolve>
void ApplyResolved(const core::AuxEngineServices &services, TpchQ9State &state, BlockEditor &editor,
                   const std::vector<Item> &items, Resolve &&resolve) {
  CellEdits edits;
  core::ParallelReduce(
      services, items.size(), kResolveGrain, edits,
      [&](CellEdits &out, std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i)
          resolve(items[i], out);
      },
      [](CellEdits &into, CellEdits &&from) {
        into.insert(into.end(), from.begin(), from.end());
      });
  for (const CellEdit &edit : edits)
    editor.For(edit.partkey)
        .AddCell(SlotOf(edit.partkey), state.GroupFor(edit.nationkey, edit.year), edit.drows,
                 edit.dprofit);
}

// Any argument without a color aggregate: scan the name column for matching
// parts and add up their cells; emit(group, rows, profit) per group.
template <typename Emit>
void ScanMatching(const core::AuxEngineServices &services, const TpchQ9State &state,
                  const std::string &argument, Emit &&emit) {
  // The template is `p_name LIKE '%<color>%'`: the argument is spliced into the
  // pattern as is, so any LIKE metacharacter in it keeps its meaning.
  const std::string pattern = "%" + argument + "%";
  const auto matcher = like::LikeMatcher::Compile(pattern, '\\', kCaseInsensitive,
                                                  /*utf8_underscore=*/true);
  struct Acc {
    __int128 profit = 0;
    int64_t rows = 0;
  };
  const std::size_t ngroups = state.groups.size();
  const unsigned max_threads = static_cast<unsigned>(std::max<std::size_t>(1, services.WorkerCount()));
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
  for (std::size_t g = 0; g < ngroups; ++g) {
    Acc total;
    for (const auto &worker : acc) {
      total.profit += worker[g].profit;
      total.rows += worker[g].rows;
    }
    emit(g, total.rows, total.profit);
  }
}

// The build deals the scanned rows to this many owners by part block, so that
// owners edit disjoint blocks.
using core::kMaxOwners;

std::size_t OwnerOf(int64_t partkey, std::size_t owners) {
  return static_cast<uint64_t>(BlockOf(partkey)) % owners;
}

struct PartsuppRow {
  int64_t partkey = 0;
  int64_t suppkey = 0;
  int64_t cost_raw = 0;
};

using KeyedInts = std::vector<std::pair<int64_t, int32_t>>;
using KeyedNames = std::vector<std::pair<int64_t, std::string>>;

// Supplier (key, nation) rows and the distinct nation keys.
struct SupplierRows {
  KeyedInts rows;
  std::set<int32_t> nationkeys;
};

// Order (key, year) rows and the distinct years.
struct OrderRows {
  KeyedInts rows;
  std::set<int32_t> years;
};

// The editable blocks of the partsupp rows of one owner.
std::vector<std::pair<int64_t, std::shared_ptr<EditBlock>>>
BuildPartsuppBlocks(const std::vector<PartsuppRow> &rows) {
  core::FlatHashMap<std::shared_ptr<EditBlock>> blocks;
  for (const PartsuppRow &row : rows) {
    auto &edit = blocks[BlockOf(row.partkey)];
    if (!edit)
      edit = std::make_shared<EditBlock>();
    edit->SetPs(SlotOf(row.partkey), row.suppkey, row.cost_raw);
  }
  std::vector<std::pair<int64_t, std::shared_ptr<EditBlock>>> built;
  for (auto entry : blocks)
    built.emplace_back(entry.first, entry.second);
  return built;
}

// Lineitems join their supplier's nation, their order's year and their partsupp
// row, and add their profit to the (part, nation, year) cell. Owners edit
// disjoint blocks, so the join runs in parallel; every (nation, year) group is
// interned beforehand.
void AddLineitemCells(const core::AuxEngineServices &services, const TpchQ9State &state,
                      const BlockEditor &editor,
                      const core::Buckets<TpchQ9LineitemInput> &by_owner) {
  core::ParallelEach(services, by_owner.size(), 1, [&](std::size_t owner) {
    for (const TpchQ9LineitemInput &input : by_owner[owner]) {
      int32_t nationkey = 0, year = 0;
      EditBlock *edit = editor.Find(input.partkey);
      const auto slot = SlotOf(input.partkey);
      int64_t cost = 0, profit = 0;
      if (!edit || !state.LookupSupplierNation(input.suppkey, nationkey) ||
          !state.LookupOrderYear(input.orderkey, year) ||
          !edit->FindPs(slot, input.suppkey, cost))
        continue;
      ProfitRaw(input.extendedprice_raw, input.discount_raw, cost, input.quantity_raw, profit);
      edit->AddCell(slot, *state.FindGroup(nationkey, year), 1, profit);
    }
  });
}

// Join values (order year, supplier nation, supplycost) that differ between the
// published state and the last value of the batch. An order, supplier or
// partsupp row that appears, disappears or changes its join value moves the
// lineitems that join it between cells or changes their profit (also lineitems
// this batch does not touch: a supplier deleted before or without its
// lineitems, an order re-inserted after them).
struct JoinChanges {
  std::vector<int64_t> orders, suppliers, parts;
  std::set<std::pair<int64_t, int64_t>> partsupps;

  bool Empty() const { return orders.empty() && suppliers.empty() && parts.empty(); }
  std::size_t KeyCount() const { return orders.size() + suppliers.size() + parts.size(); }
};

JoinChanges FindJoinChanges(const TpchQ9State &old, const TpchQ9TxnBuffer &buffer) {
  JoinChanges changes;
  std::unordered_map<int64_t, std::optional<int32_t>> year_after;
  for (const auto &d : buffer.OrderDeltas())
    year_after[d.orderkey] = d.sign > 0 ? std::optional<int32_t>(d.order_year) : std::nullopt;
  std::unordered_map<int64_t, std::optional<int32_t>> nation_after;
  for (const auto &d : buffer.SupplierDeltas())
    nation_after[d.suppkey] = d.sign > 0 ? std::optional<int32_t>(d.nationkey) : std::nullopt;
  std::map<std::pair<int64_t, int64_t>, std::optional<int64_t>> cost_after;
  for (const auto &d : buffer.PartsuppDeltas())
    cost_after[{d.partkey, d.suppkey}] = d.sign > 0 ? std::optional<int64_t>(d.cost_raw) : std::nullopt;
  for (const auto &[key, after] : year_after) {
    int32_t before = 0;
    const bool had = old.LookupOrderYear(key, before);
    if (had != after.has_value() || (had && before != *after))
      changes.orders.push_back(key);
  }
  for (const auto &[key, after] : nation_after) {
    int32_t before = 0;
    const bool had = old.LookupSupplierNation(key, before);
    if (had != after.has_value() || (had && before != *after))
      changes.suppliers.push_back(key);
  }
  for (const auto &[key, after] : cost_after) {
    int64_t before = 0;
    const bool had = BlockFindPs(old.FindBlock(key.first), SlotOf(key.first), key.second, before);
    if (had != after.has_value() || (had && before != *after)) {
      changes.partsupps.insert(key);
      if (changes.parts.empty() || changes.parts.back() != key.first)
        changes.parts.push_back(key.first);
    }
  }
  return changes;
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

// The blocks of every part edited below, decoded in parallel up front; the
// lineitem steps then resolve their joins in parallel slices and apply the
// cell edits serially, in delta order.
void PrepareBlocks(const core::AuxEngineServices &services, const TpchQ9TxnBuffer &buffer,
                   const BatchLineitems &lines, const TpchQ9State &updated, BlockEditor &editor) {
  std::vector<int64_t> partkeys;
  for (const auto *d : lines.deletes)
    partkeys.push_back(d->partkey);
  for (const auto *d : lines.inserts)
    partkeys.push_back(d->partkey);
  for (const auto &d : buffer.PartsuppDeltas())
    partkeys.push_back(d.partkey);
  if (updated.color_index)
    for (const auto &d : buffer.PartDeltas())
      partkeys.push_back(d.partkey);
  editor.Prepare(services, partkeys);
}

// Deleted lineitems leave the cells they were counted in (resolved against the
// dimension values of the published state).
void RemoveDeletedLineitems(const core::AuxEngineServices &services, const TpchQ9State &old,
                            TpchQ9State &updated, BlockEditor &editor,
                            const std::vector<const TpchQ9LineitemDelta *> &deletes) {
  ApplyResolved(services, updated, editor, deletes,
                [&](const TpchQ9LineitemDelta *d, CellEdits &out) {
                  int32_t nationkey = 0, year = 0;
                  int64_t cost = 0, profit = 0;
                  if (!old.LookupSupplierNation(d->suppkey, nationkey) ||
                      !old.LookupOrderYear(d->orderkey, year) ||
                      !BlockFindPs(old.FindBlock(d->partkey), SlotOf(d->partkey), d->suppkey, cost))
                    return; // never joined: nothing to remove
                  ProfitRaw(d->extendedprice_raw, d->discount_raw, cost, d->quantity_raw, profit);
                  out.push_back({d->partkey, nationkey, year, -1, -profit});
                });
}

// Dimension changes; the lineitems they move are re-counted by
// RecountJoinChanges.
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
    if (updated.color_index)
      editor.MarkRenamed(d.partkey); // its cells move between colors at Flush
  }
  for (const auto &d : buffer.NationDeltas()) {
    if (d.sign > 0)
      updated.nation_names[d.nationkey] = d.name;
    else
      updated.nation_names.erase(d.nationkey);
  }
}

// Inserted lineitems join against the post-batch dimensions.
void AddInsertedLineitems(const core::AuxEngineServices &services, TpchQ9State &updated,
                          BlockEditor &editor,
                          const std::vector<const TpchQ9LineitemDelta *> &inserts) {
  const TpchQ9State &after = updated;
  ApplyResolved(services, updated, editor, inserts,
                [&](const TpchQ9LineitemDelta *d, CellEdits &out) {
                  int32_t nationkey = 0, year = 0;
                  int64_t cost = 0, profit = 0;
                  const EditBlock *edit = editor.Find(d->partkey);
                  if (!edit || !after.LookupSupplierNation(d->suppkey, nationkey) ||
                      !after.LookupOrderYear(d->orderkey, year) ||
                      !edit->FindPs(SlotOf(d->partkey), d->suppkey, cost))
                    return; // does not join (yet)
                  ProfitRaw(d->extendedprice_raw, d->discount_raw, cost, d->quantity_raw, profit);
                  out.push_back({d->partkey, nationkey, year, +1, profit});
                });
}

// Every lineitem joining a changed order, supplier or partsupp that was there
// before the batch and still is after it (not one of the batch's own inserts,
// which AddInsertedLineitems counted against the new dimensions; deleted ones
// left in RemoveDeletedLineitems against the old) leaves the cell it was
// counted in (old dimensions, if it joined) and enters the one it joins now
// (new dimensions, if any). The lineitems are looked up in the database as the
// transaction leaves it; false when that is not supported.
bool RecountJoinChanges(const core::AuxEngineServices &services, TpchQ9InitialScan *scan,
                        const TpchQ9State &old, TpchQ9State &updated, BlockEditor &editor,
                        const JoinChanges &changes,
                        const std::vector<const TpchQ9LineitemDelta *> &inserts) {
  std::map<std::pair<int64_t, int32_t>, TpchQ9LineitemInput> affected; // (orderkey, linenumber)
  const auto collect = [&](const char *column, const std::vector<int64_t> &keys, bool by_part) {
    if (keys.empty())
      return true;
    return scan && scan->LookupLineitems(column, keys, [&](const TpchQ9LineitemInput &l, int32_t line) {
      if (!by_part || changes.partsupps.count({l.partkey, l.suppkey}))
        affected.emplace(std::make_pair(l.orderkey, line), l);
    });
  };
  if (!collect("l_orderkey", changes.orders, false) ||
      !collect("l_suppkey", changes.suppliers, false) ||
      !collect("l_partkey", changes.parts, true))
    return false;
  std::map<std::array<int64_t, 6>, std::size_t> own_inserts;
  for (const auto *d : inserts)
    ++own_inserts[{d->partkey, d->suppkey, d->orderkey, d->quantity_raw,
                   d->extendedprice_raw, d->discount_raw}];
  std::vector<const TpchQ9LineitemInput *> recount;
  std::vector<int64_t> recount_partkeys;
  for (const auto &entry : affected) {
    const TpchQ9LineitemInput &l = entry.second;
    const auto own = own_inserts.find({l.partkey, l.suppkey, l.orderkey, l.quantity_raw,
                                       l.extendedprice_raw, l.discount_raw});
    if (own != own_inserts.end() && own->second > 0) {
      --own->second;
      continue;
    }
    recount.push_back(&l);
    recount_partkeys.push_back(l.partkey);
  }
  editor.Prepare(services, recount_partkeys);
  const TpchQ9State &after = updated;
  ApplyResolved(services, updated, editor, recount,
                [&](const TpchQ9LineitemInput *l, CellEdits &out) {
                  const auto slot = SlotOf(l->partkey);
                  const EditBlock *edit = editor.Find(l->partkey);
                  int32_t nationkey = 0, year = 0;
                  int64_t cost = 0, profit = 0;
                  if (old.LookupSupplierNation(l->suppkey, nationkey) &&
                      old.LookupOrderYear(l->orderkey, year) &&
                      BlockFindPs(old.FindBlock(l->partkey), slot, l->suppkey, cost)) {
                    ProfitRaw(l->extendedprice_raw, l->discount_raw, cost, l->quantity_raw, profit);
                    out.push_back({l->partkey, nationkey, year, -1, -profit});
                  }
                  if (edit && after.LookupSupplierNation(l->suppkey, nationkey) &&
                      after.LookupOrderYear(l->orderkey, year) &&
                      edit->FindPs(slot, l->suppkey, cost)) {
                    ProfitRaw(l->extendedprice_raw, l->discount_raw, cost, l->quantity_raw, profit);
                    out.push_back({l->partkey, nationkey, year, +1, profit});
                  }
                });
  return true;
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

std::shared_ptr<TpchQ9State> TpchQ9Module::BuildState(core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q9 aux requires a typed initial scan source");
  auto state = std::make_shared<TpchQ9State>();
  const std::size_t partitions = core::BuildPartitions(services);

  initial_scan->ScanNations([&](const TpchQ9NationInput &input) {
    state->nation_names[input.nationkey] = input.name;
  });

  // The scans fill per-partition partials in parallel; the single-writer
  // columns of the state are then set serially, in scan order.
  const auto suppliers = core::PartitionedBuild<SupplierRows>(
      services, [] { return SupplierRows{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanSuppliersPartitioned(
            services, parts, [&](std::size_t partition, const TpchQ9SupplierInput &input) {
              sink(partition, core::SingleRowBatch<TpchQ9SupplierInput>{input});
            });
      },
      [](SupplierRows &into, const TpchQ9SupplierInput &input) {
        into.rows.emplace_back(input.suppkey, input.nationkey);
        into.nationkeys.insert(input.nationkey);
      },
      [](SupplierRows &into, SupplierRows &&from) {
        into.rows.insert(into.rows.end(), from.rows.begin(), from.rows.end());
        into.nationkeys.insert(from.nationkeys.begin(), from.nationkeys.end());
      },
      partitions);
  for (const auto &[suppkey, nationkey] : suppliers.rows)
    state->SetSupplierNation(suppkey, nationkey);

  const auto orders = core::PartitionedBuild<OrderRows>(
      services, [] { return OrderRows{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanOrdersPartitioned(
            services, parts, [&](std::size_t partition, const TpchQ9OrdersInput &input) {
              sink(partition, core::SingleRowBatch<TpchQ9OrdersInput>{input});
            });
      },
      [](OrderRows &into, const TpchQ9OrdersInput &input) {
        const int32_t year = TpchQ9ExtractYear(input.orderdate_days);
        into.rows.emplace_back(input.orderkey, year);
        into.years.insert(year);
      },
      [](OrderRows &into, OrderRows &&from) {
        into.rows.insert(into.rows.end(), from.rows.begin(), from.rows.end());
        into.years.insert(from.years.begin(), from.years.end());
      },
      partitions);
  for (const auto &[orderkey, year] : orders.rows)
    state->SetOrderYear(orderkey, year);

  auto part_names = core::PartitionedBuild<KeyedNames>(
      services, [] { return KeyedNames{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanPartsPartitioned(
            services, parts, [&](std::size_t partition, const TpchQ9PartInput &input) {
              sink(partition, core::SingleRowBatch<TpchQ9PartInput>{input});
            });
      },
      [](KeyedNames &into, const TpchQ9PartInput &input) {
        into.emplace_back(input.partkey, input.name);
      },
      core::AppendRows<std::pair<int64_t, std::string>>,
      partitions);
  for (const auto &[partkey, name] : part_names)
    state->part_names.Set(partkey, name);

  // Partsupp rows and profit cells are gathered per block in editable form and
  // encoded at the end (transient: the editable copy of the cells, ~1.3x).
  BlockEditor editor(*state);
  auto partsupps = core::PartitionedBuild<core::Buckets<PartsuppRow>>(
      services, [] { return core::Buckets<PartsuppRow>(kMaxOwners); },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanPartsuppsPartitioned(
            services, parts, [&](std::size_t partition, const TpchQ9PartsuppInput &input) {
              sink(partition, core::SingleRowBatch<TpchQ9PartsuppInput>{input});
            });
      },
      [](core::Buckets<PartsuppRow> &into, const TpchQ9PartsuppInput &input) {
        into[OwnerOf(input.partkey, kMaxOwners)].push_back(
            {input.partkey, input.suppkey, input.supplycost_raw});
      },
      core::AppendBuckets<PartsuppRow>, partitions);
  std::vector<std::vector<std::pair<int64_t, std::shared_ptr<EditBlock>>>> built(kMaxOwners);
  core::ParallelEach(services, kMaxOwners, 1, [&](std::size_t owner) {
    built[owner] = BuildPartsuppBlocks(partsupps[owner]);
    partsupps[owner] = {};
  });
  for (auto &owned : built)
    for (auto &[block_index, edit] : owned)
      editor.Adopt(block_index, std::move(edit));

  for (const int32_t nationkey : suppliers.nationkeys)
    for (const int32_t year : orders.years)
      state->GroupFor(nationkey, year);
  auto lineitems = core::PartitionedBuild<core::Buckets<TpchQ9LineitemInput>>(
      services, [] { return core::Buckets<TpchQ9LineitemInput>(kMaxOwners); },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanLineitemsPartitioned(
            services, parts, [&](std::size_t partition, const TpchQ9LineitemInput &input) {
              sink(partition, core::SingleRowBatch<TpchQ9LineitemInput>{input});
            });
      },
      [](core::Buckets<TpchQ9LineitemInput> &into, const TpchQ9LineitemInput &input) {
        into[OwnerOf(input.partkey, kMaxOwners)].push_back(input);
      },
      core::AppendBuckets<TpchQ9LineitemInput>, partitions);
  AddLineitemCells(services, *state, editor, lineitems);
  editor.Flush(services);
  BuildColorIndex(services, *state);
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
  if (!LoadState(services, old_state, &existing_version)) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }
  const TpchQ9State &old = *old_state;

  const JoinChanges changes = FindJoinChanges(old, buffer);
  if (changes.KeyCount() > kMaxJoinChangeKeys) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }
  const BatchLineitems lines = CancelLineitems(buffer);

  // Copy-on-write: shares every block / chunk / shard with the published state;
  // only the touched ones are cloned below.
  auto updated = std::make_shared<TpchQ9State>(old);
  BlockEditor editor(*updated, &old);
  PrepareBlocks(services, buffer, lines, *updated, editor);
  RemoveDeletedLineitems(services, old, *updated, editor, lines.deletes);
  ApplyDimensionChanges(buffer, *updated, editor);
  AddInsertedLineitems(services, *updated, editor, lines.inserts);
  if (!changes.Empty() &&
      !RecountJoinChanges(services, initial_scan, old, *updated, editor, changes, lines.inserts)) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }
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

void TpchQ9Module::EmitResult(const core::AuxEngineServices &services,
                              const TpchQ9State &state,
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
  const std::string &argument = std::get<std::string>(arguments.values[0].payload);
  std::map<std::pair<std::string, int32_t>, __int128> sums;
  // GROUP BY n_name, year: groups of nations that share a name merge, and a
  // supplier nation without a nation row joins nothing.
  const auto add_group = [&](std::size_t g, int64_t rows, __int128 profit) {
    if (rows == 0)
      return;
    const auto nation = state.nation_names.find(state.groups[g].first);
    if (nation == state.nation_names.end())
      return;
    sums[{nation->second, state.groups[g].second}] += profit;
  };

  const int color = state.color_index ? ColorOf(argument) : -1;
  if (color >= 0) {
    // Pre-aggregated answer of one of the 92 color words: O(groups).
    const std::size_t n = std::min(state.color_groups.size(), state.groups.size());
    for (std::size_t g = 0; g < n; ++g)
      if (const auto &record = state.color_groups[g]) {
        const auto &acc = record->by_color[static_cast<std::size_t>(color)];
        add_group(g, acc.rows, acc.profit);
      }
  } else {
    ScanMatching(services, state, argument, add_group);
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
