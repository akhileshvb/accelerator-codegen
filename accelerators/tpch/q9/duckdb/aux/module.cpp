#include "tpch/q9/duckdb/aux/module.hpp"
#include "tpch/q9/calibration_writes.hpp"
#include "prims/sql_date.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

// Profit slots per slice when visiting their part records (~0.1 ms per slot).
constexpr std::size_t kSlotGrain = 16;
// Profit slots per slice when copying them or folding them into the cube (~10 us per slot).
constexpr std::size_t kSlotFoldGrain = 8;
// Profit slots per slice when applying buffered pair aggregates at build.
constexpr std::size_t kPairSlotGrain = 4;
// Candidate parts per read slice: a hash lookup, substring test and cells, ~0.1 us each.
constexpr std::size_t kCandidateGrain = 8192;
// Affected pairs per snapshot chunk; one chunk per slice.
constexpr std::size_t kPairChunk = 4096;
constexpr std::size_t kPairChunkGrain = 1;
// Parts reserved before the part scan.
constexpr std::size_t kPartScanReserve = 4096;

// Order years 1992..1998 index the dense profit grid.
constexpr int32_t kDenseNations = Q9TokenCube::kNations;
constexpr int32_t kDenseYears = Q9TokenCube::kYears;
constexpr int32_t kDenseYearBase = Q9TokenCube::kYearBase;

// One (part, supplier, order year) lineitem aggregate: rev = SUM(extendedprice_raw
// * (100 - discount_raw)), qty = SUM(quantity_raw).
struct PairAgg {
  int64_t partkey, suppkey;
  int32_t year;
  int64_t rev, qty;
};

// Keys that do not fit the int32 pair index are not supported incrementally.
bool PairKeyFits(int64_t suppkey) {
  return suppkey >= 1 && suppkey <= static_cast<int64_t>(INT32_MAX);
}

std::shared_ptr<const TpchQ9StateImage>
MakeImage(TpchQ9ImageKind kind, const std::shared_ptr<const TpchQ9State> &state,
          core::PartitionId profit_slot = 0) {
  auto image = std::make_shared<TpchQ9StateImage>();
  image->kind = kind;
  image->profit_slot = profit_slot;
  image->state = state;
  return image;
}

const TpchQ9StateImage *AsQ9Image(const core::StateImage &image) {
  const auto *payload = image.As<TpchQ9StateImage>();
  return payload && payload->state ? payload : nullptr;
}

// A state image holding one profit slot at index 0.
std::shared_ptr<const TpchQ9StateImage>
MakeProfitSlotImage(core::PartitionId slot_count, core::PartitionId slot,
                    TpchQ9State::PartProfitPartition &&partition) {
  auto slot_state = std::make_shared<TpchQ9State>();
  slot_state->profit_slot_count = slot_count;
  slot_state->profit_partitions.resize(1);
  slot_state->profit_partitions[0] = std::move(partition);
  return MakeImage(TpchQ9ImageKind::ProfitSlot, slot_state, slot);
}

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

bool ContainsSubstring(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) return true;
  return haystack.find(needle) != std::string_view::npos;
}

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

bool ReadJoinedLineitem(const core::AuxRow &row, int64_t &partkey,
                        int64_t &suppkey, int64_t &orderkey,
                        int64_t &quantity_raw, int64_t &extendedprice_raw,
                        int64_t &discount_raw) {
  int32_t scale = 0;
  return core::IntCell(row, 0, partkey) && core::IntCell(row, 1, suppkey) &&
         core::IntCell(row, 2, orderkey) &&
         core::DecimalCell(row, 3, quantity_raw, scale) &&
         core::DecimalCell(row, 4, extendedprice_raw, scale) &&
         core::DecimalCell(row, 5, discount_raw, scale);
}

bool ReadPart(const core::AuxRow &row, int64_t &partkey, std::string &name) {
  return core::IntCell(row, 0, partkey) && core::StringCell(row, 1, name);
}

// An int32 nation key in column 1.
bool ReadNationKeyCell(const core::AuxRow &row, int64_t &nationkey) {
  int32_t nk = 0;
  if (!core::Int32Cell(row, 1, nk))
    return false;
  nationkey = nk;
  return true;
}

bool ReadSupplier(const core::AuxRow &row, int64_t &suppkey,
                  int64_t &nationkey) {
  return core::IntCell(row, 0, suppkey) && ReadNationKeyCell(row, nationkey);
}

bool ReadOrder(const core::AuxRow &row, int64_t &orderkey, int32_t &orderdate) {
  return core::IntCell(row, 0, orderkey) && core::DateCell(row, 1, orderdate);
}

bool ReadPartsupp(const core::AuxRow &row, int64_t &partkey, int64_t &suppkey,
                  int64_t &supplycost_raw) {
  int32_t scale = 0;
  return core::IntCell(row, 0, partkey) && core::IntCell(row, 1, suppkey) &&
         core::DecimalCell(row, 2, supplycost_raw, scale);
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

// Cube deltas aggregated per token before touching the (COW) cube. Token views
// point into part names, which outlive the netting (the records are held by the
// slot maps and are never renamed there).
constexpr int kDenseCells = kDenseNations * kDenseYears;
struct TokenAcc {
  std::unordered_map<std::string_view, std::size_t> index;
  std::vector<std::string_view> names;
  std::vector<std::array<int64_t, kDenseCells>> acc;
  std::vector<TpchQ9State::CellDelta> sparse; // outside the dense range
  std::vector<std::string_view> sparse_tokens; // the token of each sparse delta

  std::array<int64_t, kDenseCells> &Slot(std::string_view token) {
    auto it = index.find(token);
    if (it == index.end()) {
      it = index.emplace(token, acc.size()).first;
      names.push_back(token);
      acc.emplace_back();
      acc.back().fill(0);
    }
    return acc[it->second];
  }

  void Merge(TokenAcc &&from) {
    for (std::size_t t = 0; t < from.names.size(); ++t) {
      auto &into = Slot(from.names[t]);
      for (int c = 0; c < kDenseCells; ++c)
        into[c] += from.acc[t][c];
    }
    sparse.insert(sparse.end(), from.sparse.begin(), from.sparse.end());
    sparse_tokens.insert(sparse_tokens.end(), from.sparse_tokens.begin(),
                         from.sparse_tokens.end());
  }
};

// Distinct whitespace-separated tokens of the name (as NameTokens).
void NameTokenViews(const std::string &name, std::vector<std::string_view> &views) {
  views.clear();
  std::size_t p = 0;
  while (p < name.size()) {
    while (p < name.size() && std::isspace(static_cast<unsigned char>(name[p])))
      ++p;
    if (p >= name.size())
      break;
    const std::size_t start = p;
    while (p < name.size() && !std::isspace(static_cast<unsigned char>(name[p])))
      ++p;
    const std::string_view v(name.data() + start, p - start);
    if (std::find(views.begin(), views.end(), v) == views.end())
      views.push_back(v);
  }
}

// Nets one slot's deltas per (part, nation, year), applies them to the parts'
// cells and adds the token deltas to `ta`. Slots own disjoint part records.
void ApplySlotDeltas(TpchQ9State &state, std::vector<TpchQ9State::CellDelta> &deltas,
                     TokenAcc &ta) {
  std::sort(deltas.begin(), deltas.end(),
            [](const TpchQ9State::CellDelta &a, const TpchQ9State::CellDelta &b) {
              if (a.partkey != b.partkey)
                return a.partkey < b.partkey;
              if (a.nation != b.nation)
                return a.nation < b.nation;
              return a.year < b.year;
            });
  std::vector<TpchQ9State::CellDelta> net;
  std::vector<std::string_view> views;
  std::size_t i = 0;
  while (i < deltas.size()) {
    const int64_t partkey = deltas[i].partkey;
    std::size_t group_end = i;
    while (group_end < deltas.size() && deltas[group_end].partkey == partkey)
      ++group_end;
    net.clear();
    for (std::size_t j = i; j < group_end;) {
      auto cur = deltas[j];
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
    auto *rec = state.GetPartition(partkey).parts.Mutable(partkey);
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
    if (!state.cube_active)
      continue;
    NameTokenViews(rec->normalized_name, views);
    for (const auto v : views) {
      auto &acc = ta.Slot(v);
      for (const auto &c : net) {
        const int32_t yi = c.year - kDenseYearBase;
        if (c.nation >= 0 && c.nation < kDenseNations && yi >= 0 && yi < kDenseYears) {
          acc[c.nation * kDenseYears + yi] += c.delta;
        } else {
          ta.sparse.push_back(c);
          ta.sparse_tokens.push_back(v);
        }
      }
    }
  }
}

// Lineitems become per-(part, supplier, order year) aggregates, bucketed by
// profit slot: partitions fill their own buckets, then every slot (which owns
// disjoint part records) is applied by one worker.
void BuildPairAggregates(const core::AuxEngineServices &services,
                         TpchQ9InitialScan &initial_scan, TpchQ9State &state) {
  using SlotPairs = std::vector<std::vector<PairAgg>>;
  const auto slots = state.profit_slot_count;
  SlotPairs pairs = core::PartitionedBuild<SlotPairs>(
      services, [slots] { return SlotPairs(slots); },
      [&](std::size_t parts, auto &&sink) {
        initial_scan.ScanLineitems(services, parts,
                                   [&](std::size_t p, const TpchQ9LineitemInput &input) {
                                     sink(p, core::SingleRowBatch<TpchQ9LineitemInput>{input});
                                   });
      },
      [&](SlotPairs &partial, const TpchQ9LineitemInput &input) {
        int32_t year = 0;
        if (!PairKeyFits(input.suppkey) ||
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
}

// Part mutations: name changes affect the postings; a key change is a delete
// of the old part plus an insert of the new one (pairs follow the part record,
// so no rebuild is needed).
void AccumulatePart(const core::MutationBatch &mutation, TpchQ9TxnBuffer &buffer) {
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
        buffer.AddPartNamePosting(old_pk, old_name, "");
        buffer.AddPartNamePosting(new_pk, "", new_name);
      } else if (old_name != new_name) {
        buffer.AddPartNamePosting(old_pk, old_name, new_name);
      }
    }
    break;
  }
}

// Supplier mutations: incremental over the supplier's (part, supp) pairs.
void AccumulateSupplier(const core::MutationBatch &mutation, TpchQ9TxnBuffer &buffer) {
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
          ReadSupplier(mutation.new_rows[i], nk, nnat) && ok == nk && onat == nnat)
        continue; // non-join column update: no-op for Q9
      if (!add(mutation.old_rows[i], -1) || !add(mutation.new_rows[i], 1))
        return;
    }
    break;
  }
}

// Orders mutations maintain order_years on the maintenance side. RF1 must
// apply the orders insert before the lineitems in the same txn so the
// resolution can see the new order year (AccumulateMutation order follows
// OnMutation).
void AccumulateOrders(const core::MutationBatch &mutation, TpchQ9TxnBuffer &buffer) {
  auto add = [&](const core::AuxRow &row, int8_t sign) {
    int64_t orderkey = 0;
    int32_t orderdate = 0;
    if (!ReadOrder(row, orderkey, orderdate)) {
      buffer.MarkRebuild();
      return;
    }
    buffer.AddOrderDelta({orderkey, TpchQ9ExtractYear(orderdate), sign});
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

// Partsupp mutations: incremental per (part, supp) pair.
void AccumulatePartsupp(const core::MutationBatch &mutation, TpchQ9TxnBuffer &buffer) {
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
}

// Lineitem mutations: applied as deltas when fully resolvable.
void AccumulateLineitems(const core::MutationBatch &mutation, TpchQ9TxnBuffer &buffer) {
  auto add = [&](const core::AuxRow &row, int8_t sign) {
    int64_t partkey = 0, suppkey = 0, orderkey = 0;
    int64_t quantity_raw = 0, extendedprice_raw = 0, discount_raw = 0;
    if (!ReadJoinedLineitem(row, partkey, suppkey, orderkey, quantity_raw,
                            extendedprice_raw, discount_raw)) {
      buffer.MarkRebuild();
      return;
    }
    buffer.AddDelta({partkey, suppkey, orderkey, quantity_raw, extendedprice_raw,
                     discount_raw, sign});
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

// An order whose year changes moves the profit of its existing lineitems
// between years, which the deltas cannot do.
bool OrderYearsConsistent(const TpchQ9TxnBuffer &buffer) {
  std::unordered_map<int64_t, int32_t> removed_year;
  for (const auto &delta : buffer.OrderDeltas())
    if (delta.sign < 0)
      removed_year[delta.orderkey] = delta.order_year;
  for (const auto &delta : buffer.OrderDeltas()) {
    if (delta.sign <= 0)
      continue;
    const auto it = removed_year.find(delta.orderkey);
    if (it != removed_year.end() && it->second != delta.order_year)
      return false;
  }
  return true;
}

// Delta maintenance of the profit slots, the token cube, the join-side maps and
// the name index for one commit. Run() is false when the delta cannot be
// applied exactly and the state must rebuild; otherwise `outcome` is complete.
//
// Pass A removes the affected (part, supplier) pairs' contributions under the
// old state and pass B adds them under the new one. Parts whose name changes
// keep the immediate per-token path (their tokens differ between the two
// passes). Every other part's contributions are collected as signed cell
// deltas and netted per (part, nation, year) at the end, so an untouched
// year/pair costs nothing and the cube is updated once per (token, nation,
// year) instead of twice per pair cell and token.
class ProfitMaintainer {
public:
  ProfitMaintainer(core::AuxEngineServices &services, const TpchQ9TxnBuffer &buffer,
                   TpchQ9InitialScan *initial_scan,
                   core::StagedMaintenanceOutcome &outcome)
      : services(services), buffer(buffer), initial_scan(initial_scan),
        outcome(outcome), visibility(services.CurrentVisibility()) {}

  bool Run() {
    if (!LoadImages() || !CollectAffected())
      return false;
    LoadTouchedSlots();
    AddRenamedPairs();
    SnapshotOldContributions();
    if (!ApplyLineitemDeltas())
      return false;
    ApplyJoinSideDeltas();
    ApplyPartNameDeltas();
    AddNewContributions();
    new_state->ApplyNetCellDeltas(services, net_chunks);
    Publish();
    return true;
  }

private:
  // Metadata, side and cube images; the slots follow once the batch's affected
  // parts are known. The snapshots stay alive for the whole commit.
  bool LoadImages() {
    meta_snapshot.emplace(services.StateStore().ReadPartitions(
        visibility, {TpchQ9Layout::kMetadata}));
    for (const auto &image : meta_snapshot->partitions) {
      if (image.partition != TpchQ9Layout::kMetadata)
        continue;
      metadata_image = AsQ9Image(image);
      metadata_version = image.version;
      break;
    }
    if (!metadata_image)
      return false;

    new_state = std::make_shared<TpchQ9State>();
    new_state->profit_slot_count = metadata_image->state->profit_slot_count;
    new_state->InitSlots(new_state->profit_slot_count);
    slot_count = new_state->profit_slot_count;

    aux_snapshot.emplace(services.StateStore().ReadPartitions(
        visibility, {TpchQ9Layout::kMaintenanceSide, TpchQ9Layout::kCube}));
    const TpchQ9State *cube_src = nullptr;
    for (const auto &image : aux_snapshot->partitions) {
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
      return false;
    new_state->cube = cube_src->cube; // shared_ptr copies; cloned on write
    new_state->cube_active = true;

    touch_names = !buffer.PartNameDeltas().empty();
    if (touch_names) {
      new_state->nation_names = metadata_image->state->nation_names;
      new_state->name_postings = metadata_image->state->name_postings; // COW
      new_state->nation_emit_order = metadata_image->state->nation_emit_order;
    }
    return true;
  }

  void Touch(int64_t partkey) {
    touched_slots.insert(TpchQ9State::HashPartition(partkey, slot_count));
  }

  // The (part, supplier) pairs and the profit slots this batch can affect.
  bool CollectAffected() {
    for (const auto &d : buffer.Deltas()) {
      if (!PairKeyFits(d.suppkey))
        return false;
      Touch(d.partkey);
      affected.emplace_back(d.partkey, static_cast<int32_t>(d.suppkey));
    }
    for (const auto &d : buffer.PartsuppDeltas()) {
      if (!PairKeyFits(d.suppkey))
        return false;
      Touch(d.partkey);
      affected.emplace_back(d.partkey, static_cast<int32_t>(d.suppkey));
    }
    for (const auto &d : buffer.SupplierDeltas()) {
      if (!PairKeyFits(d.suppkey))
        return false;
      if (const auto list = join_side->SuppPairs(d.suppkey)) {
        for (const auto pk : *list) {
          Touch(pk);
          affected.emplace_back(pk, static_cast<int32_t>(d.suppkey));
        }
      }
    }
    for (const auto &change : buffer.PartNameDeltas())
      Touch(std::get<0>(change));
    return true;
  }

  // Copies the touched slots out of the published images: pointer vectors of
  // shared records, so independent per slot. Records are cloned on write.
  void LoadTouchedSlots() {
    if (touched_slots.empty())
      return;
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
    core::ParallelEach(services, copies.size(), kSlotFoldGrain, [&](std::size_t i) {
      new_state->profit_partitions[copies[i].first] =
          copies[i].second->profit_partitions[0];
    });
  }

  Q9PartRecord *FindRecord(int64_t partkey) {
    return new_state->GetPartition(partkey).parts.Mutable(partkey);
  }

  // A part change affects every pair the part has aggregates for.
  void AddRenamedPairs() {
    for (const auto &change : buffer.PartNameDeltas()) {
      const auto partkey = std::get<0>(change);
      renamed.insert(partkey);
      if (const auto *rec = new_state->FindPart(partkey))
        for (const auto &pair : rec->pairs)
          affected.emplace_back(partkey, pair.suppkey);
    }
    std::sort(affected.begin(), affected.end());
    affected.erase(std::unique(affected.begin(), affected.end()), affected.end());
    chunk_count = (affected.size() + kPairChunk - 1) / kPairChunk;
    net_chunks.resize(chunk_count);
    snaps.resize(affected.size());
    snap_valid.assign(affected.size(), 0);
  }

  // Pass A, read-only over the slots and the published side image: parallel by chunk.
  void SnapshotOldContributions() {
    core::ParallelEach(services, chunk_count, kPairChunkGrain, [&](std::size_t c) {
      const std::size_t end = std::min(affected.size(), (c + 1) * kPairChunk);
      for (std::size_t idx = c * kPairChunk; idx < end; ++idx) {
        const auto partkey = affected[idx].first;
        const auto suppkey = affected[idx].second;
        if (renamed.count(partkey))
          continue;
        const auto *rec = new_state->FindPart(partkey);
        if (!rec)
          continue;
        snap_valid[idx] = TpchQ9State::PairSnapshot(
            partkey, *rec, suppkey, -1, *join_side, snaps[idx], net_chunks[c]);
      }
    });
    ApplyRenamedPairs(-1, *join_side);
  }

  void ApplyRenamedPairs(int sign, const TpchQ9State &side) {
    for (const auto &[partkey, suppkey] : affected) {
      if (!renamed.count(partkey))
        continue;
      auto *rec = FindRecord(partkey);
      if (!rec || !TpchQ9State::FindPair(*rec, suppkey))
        continue;
      const auto tokens = NameTokens(rec->normalized_name);
      new_state->ApplyPairContribution(partkey, *rec, tokens, suppkey, sign, side);
    }
  }

  // Lineitem deltas move the pair aggregates (no cube work here: pass B).
  // Years are resolved first (the scan fallback is not thread-safe); the
  // aggregates are then applied per slot, grouped by part so each record is
  // looked up (and cloned) once.
  bool ApplyLineitemDeltas() {
    side_work = std::make_shared<TpchQ9State>();
    side_work->CopySideFrom(*join_side);
    side_dirty = !buffer.OrderDeltas().empty() || !buffer.SupplierDeltas().empty() ||
                 !buffer.PartsuppDeltas().empty();

    std::unordered_map<int64_t, int32_t> year_overlay;
    for (const auto &delta : buffer.OrderDeltas()) {
      if (delta.sign > 0)
        year_overlay[delta.orderkey] = delta.order_year;
    }

    const auto &deltas = buffer.Deltas();
    std::vector<int32_t> years(deltas.size());
    for (std::size_t k = 0; k < deltas.size(); ++k)
      if (!ResolveOrderYear(*join_side, initial_scan, deltas[k].orderkey, years[k],
                            year_overlay))
        return false;
    std::vector<std::vector<uint32_t>> by_slot(slot_count);
    for (uint32_t k = 0; k < deltas.size(); ++k)
      by_slot[TpchQ9State::HashPartition(deltas[k].partkey, slot_count)].push_back(k);
    std::vector<core::PartitionId> active;
    for (core::PartitionId slot = 0; slot < slot_count; ++slot)
      if (!by_slot[slot].empty())
        active.push_back(slot);
    std::vector<std::vector<std::pair<int64_t, int64_t>>> created(active.size());
    // Slots are disjoint; each task owns its slot's records and `created[t]`.
    core::ParallelEach(services, active.size(), kSlotGrain, [&](std::size_t t) {
      auto &order = by_slot[active[t]];
      std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
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
        const int64_t rev = delta.extendedprice_raw *
                            (static_cast<int64_t>(100) - delta.discount_raw);
        if (TpchQ9State::AddPairAgg(*rec, static_cast<int32_t>(delta.suppkey),
                                    years[k], sign * rev, sign * delta.quantity_raw))
          created[t].emplace_back(delta.suppkey, delta.partkey);
      }
    });
    for (const auto &list : created)
      for (const auto &[suppkey, partkey] : list) {
        side_work->AddSuppPair(suppkey, partkey);
        side_dirty = true;
      }
    return true;
  }

  void ApplyJoinSideDeltas() {
    for (const auto &d : buffer.SupplierDeltas()) {
      if (d.sign < 0)
        side_work->EraseSupplierNation(d.suppkey);
      else
        side_work->SetSupplierNation(d.suppkey, static_cast<int32_t>(d.nationkey));
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
  }

  // Part name / existence changes (all of the part's pairs are in `affected`).
  void ApplyPartNameDeltas() {
    for (const auto &[partkey, old_name, new_name] : buffer.PartNameDeltas()) {
      if (!old_name.empty()) {
        new_state->RemoveNamePostings(partkey);
        if (auto *rec = FindRecord(partkey)) {
          rec->normalized_name.clear();
          rec->exists = false;
        }
      }
      if (!new_name.empty()) {
        new_state->AddNamePostings(partkey, new_name);
      } else if (auto *rec = FindRecord(partkey)) {
        if (rec->pairs.empty() && rec->extras.empty() && rec->cells.empty())
          new_state->GetPartition(partkey).parts.Erase(partkey);
      }
    }
  }

  // Pass B: the affected pairs' contributions under the new state, as the
  // difference to their pass-A snapshot.
  void AddNewContributions() {
    core::ParallelEach(services, chunk_count, kPairChunkGrain, [&](std::size_t c) {
      auto &out = net_chunks[c];
      const std::size_t end = std::min(affected.size(), (c + 1) * kPairChunk);
      for (std::size_t idx = c * kPairChunk; idx < end; ++idx) {
        const auto partkey = affected[idx].first;
        const auto suppkey = affected[idx].second;
        if (renamed.count(partkey))
          continue;
        const auto *rec = new_state->FindPart(partkey);
        if (!rec)
          continue;
        TpchQ9State::PairSnap now;
        const bool now_valid = TpchQ9State::PairSnapshot(partkey, *rec, suppkey, +1,
                                                         *side_work, now, out);
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
              out.push_back({partkey, old.nation, Q9Pair::kYearBase + yi, -old.v[yi]});
        if (now_valid)
          for (int32_t yi = 0; yi < Q9Pair::kYears; ++yi)
            if (now.v[yi] != 0)
              out.push_back({partkey, now.nation, Q9Pair::kYearBase + yi, now.v[yi]});
      }
    });
    ApplyRenamedPairs(+1, *side_work);
  }

  void Publish() {
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
          TpchQ9Layout::kAccelerator, TpchQ9Layout::kMetadata, metadata_version,
          MakeImage(TpchQ9ImageKind::Metadata, mutable_meta)));
    }

    for (const auto slot : touched_slots) {
      // new_state is discarded after this call, so move rather than copy.
      outcome.updates.push_back(core::StateImage::Make(
          TpchQ9Layout::kAccelerator, TpchQ9Layout::ProfitSlotPartition(slot), 0,
          MakeProfitSlotImage(slot_count, slot,
                              std::move(new_state->profit_partitions[slot]))));
    }

    if (!touched_slots.empty()) {
      auto cube_state = std::make_shared<TpchQ9State>();
      cube_state->cube = std::move(new_state->cube);
      cube_state->cube_active = true;
      cube_state->profit_slot_count = slot_count;
      outcome.updates.push_back(core::StateImage::Make(
          TpchQ9Layout::kAccelerator, TpchQ9Layout::kCube, cube_version,
          MakeImage(TpchQ9ImageKind::Cube, cube_state)));
    }
    if (side_dirty) {
      outcome.updates.push_back(core::StateImage::Make(
          TpchQ9Layout::kAccelerator, TpchQ9Layout::kMaintenanceSide, side_version,
          MakeImage(TpchQ9ImageKind::MaintenanceSide, side_work)));
    }
  }

  core::AuxEngineServices &services;
  const TpchQ9TxnBuffer &buffer;
  TpchQ9InitialScan *initial_scan;
  core::StagedMaintenanceOutcome &outcome;
  const core::VisibilityToken visibility;

  std::optional<core::StateSnapshot> meta_snapshot, aux_snapshot, slot_snapshot;
  const TpchQ9StateImage *metadata_image = nullptr;
  const TpchQ9State *join_side = nullptr;
  core::Version metadata_version = 0, side_version = 0, cube_version = 0;
  core::PartitionId slot_count = 0;
  bool touch_names = false;
  std::shared_ptr<TpchQ9State> new_state;
  // The join side is a COW copy of the published side image.
  std::shared_ptr<TpchQ9State> side_work;
  bool side_dirty = false;

  std::set<core::PartitionId> touched_slots;
  std::vector<std::pair<int64_t, int32_t>> affected;
  std::unordered_set<int64_t> renamed;
  std::size_t chunk_count = 0;
  std::vector<std::vector<TpchQ9State::CellDelta>> net_chunks;
  std::vector<TpchQ9State::PairSnap> snaps;
  std::vector<uint8_t> snap_valid;
};

struct Q9Accumulator {
  int64_t dense_profit[kDenseNations][kDenseYears] = {};
  bool dense_valid[kDenseNations][kDenseYears] = {};
  bool nation_seen[kDenseNations] = {};
  std::unordered_map<Q9CellKey, int64_t, Q9CellKeyHash> sparse;

  void AddCell(int32_t nation, int32_t year, int64_t profit) {
    const int32_t year_idx = year - kDenseYearBase;
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
          core::AuxType::Int64, static_cast<int64_t>(kDenseYearBase + yi)));
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


// Candidate parts (or every part when null) of the needle summed over the
// profit slots read in place; slots[i] may be null = absent.
void EmitFromSlots(const core::AuxEngineServices &services, const TpchQ9State &meta,
                   const std::vector<const TpchQ9State::PartProfitPartition *> &slots,
                   const std::string &needle, const std::vector<int32_t> *candidates,
                   core::AuxRowSink &sink) {
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

// The needle is one whole token present in the index: its cube cells answer it.
void EmitFromCube(const TpchQ9State &meta, const TpchQ9State &cube_state,
                  const std::string &needle, core::AuxRowSink &sink) {
  Q9Accumulator acc;
  const auto it = cube_state.cube.find(needle);
  if (it != cube_state.cube.end()) {
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
}

// The profit slots the candidates hash to (every slot without candidates),
// read in place from `snapshot`.
std::vector<const TpchQ9State::PartProfitPartition *>
LoadSlotViews(core::AuxEngineServices &services, const core::VisibilityToken &visibility,
              const TpchQ9State &meta, const std::vector<int32_t> *candidates,
              std::optional<core::StateSnapshot> &snapshot) {
  const auto slot_count = meta.profit_slot_count;
  std::vector<core::PartitionId> slot_partitions;
  if (candidates) {
    std::vector<bool> seen(slot_count, false);
    for (const auto pk : *candidates) {
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

  std::vector<const TpchQ9State::PartProfitPartition *> slots(slot_count, nullptr);
  if (slot_partitions.empty())
    return slots;
  snapshot.emplace(services.StateStore().ReadPartitions(visibility, slot_partitions));
  for (const auto &image : snapshot->partitions) {
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
  return slots;
}

} // namespace

int32_t TpchQ9ExtractYear(int32_t days_since_epoch) {
  return core::sql_date::YearOf(days_since_epoch);
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
      services, profit_partitions.size(), kSlotFoldGrain, *this,
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
  for (auto &entry : name_postings) {
    auto &posting = *entry.second;
    std::sort(posting.begin(), posting.end());
    posting.erase(std::unique(posting.begin(), posting.end()), posting.end());
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

void TpchQ9State::RemoveNamePostings(int64_t partkey) {
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

void TpchQ9State::BuildCellsFromPairs(const core::AuxEngineServices &services) {
  static const std::vector<std::string> kNoTokens;
  // Each record's cells are written only by its own slot; the side maps are read-only.
  core::ParallelSlices(
      services, profit_partitions.size(), kSlotGrain,
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

  TokenAcc ta;
  core::ParallelReduce(
      services, active.size(), kSlotFoldGrain, ta,
      [&](TokenAcc &acc, std::size_t begin, std::size_t end) {
        for (std::size_t t = begin; t < end; ++t)
          ApplySlotDeltas(*this, by_slot[active[t]], acc);
      },
      [](TokenAcc &into, TokenAcc &&from) { into.Merge(std::move(from)); });

  for (std::size_t t = 0; t < ta.names.size(); ++t) {
    const std::vector<std::string> one{std::string(ta.names[t])};
    const auto &acc = ta.acc[t];
    for (int n = 0; n < kDenseNations; ++n)
      for (int y = 0; y < kDenseYears; ++y)
        if (acc[n * kDenseYears + y] != 0)
          CubeAdd(one, n, kDenseYearBase + y, acc[n * kDenseYears + y]);
  }
  for (std::size_t k = 0; k < ta.sparse.size(); ++k)
    CubeAdd({std::string(ta.sparse_tokens[k])}, ta.sparse[k].nation,
            ta.sparse[k].year, ta.sparse[k].delta);
}

bool TpchQ9TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ9TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
  order_deltas.clear();
  supplier_deltas.clear();
  partsupp_deltas.clear();
  part_name_deltas.clear();
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
                                  TpchQ9State &state) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q9 aux requires a typed initial scan source");

  initial_scan->ScanNations([&](const TpchQ9NationInput &input) {
    state.nation_names.emplace(input.nationkey, input.name);
  });

  // Scan parts for the name index, then size the profit slots from cardinality.
  std::vector<std::pair<int64_t, std::string>> parts;
  parts.reserve(kPartScanReserve);
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

  BuildPairAggregates(services, *initial_scan, state);

  state.FinishBulkParts();
  state.FinalizePostings();
  state.RebuildNationEmitOrder();
  state.BuildCellsFromPairs(services);
  state.BuildSuppPairsIndex();
  state.BuildCube(services);
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
      TpchQ9Layout::kAccelerator, TpchQ9Layout::kMetadata, 0,
      MakeImage(TpchQ9ImageKind::Metadata, metadata_state)));

  // Dense join maps live on kMaintenanceSide; Execute never reads it.
  auto side_state = std::make_shared<TpchQ9State>();
  side_state->CopySideFrom(*state);
  state->supplier_nations.Clear();
  state->order_years.Clear();
  state->partsupp_rows.Clear();
  builder.Write(core::StateImage::Make(
      TpchQ9Layout::kAccelerator, TpchQ9Layout::kMaintenanceSide, 0,
      MakeImage(TpchQ9ImageKind::MaintenanceSide, side_state)));

  auto cube_state = std::make_shared<TpchQ9State>();
  cube_state->cube = std::move(state->cube);
  cube_state->cube_active = true;
  cube_state->profit_slot_count = state->profit_slot_count;
  builder.Write(core::StateImage::Make(
      TpchQ9Layout::kAccelerator, TpchQ9Layout::kCube, 0,
      MakeImage(TpchQ9ImageKind::Cube, cube_state)));

  for (core::PartitionId slot = 0; slot < state->profit_slot_count; ++slot) {
    builder.Write(core::StateImage::Make(
        TpchQ9Layout::kAccelerator, TpchQ9Layout::ProfitSlotPartition(slot), 0,
        MakeProfitSlotImage(state->profit_slot_count, slot,
                            std::move(state->profit_partitions[slot]))));
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
  if (mutation.table == config.part_table)
    AccumulatePart(mutation, buffer);
  else if (mutation.table == config.supplier_table)
    AccumulateSupplier(mutation, buffer);
  else if (mutation.table == config.nation_table)
    buffer.MarkRebuild(); // n_name changes are side-store; n_nationkey changes rebuild
  else if (mutation.table == config.orders_table)
    AccumulateOrders(mutation, buffer);
  else if (mutation.table == config.partsupp_table)
    AccumulatePartsupp(mutation, buffer);
  else if (mutation.table == config.lineitem_table)
    AccumulateLineitems(mutation, buffer);
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
    core::FinishRebuildAll(
        outcome, services.ChooseMaintenance(core::MaintenanceRequest::RebuildOnly(
                     services.ExpectedQueriesBeforeNextWrite())));
    return outcome;
  }

  if (!OrderYearsConsistent(buffer)) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }

  const auto total_changes =
      buffer.Deltas().size() + buffer.PartNameDeltas().size() +
      buffer.OrderDeltas().size() + buffer.SupplierDeltas().size() +
      buffer.PartsuppDeltas().size();
  const auto decision = services.ChooseMaintenance(
      core::MaintenanceRequest::IncrementalSingleState(
          total_changes, services.ExpectedQueriesBeforeNextWrite()));
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    core::FinishRebuildAll(outcome, decision);
    return outcome;
  }

  ProfitMaintainer maintainer(services, buffer, initial_scan, outcome);
  if (!maintainer.Run())
    core::FinishRebuildAll(outcome);
  return outcome;
}

bool TpchQ9Module::CheckValidity(core::AuxEngineServices &services,
                                 const core::QueryArguments &arguments) const {
  if (arguments.values.empty())
    return false;
  // Validate only the metadata partition: every answer needs it.
  return services.StateStore()
      .CheckValidity({Id(), std::to_string(TpchQ9Layout::kMetadata)},
                     services.CurrentVisibility())
      .IsValid();
}

void TpchQ9Module::Execute(core::AuxEngineServices &services,
                           const core::QueryArguments &arguments,
                           core::AuxRowSink &sink) {
  if (arguments.values.empty() || arguments.values[0].is_null ||
      !std::holds_alternative<std::string>(arguments.values[0].payload)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  const std::string &color = std::get<std::string>(arguments.values[0].payload);
  if (color.empty() || !CheckValidity(services, arguments)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  // One snapshot of metadata + cube; everything is read in place from the
  // shared immutable images (no assembly / copying).
  const auto visibility = services.CurrentVisibility();
  const auto snapshot = services.StateStore().ReadPartitions(
      visibility, {TpchQ9Layout::kMetadata, TpchQ9Layout::kCube});
  const TpchQ9StateImage *meta_image = nullptr;
  const TpchQ9StateImage *cube_image = nullptr;
  for (const auto &image : snapshot.partitions) {
    const auto *typed = AsQ9Image(image);
    if (!typed)
      continue;
    if (image.partition == TpchQ9Layout::kMetadata)
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
  if (tokens.empty()) {
    EmitEmpty(sink);
    return;
  }

  // Cube fast path: needle is exactly one whole token present in the index.
  if (cube_image && tokens.size() == 1 && tokens[0] == needle &&
      meta.name_postings.find(needle) != meta.name_postings.end()) {
    EmitFromCube(meta, *cube_image->state, needle, sink);
    return;
  }

  // General path: candidate parts from the posting intersection (or every
  // part when the needle is not a whole indexed token), slots read in place.
  std::vector<int32_t> candidates;
  const bool use_candidates = IntersectPostings(meta, tokens, candidates);
  std::optional<core::StateSnapshot> slot_snapshot;
  const auto slots = LoadSlotViews(services, visibility, meta,
                                   use_candidates ? &candidates : nullptr,
                                   slot_snapshot);
  EmitFromSlots(services, meta, slots, needle, use_candidates ? &candidates : nullptr,
                sink);
}

std::vector<core::CalibrationWrite> TpchQ9Module::CalibrationWrites() const {
  return TpchQ9CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
