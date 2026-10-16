#include "tpch/q21/postgres/aux/module.hpp"
#include "tpch/q21/calibration_writes.hpp"
#include "substrate/aux/core/parallel_slices.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

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
  return false;
}

bool ReadSupplier(const core::AuxRow &row, TpchQ21SupplierInput &out) {
  return IntCell(row, 0, out.suppkey) && StringCell(row, 1, out.name) &&
         IntCell(row, 2, out.nationkey);
}

bool ReadNation(const core::AuxRow &row, TpchQ21NationInput &out) {
  return IntCell(row, 0, out.nationkey) && StringCell(row, 1, out.name);
}

std::shared_ptr<const TpchQ21StateImage>
MakeStateImage(const std::shared_ptr<const TpchQ21State> &state) {
  auto image = std::make_shared<TpchQ21StateImage>();
  image->state = state;
  return image;
}

const TpchQ21StateImage *AsQ21Image(const core::StateImage &image) {
  const auto *payload = image.As<TpchQ21StateImage>();
  return payload && payload->state ? payload : nullptr;
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

// ── Order contribution ──────────────────────────────────────────────────────
// Reduces the two correlated subqueries of one fulfilled order. Candidate
// l1 rows are the late lineitems; EXISTS needs a second supplier on the order
// and NOT EXISTS forbids a late lineitem of any other supplier, so the order
// counts only when exactly one supplier has late lineitems (and another
// supplier has any lineitem): that supplier gains its number of late rows.
bool ContributionOf(std::vector<std::pair<int64_t, bool>> &lines,
                    TpchQ21Contribution &out) {
  std::sort(lines.begin(), lines.end());
  std::size_t suppliers = 0, late_suppliers = 0;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const bool first_of_supplier = i == 0 || lines[i].first != lines[i - 1].first;
    if (first_of_supplier)
      ++suppliers;
    if (!lines[i].second)
      continue;
    if (first_of_supplier || !lines[i - 1].second) {
      ++late_suppliers; // first late row of this supplier
      out.suppkey = lines[i].first;
      out.count = 0;
    }
    ++out.count;
  }
  return suppliers >= 2 && late_suppliers == 1;
}

// Collects the consecutive rows of one order and reports its contribution
// when the next order starts (or at Flush).
class OrderGrouper {
public:
  using Emit = std::function<void(int64_t, bool, const TpchQ21Contribution &)>;
  explicit OrderGrouper(Emit emit) : emit(std::move(emit)) {}

  void Add(const TpchQ21LineInput &line) {
    if (open && line.orderkey != orderkey)
      Flush();
    orderkey = line.orderkey;
    open = true;
    lines.emplace_back(line.suppkey, line.late);
  }
  void Flush() {
    if (!open)
      return;
    TpchQ21Contribution contribution;
    const bool has = ContributionOf(lines, contribution);
    emit(orderkey, has, contribution);
    lines.clear();
    open = false;
  }

private:
  Emit emit;
  int64_t orderkey = 0;
  bool open = false;
  std::vector<std::pair<int64_t, bool>> lines;
};

// ORDER BY numwait DESC, s_name (bytewise, the C collation the runtime
// requires): true when (numwait, name) comes first.
bool RanksBefore(int64_t lhs_numwait, std::string_view lhs_name,
                 int64_t rhs_numwait, std::string_view rhs_name) {
  if (lhs_numwait != rhs_numwait)
    return lhs_numwait > rhs_numwait;
  return lhs_name < rhs_name;
}

// The first kTpchQ21Limit groups in answer order, from (name, numwait) groups
// with distinct names: a bounded heap whose top is the worst group kept.
template <typename Name>
std::vector<TpchQ21OutputRow>
TopGroups(const std::vector<std::pair<Name, int64_t>> &groups) {
  using Group = std::pair<Name, int64_t>;
  const auto before = [](const Group &lhs, const Group &rhs) {
    return RanksBefore(lhs.second, std::string_view(*lhs.first), rhs.second,
                       std::string_view(*rhs.first));
  };
  std::vector<Group> heap;
  heap.reserve(std::min(groups.size(), kTpchQ21Limit));
  for (const auto &group : groups) {
    if (heap.size() < kTpchQ21Limit) {
      heap.push_back(group);
      std::push_heap(heap.begin(), heap.end(), before);
    } else if (before(group, heap.front())) {
      std::pop_heap(heap.begin(), heap.end(), before);
      heap.back() = group;
      std::push_heap(heap.begin(), heap.end(), before);
    }
  }
  std::sort_heap(heap.begin(), heap.end(), before);
  std::vector<TpchQ21OutputRow> rows;
  rows.reserve(heap.size());
  for (const auto &group : heap)
    rows.push_back({std::string(*group.first), group.second});
  return rows;
}

// Q21 groups by s_name, not s_suppkey: suppliers that share a name form one
// output group whose numwait is the sum of their counts. Then ORDER BY
// numwait DESC, s_name LIMIT 100.
std::vector<TpchQ21OutputRow>
MergeByNameAndTop(const std::vector<std::pair<std::string_view, int64_t>> &rows) {
  std::unordered_map<std::string_view, int64_t> by_name;
  by_name.reserve(rows.size());
  for (const auto &[name, numwait] : rows)
    by_name[name] += numwait;
  std::vector<std::pair<const std::string_view *, int64_t>> groups;
  groups.reserve(by_name.size());
  for (const auto &[name, numwait] : by_name)
    if (numwait > 0)
      groups.emplace_back(&name, numwait);
  return TopGroups(groups);
}

bool NationParameter(const core::QueryArguments &arguments, std::string &name) {
  if (arguments.values.empty() || arguments.values[0].is_null ||
      !std::holds_alternative<std::string>(arguments.values[0].payload))
    return false; // n_name = NULL keeps nothing
  name = std::get<std::string>(arguments.values[0].payload);
  return true;
}

void EmitEmpty(core::AuxRowSink &sink) {
  sink.Begin(core::OutputSchema{});
  sink.End();
}

} // namespace

// ── State ───────────────────────────────────────────────────────────────────
void TpchQ21State::SetContribution(int64_t orderkey, bool has,
                                   const TpchQ21Contribution &contribution) {
  const TpchQ21Contribution *found = contributions.Find(orderkey);
  const bool had = found != nullptr;
  const TpchQ21Contribution old = had ? *found : TpchQ21Contribution{};
  if (had && has && old.suppkey == contribution.suppkey &&
      old.count == contribution.count)
    return;
  if (had) {
    AdjustNumwait(old.suppkey, -old.count);
    if (!has)
      contributions.Erase(orderkey);
  }
  if (has) {
    contributions.Upsert(orderkey, contribution);
    AdjustNumwait(contribution.suppkey, contribution.count);
  }
}

void TpchQ21State::AdjustNumwait(int64_t suppkey, int64_t delta) {
  const int64_t *current = numwait.Find(suppkey);
  const int64_t value = (current ? *current : 0) + delta;
  if (value == 0)
    numwait.Erase(suppkey);
  else
    numwait.Upsert(suppkey, value);
  const TpchQ21Supplier *supplier = suppliers.Find(suppkey);
  if (!supplier)
    return;
  const int64_t nationkey = supplier->nationkey;
  if (value != 0) {
    nation_numwait[nationkey].Upsert(suppkey,
                                     TpchQ21NationEntry{value, supplier->name});
    return;
  }
  const auto it = nation_numwait.find(nationkey);
  if (it == nation_numwait.end())
    return;
  it->second.Erase(suppkey);
  if (it->second.size() == 0)
    nation_numwait.erase(it);
}

void TpchQ21State::PutSupplier(int64_t suppkey, const std::string &name,
                               int64_t nationkey) {
  RemoveSupplier(suppkey);
  auto shared_name = std::make_shared<const std::string>(name);
  CountNameHash(name, 1);
  if (const int64_t *current = numwait.Find(suppkey))
    nation_numwait[nationkey].Upsert(suppkey,
                                     TpchQ21NationEntry{*current, shared_name});
  suppliers.Upsert(suppkey, TpchQ21Supplier{std::move(shared_name), nationkey});
}

void TpchQ21State::CountNameHash(const std::string &name, int64_t delta) {
  const auto key = static_cast<int64_t>(std::hash<std::string>{}(name));
  const int64_t *found = name_hash_counts.Find(key);
  const int64_t before = found ? *found : 0;
  const int64_t after = before + delta;
  // Suppliers in a hash group of size n >= 2 all share it.
  const auto sharing = [](int64_t n) { return n >= 2 ? n : 0; };
  suppliers_sharing_name_hash += sharing(after) - sharing(before);
  if (after == 0)
    name_hash_counts.Erase(key);
  else
    name_hash_counts.Upsert(key, after);
}

bool TpchQ21State::RemoveSupplier(int64_t suppkey) {
  const TpchQ21Supplier *found = suppliers.Find(suppkey);
  if (!found)
    return false;
  const int64_t nationkey = found->nationkey;
  CountNameHash(*found->name, -1);
  const auto it = nation_numwait.find(nationkey);
  if (it != nation_numwait.end()) {
    it->second.Erase(suppkey);
    if (it->second.size() == 0)
      nation_numwait.erase(it);
  }
  suppliers.Erase(suppkey);
  return true;
}

void TpchQ21State::PutNation(int64_t nationkey, const std::string &name) {
  nation_keys[name].push_back(nationkey);
}

bool TpchQ21State::RemoveNation(int64_t nationkey, const std::string &name) {
  const auto it = nation_keys.find(name);
  if (it == nation_keys.end())
    return false;
  auto &keys = it->second;
  const auto pos = std::find(keys.begin(), keys.end(), nationkey);
  if (pos == keys.end())
    return false;
  keys.erase(pos);
  if (keys.empty())
    nation_keys.erase(it);
  return true;
}

// Read cost: one pass over the nation's index (suppliers with waiting orders)
// keeping the best kTpchQ21Limit in a bounded heap; no supplier lookups and
// no string copies but the emitted rows. With shared names (rare), a hash
// merge by name first.
std::vector<TpchQ21OutputRow>
TpchQ21State::TopRows(const std::string &nation) const {
  const auto keys = nation_keys.find(nation);
  if (keys == nation_keys.end())
    return {};
  if (suppliers_sharing_name_hash == 0) {
    std::vector<std::pair<const std::string *, int64_t>> groups;
    for (const int64_t nationkey : keys->second) {
      const auto index = nation_numwait.find(nationkey);
      if (index == nation_numwait.end())
        continue;
      groups.reserve(groups.size() + index->second.size());
      index->second.ForEach([&](int64_t, const TpchQ21NationEntry &entry) {
        if (entry.numwait > 0)
          groups.emplace_back(entry.name.get(), entry.numwait);
      });
    }
    return TopGroups(groups);
  }
  std::vector<std::pair<std::string_view, int64_t>> rows;
  for (const int64_t nationkey : keys->second) {
    const auto index = nation_numwait.find(nationkey);
    if (index == nation_numwait.end())
      continue;
    index->second.ForEach([&](int64_t, const TpchQ21NationEntry &entry) {
      rows.emplace_back(*entry.name, entry.numwait);
    });
  }
  return MergeByNameAndTop(rows);
}

TpchQ21AnswerCache::Rows
TpchQ21AnswerCache::Get(const std::string &nation) const {
  std::lock_guard<std::mutex> lock(mu);
  const auto it = by_nation.find(nation);
  return it == by_nation.end() ? nullptr : it->second;
}

void TpchQ21AnswerCache::Put(const std::string &nation, Rows rows) const {
  std::lock_guard<std::mutex> lock(mu);
  if (by_nation.size() >= kMaxEntries)
    by_nation.clear(); // bounded: arbitrary parameter strings
  by_nation[nation] = std::move(rows);
}

// ── TxnBuffer ───────────────────────────────────────────────────────────────
bool TpchQ21TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ21TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  order_results.clear();
  supplier_deltas.clear();
  nation_deltas.clear();
}
bool TpchQ21TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
void TpchQ21TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  order_results.clear();
  supplier_deltas.clear();
  nation_deltas.clear();
}
void TpchQ21TxnBuffer::AddOrderResult(TpchQ21OrderResult result) {
  pending = true;
  order_results.push_back(result);
}
void TpchQ21TxnBuffer::AddSupplierDelta(TpchQ21SupplierDelta delta) {
  pending = true;
  supplier_deltas.push_back(std::move(delta));
}
void TpchQ21TxnBuffer::AddNationDelta(TpchQ21NationDelta delta) {
  pending = true;
  nation_deltas.push_back(std::move(delta));
}
const std::vector<TpchQ21OrderResult> &TpchQ21TxnBuffer::OrderResults() const {
  return order_results;
}
const std::vector<TpchQ21SupplierDelta> &TpchQ21TxnBuffer::SupplierDeltas() const {
  return supplier_deltas;
}
const std::vector<TpchQ21NationDelta> &TpchQ21TxnBuffer::NationDeltas() const {
  return nation_deltas;
}
uint64_t TpchQ21TxnBuffer::Size() const {
  return order_results.size() + supplier_deltas.size() + nation_deltas.size();
}

// ── Module ──────────────────────────────────────────────────────────────────
core::AcceleratorId TpchQ21Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ21Module::Dependencies() const {
  return {{config.supplier_table, {0, 1, 2}, {0, 1, 3}},
          {config.orders_table, {0, 1}, {0, 2}},
          {config.lineitem_table, {0, 1, 2, 3}, {0, 2, 11, 12}},
          {config.nation_table, {0, 1}, {0, 1}}};
}

namespace {

// Buckets of the build's fulfilled-order lineitems (by order).
constexpr std::size_t kBuildBuckets = 64;

struct OrderContribution {
  int64_t orderkey;
  TpchQ21Contribution contribution;
};

// The contributions of one bucket's lineitems, order by order.
std::vector<OrderContribution> ContributionsOfBucket(
    std::vector<TpchQ21LineInput> &lines) {
  std::sort(lines.begin(), lines.end(),
            [](const TpchQ21LineInput &a, const TpchQ21LineInput &b) {
              return a.orderkey < b.orderkey;
            });
  std::vector<OrderContribution> found;
  OrderGrouper grouper([&](int64_t orderkey, bool has,
                           const TpchQ21Contribution &contribution) {
    if (has)
      found.push_back({orderkey, contribution});
  });
  for (const auto &line : lines)
    grouper.Add(line);
  grouper.Flush();
  return found;
}

// The contribution of every fulfilled order that has one. The scans only join
// (lineitems of fulfilled orders dealt out to buckets by order); the buckets
// are then reduced in parallel slices.
std::vector<std::vector<OrderContribution>>
FulfilledContributions(TpchQ21InitialScan &scan,
                       const core::AuxEngineServices &services) {
  core::FlatHashMap<uint8_t> fulfilled;
  scan.ScanFulfilledOrders([&](int64_t orderkey) { fulfilled.Upsert(orderkey, 1); });
  std::vector<std::vector<TpchQ21LineInput>> buckets(kBuildBuckets);
  scan.ScanLineitems([&](const TpchQ21LineInput &line) {
    if (fulfilled.Contains(line.orderkey))
      buckets[static_cast<uint64_t>(line.orderkey) % kBuildBuckets].push_back(line);
  });
  std::vector<std::vector<OrderContribution>> found(kBuildBuckets);
  core::ParallelSlices(services, kBuildBuckets, 1,
                       [&](std::size_t, std::size_t begin, std::size_t end) {
                         for (std::size_t i = begin; i < end; ++i) {
                           found[i] = ContributionsOfBucket(buckets[i]);
                           buckets[i] = {};
                         }
                       });
  return found;
}

} // namespace

std::unique_ptr<core::AuxTxnBuffer> TpchQ21Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ21TxnBuffer>();
}

void TpchQ21Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  const auto typed_state =
      std::make_shared<const TpchQ21State>(BuildState(services));
  builder.Write(core::StateImage::Make(
      TpchQ21Layout::kAccelerator, TpchQ21Layout::kState, 0,
      MakeStateImage(typed_state)));
  builder.Finish();
}

core::InvalidationPlan
TpchQ21Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table ||
      mutation.table == config.orders_table ||
      mutation.table == config.supplier_table ||
      mutation.table == config.nation_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  return {};
}

// Every write is applied incrementally:
//   supplier / nation rows  -> a row delta on the supplier and nation maps;
//   lineitem / orders rows  -> the touched orders are re-read from the
//     database as the statement left them (one keyed query) and their new
//     contribution recorded. An order that is not in the state contributes
//     nothing, so no earlier state of it is needed.
void TpchQ21Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ21TxnBuffer &>(base_buffer);
  const bool paired = mutation.kind == core::MutationKind::Update;
  if (paired && mutation.old_rows.size() != mutation.new_rows.size()) {
    buffer.MarkRebuild();
    return;
  }
  const bool has_old = mutation.kind != core::MutationKind::Insert;
  const bool has_new = mutation.kind != core::MutationKind::Delete;
  const std::size_t count =
      has_old ? mutation.old_rows.size() : mutation.new_rows.size();

  if (mutation.table == config.supplier_table) {
    for (std::size_t i = 0; i < count; ++i) {
      TpchQ21SupplierDelta delta;
      delta.has_old = has_old;
      delta.has_new = has_new;
      TpchQ21SupplierInput old_row;
      if ((has_old && !ReadSupplier(mutation.old_rows[i], old_row)) ||
          (has_new && !ReadSupplier(mutation.new_rows[i], delta.new_row))) {
        buffer.MarkRebuild();
        return;
      }
      delta.old_suppkey = old_row.suppkey;
      buffer.AddSupplierDelta(std::move(delta));
    }
    return;
  }

  if (mutation.table == config.nation_table) {
    for (std::size_t i = 0; i < count; ++i) {
      TpchQ21NationDelta delta;
      delta.has_old = has_old;
      delta.has_new = has_new;
      if ((has_old && !ReadNation(mutation.old_rows[i], delta.old_row)) ||
          (has_new && !ReadNation(mutation.new_rows[i], delta.new_row))) {
        buffer.MarkRebuild();
        return;
      }
      buffer.AddNationDelta(std::move(delta));
    }
    return;
  }

  if (mutation.table != config.lineitem_table &&
      mutation.table != config.orders_table)
    return;

  // Both tables carry the orderkey in column 0.
  std::vector<int64_t> orderkeys;
  for (std::size_t i = 0; i < count; ++i) {
    int64_t key = 0;
    if (has_old) {
      if (!IntCell(mutation.old_rows[i], 0, key)) {
        buffer.MarkRebuild();
        return;
      }
      orderkeys.push_back(key);
    }
    if (has_new) {
      if (!IntCell(mutation.new_rows[i], 0, key)) {
        buffer.MarkRebuild();
        return;
      }
      orderkeys.push_back(key);
    }
  }
  std::sort(orderkeys.begin(), orderkeys.end());
  orderkeys.erase(std::unique(orderkeys.begin(), orderkeys.end()),
                  orderkeys.end());
  if (orderkeys.empty())
    return;

  std::unordered_map<int64_t, TpchQ21OrderResult> results;
  OrderGrouper grouper([&](int64_t orderkey, bool has,
                           const TpchQ21Contribution &contribution) {
    results[orderkey] = TpchQ21OrderResult{orderkey, has, contribution};
  });
  if (!initial_scan ||
      !initial_scan->LookupFulfilledLineitems(
          orderkeys,
          [&](const TpchQ21LineInput &line) { grouper.Add(line); })) {
    buffer.MarkRebuild();
    return;
  }
  grouper.Flush();
  for (const int64_t orderkey : orderkeys) {
    const auto it = results.find(orderkey);
    buffer.AddOrderResult(it != results.end()
                              ? it->second
                              : TpchQ21OrderResult{orderkey, false, {}});
  }
}

core::StagedMaintenanceOutcome
TpchQ21Module::StageMaintenance(core::AuxEngineServices &services,
                                const core::InvalidationPlan &,
                                core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ21TxnBuffer &>(base_buffer);
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
  if (buffer.Size() == 0)
    return outcome;

  const auto decision = services.ChooseMaintenance(
      {buffer.Size(), 1, true, false, true, false, false,
       core::MaintenanceAction::NoOp, services.ExpectedQueriesBeforeNextWrite()});
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

  std::shared_ptr<const TpchQ21State> current;
  core::Version version = 0;
  if (!LoadState(services, current, &version)) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }
  // Copy-on-write copy: only the shards a delta touches are cloned.
  auto new_state = std::make_shared<TpchQ21State>(*current);

  for (const auto &delta : buffer.NationDeltas()) {
    if (delta.has_old &&
        !new_state->RemoveNation(delta.old_row.nationkey, delta.old_row.name)) {
      core::FinishRebuildAll(outcome); // state and tables disagree
      return outcome;
    }
    if (delta.has_new)
      new_state->PutNation(delta.new_row.nationkey, delta.new_row.name);
  }
  for (const auto &delta : buffer.SupplierDeltas()) {
    if (delta.has_old && !new_state->RemoveSupplier(delta.old_suppkey)) {
      core::FinishRebuildAll(outcome);
      return outcome;
    }
    if (delta.has_new)
      new_state->PutSupplier(delta.new_row.suppkey, delta.new_row.name,
                             delta.new_row.nationkey);
  }
  for (const auto &result : buffer.OrderResults())
    new_state->SetContribution(result.orderkey, result.has, result.contribution);

  outcome.action = core::MaintenanceAction::ApplyDelta;
  const auto typed_state =
      std::static_pointer_cast<const TpchQ21State>(std::move(new_state));
  outcome.updates.push_back(core::StateImage::Make(
      TpchQ21Layout::kAccelerator, TpchQ21Layout::kState, version,
      MakeStateImage(typed_state)));
  return outcome;
}

bool TpchQ21Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &arguments) const {
  if (arguments.values.empty())
    return false;
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

// Execute cost: the first read of a nation in a state version is one pass over
// the nation's suppliers with waiting orders (TopRows); later reads of that
// version reuse its answer. Either way at most kTpchQ21Limit rows are emitted.
void TpchQ21Module::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  // LocalWriteSafe: same-transaction relevant writes => fallback.
  if (services.StateStore().IsTransactionDirty()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  std::string nation_name;
  if (!NationParameter(arguments, nation_name)) {
    EmitEmpty(sink);
    return;
  }
  const auto visibility = services.CurrentVisibility();
  std::shared_ptr<const TpchQ21State> state;
  if (!services.StateStore().CheckValidity({Id(), {}}, visibility).IsValid() ||
      !LoadState(services, state)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  auto rows = state->answers.Get(nation_name);
  if (!rows) {
    rows = std::make_shared<const std::vector<TpchQ21OutputRow>>(
        state->TopRows(nation_name));
    state->answers.Put(nation_name, rows);
  }
  EmitRows(*rows, sink);
}

// No usable state: stream the fulfilled lineitems once, keeping only the
// suppliers of the requested nation(s).
void TpchQ21Module::ExecuteFallback(core::AuxEngineServices &services,
                                    const core::QueryArguments &arguments,
                                    core::AuxRowSink &sink) {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q21 aux requires a typed initial scan source");
  std::string nation_name;
  if (!NationParameter(arguments, nation_name)) {
    EmitEmpty(sink);
    return;
  }
  std::vector<int64_t> nationkeys;
  initial_scan->ScanNations([&](const TpchQ21NationInput &input) {
    if (input.name == nation_name)
      nationkeys.push_back(input.nationkey);
  });
  std::unordered_map<int64_t, std::string> suppliers; // suppkey -> s_name
  initial_scan->ScanSuppliers([&](const TpchQ21SupplierInput &input) {
    if (std::find(nationkeys.begin(), nationkeys.end(), input.nationkey) !=
        nationkeys.end())
      suppliers[input.suppkey] = input.name;
  });
  std::vector<std::pair<std::string_view, int64_t>> rows;
  if (!suppliers.empty()) {
    std::unordered_map<int64_t, int64_t> numwait;
    for (const auto &bucket : FulfilledContributions(*initial_scan, services))
      for (const auto &order : bucket)
        if (suppliers.count(order.contribution.suppkey))
          numwait[order.contribution.suppkey] += order.contribution.count;
    for (const auto &[suppkey, count] : numwait)
      rows.emplace_back(suppliers[suppkey], count);
  }
  EmitRows(MergeByNameAndTop(rows), sink);
}

TpchQ21State TpchQ21Module::BuildState(
    const core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q21 aux requires a typed initial scan source");
  TpchQ21State state;

  initial_scan->ScanNations([&](const TpchQ21NationInput &input) {
    state.PutNation(input.nationkey, input.name);
  });
  initial_scan->ScanSuppliers([&](const TpchQ21SupplierInput &input) {
    state.PutSupplier(input.suppkey, input.name, input.nationkey);
  });

  // Only the contributing orders are kept.
  for (const auto &bucket : FulfilledContributions(*initial_scan, services))
    for (const auto &order : bucket)
      state.SetContribution(order.orderkey, true, order.contribution);
  return state;
}

bool TpchQ21Module::LoadState(core::AuxEngineServices &services,
                              std::shared_ptr<const TpchQ21State> &state,
                              core::Version *version) const {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {TpchQ21Layout::kState});
  for (const auto &image : snapshot.partitions) {
    if (image.partition != TpchQ21Layout::kState)
      continue;
    const auto *typed = AsQ21Image(image);
    if (!typed)
      return false;
    state = typed->state;
    if (version)
      *version = image.version;
    return true;
  }
  return false;
}

void TpchQ21Module::EmitRows(const std::vector<TpchQ21OutputRow> &rows,
                             core::AuxRowSink &sink) const {
  sink.Begin(core::OutputSchema{});
  for (const auto &row : rows) {
    core::AuxRow out;
    out.values.reserve(2);
    out.values.push_back(core::AuxValue::Of(core::AuxType::String, row.name));
    out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, row.numwait));
    sink.WriteRow(out);
  }
  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ21Module::CalibrationWrites() const {
  return TpchQ21CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
