#include "tpch/q15/duckdb/aux/module.hpp"
#include "prims/sql_date.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"
#include "tpch/q15/calibration_writes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

// Windows per slice (untimed): a rescan walks every supplier slot.
constexpr std::size_t kWindowGrain = 8;
// Lineitem deltas below which a commit applies its month windows inline (untimed).
constexpr std::size_t kDeltaOpThreshold = 4096;

int32_t MonthKey(int32_t days) {
  const auto civil = core::sql_date::CivilFromDays(days);
  return civil.year * 100 + civil.month;
}

int32_t StartOfMonthDays(int32_t days) {
  return static_cast<int32_t>(core::sql_date::MonthStart(days));
}

int32_t AddMonthsDays(int32_t days, int32_t months) {
  return static_cast<int32_t>(core::sql_date::AddMonths(days, months));
}

// Each lineitem with shipdate days S falls into up to three month-start
// 3-month rolling windows: those starting at StartOfMonth(S-2m),
// StartOfMonth(S-1m), and StartOfMonth(S).
std::vector<int32_t> CoveringWindowMonths(int32_t shipdate_days) {
  std::vector<int32_t> months;
  const int32_t m0 = StartOfMonthDays(shipdate_days);
  months.push_back(MonthKey(m0));
  const int32_t m_1 = StartOfMonthDays(AddMonthsDays(m0, -1));
  months.push_back(MonthKey(m_1));
  const int32_t m_2 = StartOfMonthDays(AddMonthsDays(m_1, -1));
  months.push_back(MonthKey(m_2));
  return months;
}

int32_t PrevMonthKey(int32_t key) {
  return key % 100 == 1 ? key - 100 + 11 : key - 1;
}

// Window keys (month-start YYYYMM) of the three rolling windows containing a
// month: the month itself and the two before it.
void CoveringWindowKeysOfMonth(int32_t month_key, int32_t out[3]) {
  out[0] = month_key;
  out[1] = PrevMonthKey(out[0]);
  out[2] = PrevMonthKey(out[1]);
}

std::shared_ptr<const TpchQ15StateImage>
MakeDirImage(const std::shared_ptr<const TpchQ15State> &state) {
  auto image = std::make_shared<TpchQ15StateImage>();
  image->kind = TpchQ15ImageKind::SupplierDirectory;
  image->month_key = 0;
  image->state = state;
  return image;
}

std::shared_ptr<const TpchQ15StateImage>
MakeWindowImage(int32_t month_key,
                const std::shared_ptr<const TpchQ15State> &state) {
  auto image = std::make_shared<TpchQ15StateImage>();
  image->kind = TpchQ15ImageKind::MonthWindow;
  image->month_key = month_key;
  image->state = state;
  return image;
}

const TpchQ15StateImage *AsQ15Image(const core::StateImage &image,
                                     TpchQ15ImageKind kind) {
  const auto *payload = image.As<TpchQ15StateImage>();
  return payload && payload->kind == kind && payload->state ? payload : nullptr;
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

bool ReadLineitem(const core::AuxRow &row, TpchQ15LineitemInput &out) {
  int64_t suppkey = 0;
  int64_t extended_raw = 0, discount_raw = 0;
  int32_t extended_scale = 0, discount_scale = 0;
  // Mutation rows are [shipdate, suppkey, extendedprice, discount].
  if (!core::DateCell(row, 0, out.shipdate_days) ||
      !core::IntCell(row, 1, suppkey) ||
      !core::DecimalCell(row, 2, extended_raw, extended_scale) ||
      !core::DecimalCell(row, 3, discount_raw, discount_scale))
    return false;
  out.suppkey = suppkey;
  out.extended_price_raw = extended_raw;
  out.extended_price_scale = extended_scale;
  out.discount_raw = discount_raw;
  out.discount_scale = discount_scale;
  return true;
}

bool ReadSupplier(const core::AuxRow &row, TpchQ15SupplierInput &out) {
  int64_t suppkey = 0;
  if (!core::IntCell(row, 0, suppkey))
    return false;
  out.suppkey = suppkey;
  if (!core::StringCell(row, 1, out.name))
    return false;
  if (!core::StringCell(row, 2, out.address))
    return false;
  if (!core::StringCell(row, 3, out.phone))
    return false;
  return true;
}

// Applies one month's lineitem deltas, maintaining the max/winner set
// incrementally; a full pass over the dense array is needed only when the
// sole current winner decreases.
void ApplyWindowDeltas(TpchQ15WindowState &ws,
                       const std::vector<TpchQ15LineitemDelta> &deltas,
                       const std::vector<uint32_t> &indices) {
  bool rescan = false;
  for (const auto index : indices) {
    const auto &delta = deltas[index];
    const int64_t signed_rev =
        static_cast<int64_t>(delta.sign) * delta.revenue_raw;
    if (signed_rev == 0)
      continue;
    const int64_t old_rev = ws.rev.Add(delta.suppkey, signed_rev);
    const int64_t new_rev = old_rev + signed_rev;
    if (old_rev == 0)
      ++ws.nonzero;
    if (new_rev == 0)
      --ws.nonzero;
    ws.empty = ws.nonzero == 0;
    if (rescan)
      continue;
    const bool was_winner =
        old_rev != 0 && !ws.winners.empty() && old_rev == ws.max_revenue;
    const auto pos = std::lower_bound(ws.winners.begin(), ws.winners.end(),
                                      delta.suppkey);
    if (was_winner && new_rev < old_rev) {
      if (ws.winners.size() > 1)
        ws.winners.erase(pos); // max unchanged: another supplier holds it
      else
        rescan = true;
    } else if (new_rev != 0 &&
               (ws.winners.empty() || new_rev > ws.max_revenue)) {
      ws.max_revenue = new_rev;
      ws.winners.assign(1, delta.suppkey);
    } else if (new_rev != 0 && new_rev == ws.max_revenue && !was_winner) {
      ws.winners.insert(pos, delta.suppkey);
    }
  }
  if (rescan || ws.nonzero == 0)
    ws.Rescan();
}


// A lineitem's revenue contribution, bucketed by the dense chunk of its supplier
// so one reduction task owns all the chunks of its bucket in every window.
struct RevenueEntry {
  int32_t month_key;
  int64_t suppkey;
  int64_t revenue;
};

constexpr std::size_t kRevenueBuckets = 1024;
constexpr std::size_t kDenseChunkSize = decltype(TpchQ15RevArray::dense)::kChunkSize;

std::size_t RevenueBucketOf(int64_t suppkey) {
  if (suppkey < 0 || suppkey >= TpchQ15RevArray::kMaxDenseKey)
    return 0; // the overflow map has a single owner
  return static_cast<std::size_t>(suppkey) / kDenseChunkSize % kRevenueBuckets;
}

// What the scan keeps per partition: O(rows seen), never sized by the supplier
// domain. Merging only collects the runs; the windows are summed afterwards.
struct RevenueRuns {
  using Bucketed = std::vector<std::vector<RevenueEntry>>;
  std::vector<Bucketed> runs;
  std::set<int32_t> months;
  int64_t max_dense_key = -1;

  RevenueRuns() : runs(1, Bucketed(kRevenueBuckets)) {}

  void Add(int32_t month_key, int64_t suppkey, int64_t revenue) {
    runs.back()[RevenueBucketOf(suppkey)].push_back({month_key, suppkey, revenue});
    months.insert(month_key);
    if (suppkey < TpchQ15RevArray::kMaxDenseKey)
      max_dense_key = std::max(max_dense_key, suppkey);
  }
  void MergeFrom(RevenueRuns &&from) {
    runs.insert(runs.end(), std::make_move_iterator(from.runs.begin()),
                std::make_move_iterator(from.runs.end()));
    months.insert(from.months.begin(), from.months.end());
    max_dense_key = std::max(max_dense_key, from.max_dense_key);
  }
};

// Revenue per (window, supplier); each month feeds the three rolling windows
// that contain it. Bucket tasks write disjoint chunks of the pre-sized windows.
void AccumulateWindows(core::AuxEngineServices &services, TpchQ15InitialScan &scan,
                       TpchQ15State &state) {
  const auto collected = core::PartitionedBuild<RevenueRuns>(
      services, [] { return RevenueRuns{}; },
      [&](std::size_t parts, auto &&sink) {
        scan.ScanLineitems(services, parts, [&](std::size_t p, const TpchQ15LineitemInput &input) {
          sink(p, core::SingleRowBatch<TpchQ15LineitemInput>{input});
        });
      },
      [](RevenueRuns &partial, const TpchQ15LineitemInput &input) {
        partial.Add(MonthKey(input.shipdate_days), input.suppkey,
                    TpchQ15RevenueRaw(input.extended_price_raw, input.extended_price_scale,
                                      input.discount_raw, input.discount_scale));
      },
      [](RevenueRuns &into, RevenueRuns &&from) { into.MergeFrom(std::move(from)); });

  std::unordered_map<int32_t, std::array<TpchQ15WindowState *, 3>> windows_of;
  for (const auto month_key : collected.months) {
    int32_t keys[3];
    CoveringWindowKeysOfMonth(month_key, keys);
    auto &targets = windows_of[month_key];
    for (int i = 0; i < 3; ++i) {
      auto &window = state.windows[keys[i]];
      if (collected.max_dense_key >= 0)
        window.rev.dense.Reserve(collected.max_dense_key);
      targets[static_cast<std::size_t>(i)] = &window;
    }
  }
  core::ParallelEach(services, kRevenueBuckets, 1, [&](std::size_t bucket) {
    int32_t cached_month = 0;
    const std::array<TpchQ15WindowState *, 3> *targets = nullptr;
    for (const auto &run : collected.runs)
      for (const auto &entry : run[bucket]) {
        if (!targets || entry.month_key != cached_month) {
          cached_month = entry.month_key;
          targets = &windows_of.at(cached_month);
        }
        for (auto *window : *targets)
          window->rev.Add(entry.suppkey, entry.revenue);
      }
  });
}

} // namespace

void TpchQ15WindowState::Rescan() {
  int64_t count = 0;
  int64_t max_rev = std::numeric_limits<int64_t>::min();
  winners.clear();
  rev.ForEachNonZero([&](int64_t key, int64_t r) {
    ++count;
    if (r > max_rev) {
      max_rev = r;
      winners.clear();
      winners.push_back(key);
    } else if (r == max_rev) {
      winners.push_back(key);
    }
  });
  nonzero = count;
  empty = count == 0;
  max_revenue = count == 0 ? 0 : max_rev;
}

void TpchQ15SupplierDirectory::FindAll(
    int64_t suppkey, std::vector<const TpchQ15SupplierRecord *> &out) const {
  const auto it = chunks.find(suppkey >> kQ15DirChunkShift);
  if (it == chunks.end())
    return;
  const auto &items = it->second->items;
  auto pos = std::lower_bound(
      items.begin(), items.end(), suppkey,
      [](const auto &e, int64_t k) { return e.first < k; });
  for (; pos != items.end() && pos->first == suppkey; ++pos)
    out.push_back(pos->second.get());
}

void TpchQ15SupplierDirectory::Add(int64_t suppkey, TpchQ15SupplierRecord record) {
  auto &slot = chunks[suppkey >> kQ15DirChunkShift];
  if (!slot)
    slot = std::make_shared<TpchQ15DirChunk>();
  else if (slot.use_count() > 1)
    slot = std::make_shared<TpchQ15DirChunk>(*slot);
  auto &items = slot->items;
  auto rec = std::make_shared<const TpchQ15SupplierRecord>(std::move(record));
  const auto pos = std::upper_bound(
      items.begin(), items.end(), suppkey,
      [](int64_t k, const auto &e) { return k < e.first; });
  items.insert(pos, {suppkey, std::move(rec)});
}

void TpchQ15SupplierDirectory::MergeFrom(TpchQ15SupplierDirectory &&other) {
  for (auto &[chunk_id, theirs] : other.chunks) {
    auto &mine = chunks[chunk_id];
    if (!mine) {
      mine = std::move(theirs);
      continue;
    }
    auto &items = mine->items;
    const auto middle = static_cast<std::ptrdiff_t>(items.size());
    items.insert(items.end(), theirs->items.begin(), theirs->items.end());
    std::inplace_merge(items.begin(), items.begin() + middle, items.end(),
                       [](const auto &a, const auto &b) { return a.first < b.first; });
  }
}

void TpchQ15SupplierDirectory::EraseOne(int64_t suppkey,
                                        const TpchQ15SupplierRecord &record) {
  const auto it = chunks.find(suppkey >> kQ15DirChunkShift);
  if (it == chunks.end())
    return;
  const auto same = [&](const auto &e) {
    return e.first == suppkey && e.second->name == record.name &&
           e.second->address == record.address &&
           e.second->phone == record.phone;
  };
  auto *chunk = it->second.get();
  if (std::none_of(chunk->items.begin(), chunk->items.end(), same))
    return;
  if (it->second.use_count() > 1) {
    it->second = std::make_shared<TpchQ15DirChunk>(*it->second);
    chunk = it->second.get();
  }
  chunk->items.erase(
      std::find_if(chunk->items.begin(), chunk->items.end(), same));
  if (chunk->items.empty())
    chunks.erase(it);
}

std::size_t TpchQ15SupplierDirectory::Size() const {
  std::size_t n = 0;
  for (const auto &[cid, chunk] : chunks)
    n += chunk->items.size();
  return n;
}

int64_t TpchQ15RevenueRaw(int64_t extended_price_raw, int32_t,
                          int64_t discount_raw, int32_t) {
  return extended_price_raw * (100 - discount_raw);
}

bool TpchQ15TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ15TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  lineitem_deltas.clear();
  supplier_deltas.clear();
}
bool TpchQ15TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ15LineitemDelta> &TpchQ15TxnBuffer::LineitemDeltas() const {
  return lineitem_deltas;
}
const std::vector<TpchQ15SupplierDelta> &TpchQ15TxnBuffer::SupplierDeltas() const {
  return supplier_deltas;
}
void TpchQ15TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  lineitem_deltas.clear();
  supplier_deltas.clear();
}
void TpchQ15TxnBuffer::AddLineitemDelta(TpchQ15LineitemDelta delta) {
  pending = true;
  lineitem_deltas.push_back(delta);
}
void TpchQ15TxnBuffer::AddSupplierDelta(TpchQ15SupplierDelta delta) {
  pending = true;
  supplier_deltas.push_back(delta);
}

core::AcceleratorId TpchQ15Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ15Module::Dependencies() const {
  return {{config.lineitem_table, {0, 1, 2, 3}, {10, 2, 5, 6}},
          {config.supplier_table, {0, 1, 2, 3}, {0, 1, 2, 4}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ15Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ15TxnBuffer>();
}

void TpchQ15Module::Build(core::AuxEngineServices &services,
                           core::StateBuilder &builder) {
  auto built = BuildState(services);
  const auto typed_state = std::make_shared<const TpchQ15State>(std::move(built));
  const auto &state = *typed_state;

  builder.Write(core::StateImage::Make(
      TpchQ15Layout::kAccelerator, TpchQ15Layout::kSupplierDirectory, 0,
      MakeDirImage(typed_state)));

  for (const auto &entry : state.windows) {
    builder.Write(core::StateImage::Make(
        TpchQ15Layout::kAccelerator,
        static_cast<core::PartitionId>(entry.first), 0,
        MakeWindowImage(entry.first, typed_state)));
  }
  builder.Finish();
}

core::InvalidationPlan
TpchQ15Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table ||
      mutation.table == config.supplier_table) {
    return All();
  }
  return {};
}

void TpchQ15Module::AccumulateMutation(const core::MutationBatch &mutation,
                                        core::AuxEngineServices &,
                                        core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ15TxnBuffer &>(base_buffer);

  if (mutation.table == config.lineitem_table) {
    auto add = [&](const core::AuxRow &row, int8_t sign) {
      TpchQ15LineitemInput input;
      if (!ReadLineitem(row, input)) {
        buffer.MarkRebuild();
        return;
      }
      const auto revenue =
          TpchQ15RevenueRaw(input.extended_price_raw,
                            input.extended_price_scale,
                            input.discount_raw, input.discount_scale);
      for (const auto month_key : CoveringWindowMonths(input.shipdate_days)) {
        TpchQ15LineitemDelta delta;
        delta.month_key = month_key;
        delta.suppkey = input.suppkey;
        delta.revenue_raw = revenue;
        delta.sign = sign;
        buffer.AddLineitemDelta(delta);
      }
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
  } else if (mutation.table == config.supplier_table) {
    auto make = [](const TpchQ15SupplierInput *old_in,
                   const TpchQ15SupplierInput *new_in) {
      TpchQ15SupplierDelta delta;
      delta.suppkey = old_in ? old_in->suppkey : new_in->suppkey;
      if (old_in) {
        delta.has_old = true;
        delta.old_name = old_in->name;
        delta.old_address = old_in->address;
        delta.old_phone = old_in->phone;
      }
      if (new_in) {
        delta.has_new = true;
        delta.new_name = new_in->name;
        delta.new_address = new_in->address;
        delta.new_phone = new_in->phone;
      }
      return delta;
    };
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows) {
        TpchQ15SupplierInput input;
        if (ReadSupplier(row, input))
          buffer.AddSupplierDelta(make(nullptr, &input));
        else
          buffer.MarkRebuild();
      }
      break;
    case core::MutationKind::Delete:
      for (const auto &row : mutation.old_rows) {
        TpchQ15SupplierInput input;
        if (ReadSupplier(row, input))
          buffer.AddSupplierDelta(make(&input, nullptr));
        else
          buffer.MarkRebuild();
      }
      break;
    case core::MutationKind::Update:
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        TpchQ15SupplierInput old_input, new_input;
        if (!ReadSupplier(mutation.old_rows[i], old_input) ||
            !ReadSupplier(mutation.new_rows[i], new_input)) {
          buffer.MarkRebuild();
          return;
        }
        // Unchanged projection (e.g. only s_acctbal/s_comment changed): no-op.
        if (old_input.suppkey == new_input.suppkey &&
            old_input.name == new_input.name &&
            old_input.address == new_input.address &&
            old_input.phone == new_input.phone)
          continue;
        buffer.AddSupplierDelta(make(&old_input, nullptr));
        buffer.AddSupplierDelta(make(nullptr, &new_input));
      }
      break;
    }
  }
}

core::StagedMaintenanceOutcome
TpchQ15Module::StageMaintenance(core::AuxEngineServices &services,
                                 const core::InvalidationPlan &,
                                 core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ15TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;

  if (buffer.RequiresRebuild()) {
    core::FinishRebuildAll(outcome, services.ChooseMaintenance(core::MaintenanceRequest::RebuildOnly(
                                        services.ExpectedQueriesBeforeNextWrite())));
    return outcome;
  }

  if (buffer.LineitemDeltas().empty() && buffer.SupplierDeltas().empty())
    return outcome;

  const auto visibility = services.CurrentVisibility();

  std::set<int32_t> touched_months;
  for (const auto &delta : buffer.LineitemDeltas())
    touched_months.insert(delta.month_key);

  const bool has_supplier_deltas = !buffer.SupplierDeltas().empty();
  const bool can_rebuild_windows =
      !touched_months.empty() && !has_supplier_deltas && initial_scan;

  auto request = core::MaintenanceRequest::IncrementalPartitioned(
      buffer.LineitemDeltas().size() + buffer.SupplierDeltas().size(),
      std::max<std::size_t>(1, touched_months.size()), services.ExpectedQueriesBeforeNextWrite());
  request.partition_rebuild_supported = can_rebuild_windows;
  const auto decision = services.ChooseMaintenance(request);

  if (decision.action == core::MaintenanceAction::RebuildPartition) {
    if (!can_rebuild_windows) {
      core::FinishRebuildAll(outcome);
      return outcome;
    }
    int32_t scan_lo = std::numeric_limits<int32_t>::max();
    int32_t scan_hi = std::numeric_limits<int32_t>::min();
    for (const auto month_key : touched_months) {
      const int32_t start = static_cast<int32_t>(
          core::sql_date::DaysFromCivil(month_key / 100, month_key % 100, 1));
      const int32_t end = AddMonthsDays(start, 3);
      scan_lo = std::min(scan_lo, start);
      scan_hi = std::max(scan_hi, end);
    }
    std::map<int32_t, TpchQ15WindowState> rebuilt;
    initial_scan->ScanLineitemsForShipdateRange(
        scan_lo, scan_hi, [&](const TpchQ15LineitemInput &input) {
          const auto revenue = TpchQ15RevenueRaw(
              input.extended_price_raw, input.extended_price_scale,
              input.discount_raw, input.discount_scale);
          for (const auto month_key :
               CoveringWindowMonths(input.shipdate_days)) {
            if (!touched_months.count(month_key))
              continue;
            rebuilt[month_key].rev.Add(input.suppkey, revenue);
          }
        });
    for (const auto month : touched_months) {
      TpchQ15WindowState ws;
      const auto it = rebuilt.find(month);
      if (it != rebuilt.end())
        ws = std::move(it->second);
      ws.Rescan();
      auto win_state = std::make_shared<const TpchQ15State>(
          TpchQ15State{TpchQ15SupplierDirectory{}, {{month, std::move(ws)}}});
      outcome.updates.push_back(core::StateImage::Make(
          TpchQ15Layout::kAccelerator, static_cast<core::PartitionId>(month), 0,
          MakeWindowImage(month, win_state)));
    }
    core::FinishRebuildPartition(outcome);
    return outcome;
  }
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    core::FinishRebuildAll(outcome, decision);
    return outcome;
  }

  // Load affected windows
  std::map<int32_t, TpchQ15WindowState> windows;
  for (const auto month : touched_months) {
    TpchQ15WindowState ws;
    if (!LoadMonth(services, month, ws)) {
      core::FinishRebuildAll(outcome);
      return outcome;
    }
    windows[month] = std::move(ws);
  }

  // Months are independent windows: each slice applies the deltas of whole
  // months, in arrival order.
  std::map<int32_t, std::vector<uint32_t>> month_deltas;
  for (uint32_t i = 0; i < buffer.LineitemDeltas().size(); ++i)
    month_deltas[buffer.LineitemDeltas()[i].month_key].push_back(i);
  std::vector<std::pair<TpchQ15WindowState *, const std::vector<uint32_t> *>>
      jobs;
  for (auto &[month, ws] : windows)
    jobs.emplace_back(&ws, &month_deltas[month]);
  core::ParallelEachWhen(services, jobs.size(), buffer.LineitemDeltas().size(), kDeltaOpThreshold,
                         [&](std::size_t j) {
                           ApplyWindowDeltas(*jobs[j].first, buffer.LineitemDeltas(), *jobs[j].second);
                         });

  // Apply supplier deltas to a directory copy (chunk-level copy on write).
  TpchQ15SupplierDirectory dir_copy;
  if (has_supplier_deltas) {
    if (!LoadDirectory(services, dir_copy)) {
      core::FinishRebuildAll(outcome);
      return outcome;
    }
    for (const auto &delta : buffer.SupplierDeltas()) {
      if (delta.has_old)
        dir_copy.EraseOne(delta.suppkey,
                          {delta.old_name, delta.old_address, delta.old_phone});
      if (delta.has_new)
        dir_copy.Add(delta.suppkey,
                     {delta.new_name, delta.new_address, delta.new_phone});
    }
  }

  outcome.action = core::MaintenanceAction::ApplyDelta;

  if (has_supplier_deltas) {
    auto dir_state = std::make_shared<const TpchQ15State>(
        TpchQ15State{std::move(dir_copy), {}});
    outcome.updates.push_back(core::StateImage::Make(
        TpchQ15Layout::kAccelerator, TpchQ15Layout::kSupplierDirectory, 0,
        MakeDirImage(dir_state)));
  }

  for (auto &[month, ws] : windows) {
    const auto partition = static_cast<core::PartitionId>(month);
    auto win_state = std::make_shared<const TpchQ15State>(
        TpchQ15State{TpchQ15SupplierDirectory{}, {{month, std::move(ws)}}});
    outcome.updates.push_back(core::StateImage::Make(
        TpchQ15Layout::kAccelerator, partition, 0,
        MakeWindowImage(month, win_state)));
  }

  return outcome;
}

bool TpchQ15Module::CheckValidity(core::AuxEngineServices &services,
                                   const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ15Module::Execute(core::AuxEngineServices &services,
                             const core::QueryArguments &arguments,
                             core::AuxRowSink &sink) {
  int32_t start = 0;
  if (!arguments.values.empty() && !arguments.values[0].is_null) {
    if (std::holds_alternative<core::AuxDate>(arguments.values[0].payload))
      start =
          std::get<core::AuxDate>(arguments.values[0].payload).days_since_epoch;
    else if (std::holds_alternative<int64_t>(arguments.values[0].payload))
      start = static_cast<int32_t>(std::get<int64_t>(arguments.values[0].payload));
  }

  if (!core::sql_date::IsMonthStart(start)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  if (!services.StateStore()
           .CheckValidity({Id(), {}}, services.CurrentVisibility())
           .IsValid()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  const auto month_key = MonthKey(start);

  // Direct state access: reference the shared state; only winners +
  // max_revenue are needed (no revenue array access).
  // The window and the supplier directory from one snapshot.
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {static_cast<core::PartitionId>(month_key), TpchQ15Layout::kSupplierDirectory});
  const TpchQ15WindowState *win = nullptr;
  const TpchQ15SupplierDirectory *dir = nullptr;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ15Layout::kSupplierDirectory) {
      if (const auto *typed = AsQ15Image(image, TpchQ15ImageKind::SupplierDirectory))
        dir = &typed->state->directory;
      continue;
    }
    if (static_cast<int32_t>(image.partition) != month_key)
      continue;
    const auto *typed = AsQ15Image(image, TpchQ15ImageKind::MonthWindow);
    if (!typed || typed->month_key != month_key)
      continue;
    const auto it = typed->state->windows.find(month_key);
    if (it != typed->state->windows.end())
      win = &it->second;
  }
  if (!win || !dir) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  if (win->empty || win->winners.empty()) {
    sink.Begin({{{"s_suppkey", core::AuxType::Int64, true},
                 {"s_name", core::AuxType::String, true},
                 {"s_address", core::AuxType::String, true},
                 {"s_phone", core::AuxType::String, true},
                 {"total_revenue", core::AuxType::ScaledDecimal, true}}});
    sink.End();
    return;
  }

  // Join the winners with the directory as of this snapshot (a supplier no
  // longer there drops out; renamed ones show their current record).
  std::vector<std::pair<int64_t, TpchQ15SupplierRecord>> winners;
  winners.reserve(win->winners.size());
  std::vector<const TpchQ15SupplierRecord *> matches;
  for (const auto sk : win->winners) {
    matches.clear();
    dir->FindAll(sk, matches);
    for (const auto *rec : matches)
      winners.emplace_back(sk, *rec);
  }
  EmitResult(winners, win->max_revenue, sink);
}

TpchQ15State TpchQ15Module::BuildState(
    core::AuxEngineServices &services) const {
  TpchQ15State state;
  if (!initial_scan)
    throw std::logic_error("TPC-H Q15 requires a typed initial scan source");

  state.directory = core::PartitionedBuild<TpchQ15SupplierDirectory>(
      services, [] { return TpchQ15SupplierDirectory{}; },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanSuppliers(services, parts,
                                    [&](std::size_t p, const TpchQ15SupplierInput &input) {
                                      sink(p, core::SingleRowBatch<TpchQ15SupplierInput>{input});
                                    });
      },
      [](TpchQ15SupplierDirectory &dir, const TpchQ15SupplierInput &input) {
        dir.Add(input.suppkey,
                TpchQ15SupplierRecord{input.name, input.address, input.phone});
      },
      [](TpchQ15SupplierDirectory &into, TpchQ15SupplierDirectory &&from) {
        into.MergeFrom(std::move(from));
      });

  AccumulateWindows(services, *initial_scan, state);
  std::vector<TpchQ15WindowState *> windows;
  for (auto &entry : state.windows)
    windows.push_back(&entry.second);
  core::ParallelSlices(services, windows.size(), kWindowGrain,
                       [&](std::size_t, std::size_t begin, std::size_t end) {
                         for (std::size_t i = begin; i < end; ++i)
                           windows[i]->Rescan();
                       });
  return state;
}

void TpchQ15Module::EmitResult(
    const std::vector<std::pair<int64_t, TpchQ15SupplierRecord>> &rows,
    int64_t revenue_raw, core::AuxRowSink &sink) const {
  constexpr int32_t kScale = 4;
  sink.Begin({{{"s_suppkey", core::AuxType::Int64, true},
               {"s_name", core::AuxType::String, true},
               {"s_address", core::AuxType::String, true},
               {"s_phone", core::AuxType::String, true},
               {"total_revenue", core::AuxType::ScaledDecimal, true}}});
  for (const auto &[suppkey, record] : rows) {
    core::ScaledDecimal revenue{revenue_raw, kScale};
    sink.WriteRow({{core::AuxValue::Of(core::AuxType::Int64, suppkey),
                    core::AuxValue::Of(core::AuxType::String, record.name),
                    core::AuxValue::Of(core::AuxType::String, record.address),
                    core::AuxValue::Of(core::AuxType::String, record.phone),
                    core::AuxValue::Of(core::AuxType::ScaledDecimal,
                                       revenue)}});
  }
  sink.End();
}

bool TpchQ15Module::LoadMonth(core::AuxEngineServices &services,
                               int32_t month_key,
                               TpchQ15WindowState &win) const {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {static_cast<core::PartitionId>(month_key)});
  for (const auto &image : snapshot.partitions) {
    if (static_cast<int32_t>(image.partition) != month_key)
      continue;
    const auto *typed = AsQ15Image(image, TpchQ15ImageKind::MonthWindow);
    if (!typed || typed->month_key != month_key)
      return false;
    const auto it = typed->state->windows.find(month_key);
    if (it != typed->state->windows.end())
      win = it->second; // shares the array chunks; they are cloned on write
    return true;
  }
  // No image yet: a month without data has no window partition (Build writes
  // only non-empty windows), so it starts empty.
  win = TpchQ15WindowState{};
  return true;
}

bool TpchQ15Module::LoadDirectory(core::AuxEngineServices &services,
                                   TpchQ15SupplierDirectory &dir) const {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {TpchQ15Layout::kSupplierDirectory});
  for (const auto &image : snapshot.partitions) {
    if (image.partition != TpchQ15Layout::kSupplierDirectory)
      continue;
    const auto *typed = AsQ15Image(image, TpchQ15ImageKind::SupplierDirectory);
    if (!typed)
      return false;
    dir = typed->state->directory; // shares chunks (COW)
    return true;
  }
  return false;
}

std::vector<core::CalibrationWrite> TpchQ15Module::CalibrationWrites() const {
  return TpchQ15CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
