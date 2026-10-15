#include "tpch/q22/duckdb/aux/module.hpp"
#include "tpch/q22/calibration_writes.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

using core::AuxValue;
using core::AuxType;
using core::ScaledDecimal;

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

bool ReadCustomer(const core::AuxRow &row, int64_t &custkey,
                  std::string &phone, int64_t &acctbal_raw,
                  int32_t &acctbal_scale) {
  if (!IntCell(row, 0, custkey))
    return false;
  if (!StringCell(row, 1, phone))
    return false;
  if (!RawDecimalCell(row, 2, acctbal_raw, acctbal_scale))
    return false;
  return true;
}

bool ReadOrder(const core::AuxRow &row, int64_t &custkey) {
  return IntCell(row, 0, custkey);
}

std::string ExtractPrefix(const std::string &phone) {
  if (phone.size() < 2)
    return "";
  return phone.substr(0, 2);
}

using Prefix = TpchQ22State::Prefix;

// Packs a two-character prefix; anything else (including "") maps to 0, which
// never matches a stored prefix.
Prefix PackPrefix(const std::string &prefix) {
  if (prefix.size() != 2)
    return 0;
  return static_cast<Prefix>((static_cast<unsigned char>(prefix[0]) << 8) |
                             static_cast<unsigned char>(prefix[1]));
}

std::shared_ptr<const TpchQ22StateImage>
MakeStateImage(const std::shared_ptr<const TpchQ22State> &state) {
  auto image = std::make_shared<TpchQ22StateImage>();
  image->kind = TpchQ22ImageKind::FullState;
  image->state = state;
  return image;
}

const TpchQ22StateImage *AsQ22Image(const core::StateImage &image) {
  const auto *payload = image.As<TpchQ22StateImage>();
  return payload && payload->kind == TpchQ22ImageKind::FullState &&
                 payload->state
             ? payload
             : nullptr;
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

} // namespace

// ── TxnBuffer methods ───────────────────────────────────────────────────────
bool TpchQ22TxnBuffer::HasPendingChanges() const {
  return pending || !customer_deltas.empty() || !order_deltas.empty();
}

void TpchQ22TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  customer_deltas.clear();
  order_deltas.clear();
}

bool TpchQ22TxnBuffer::RequiresRebuild() const { return requires_rebuild; }

void TpchQ22TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  customer_deltas.clear();
  order_deltas.clear();
}

// ── Module identity ─────────────────────────────────────────────────────────
core::AcceleratorId TpchQ22Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ22Module::Dependencies() const {
  return {{config.customer_table, {0, 1, 2}, {0, 4, 5}},
          {config.orders_table, {0}, {1}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ22Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ22TxnBuffer>();
}

// ── Build ───────────────────────────────────────────────────────────────────
void TpchQ22Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  const auto typed_state =
      std::make_shared<const TpchQ22State>(BuildState(services));
  builder.Write(core::StateImage::Make(
      TpchQ22Layout::kAccelerator, TpchQ22Layout::kFull, 0,
      MakeStateImage(typed_state)));
  builder.Finish();
}

// ── Mutation interpretation ─────────────────────────────────────────────────
core::InvalidationPlan
TpchQ22Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.customer_table || mutation.table == config.orders_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  return {};
}

// ── Mutation accumulation ───────────────────────────────────────────────────
void TpchQ22Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ22TxnBuffer &>(base_buffer);

  if (mutation.table == config.customer_table) {
    auto process_customer = [&](const core::AuxRow &row, int8_t sign) {
      int64_t custkey = 0;
      std::string phone;
      int64_t acctbal_raw = 0;
      int32_t acctbal_scale = 0;
      if (!ReadCustomer(row, custkey, phone, acctbal_raw,
                        acctbal_scale)) {
        buffer.MarkRebuild();
        return;
      }
      TpchQ22TxnBuffer::CustomerDelta delta;
      delta.custkey = custkey;
      delta.old_prefix = ExtractPrefix(phone);
      delta.new_prefix = delta.old_prefix;
      delta.old_balance = acctbal_raw;
      delta.new_balance = acctbal_raw;
      delta.sign = sign;
      buffer.customer_deltas.push_back(delta);
    };

    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows)
        process_customer(row, 1);
      break;
    case core::MutationKind::Delete:
      for (const auto &row : mutation.old_rows)
        process_customer(row, -1);
      break;
    case core::MutationKind::Update: {
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        int64_t old_custkey = 0, new_custkey = 0;
        std::string old_phone, new_phone;
        int64_t old_bal = 0, new_bal = 0;
        int32_t old_scale = 0, new_scale = 0;
        if (!ReadCustomer(mutation.old_rows[i], old_custkey, old_phone,
                          old_bal, old_scale) ||
            !ReadCustomer(mutation.new_rows[i], new_custkey, new_phone,
                          new_bal, new_scale)) {
          buffer.MarkRebuild();
          return;
        }
        TpchQ22TxnBuffer::CustomerDelta delta;
        delta.custkey = old_custkey;
        delta.old_prefix = ExtractPrefix(old_phone);
        delta.new_prefix = ExtractPrefix(new_phone);
        delta.old_balance = old_bal;
        delta.new_balance = new_bal;
        delta.sign = 0;
        buffer.customer_deltas.push_back(delta);
      }
      break;
    }
    }
    return;
  }

  if (mutation.table == config.orders_table) {
    auto process_order = [&](const core::AuxRow &row, int8_t sign) {
      int64_t custkey = 0;
      if (!ReadOrder(row, custkey)) {
        buffer.MarkRebuild();
        return;
      }
      TpchQ22TxnBuffer::OrderDelta delta;
      delta.custkey = custkey;
      delta.sign = sign;
      buffer.order_deltas.push_back(delta);
    };

    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows)
        process_order(row, 1);
      break;
    case core::MutationKind::Delete:
      for (const auto &row : mutation.old_rows)
        process_order(row, -1);
      break;
    case core::MutationKind::Update:
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        int64_t old_ck = 0, new_ck = 0;
        if (!ReadOrder(mutation.old_rows[i], old_ck) ||
            !ReadOrder(mutation.new_rows[i], new_ck)) {
          buffer.MarkRebuild();
          return;
        }
        if (old_ck != new_ck) {
          // Order custkey changed: old goes -1, new goes +1
          buffer.order_deltas.push_back({old_ck, -1});
          buffer.order_deltas.push_back({new_ck, 1});
        }
        // If custkey unchanged, no effect on Q22 order existence
      }
      break;
    }
    return;
  }
}

// ── StageMaintenance ────────────────────────────────────────────────────────
core::StagedMaintenanceOutcome
TpchQ22Module::StageMaintenance(core::AuxEngineServices &services,
                                const core::InvalidationPlan &,
                                core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ22TxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;

  const auto rebuild_all = [&]() {
    outcome = core::StagedMaintenanceOutcome{};
    outcome.action = core::MaintenanceAction::RebuildAll;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  };

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

  const bool has_customer_changes = !buffer.customer_deltas.empty();
  const bool has_order_changes = !buffer.order_deltas.empty();
  if (!has_customer_changes && !has_order_changes)
    return outcome;

  // Count the total rows affected and the number of affected prefixes
  std::set<std::string> touched_prefixes;
  size_t total_rows_touched = buffer.customer_deltas.size() + buffer.order_deltas.size();

  for (const auto &delta : buffer.customer_deltas) {
    if (!delta.old_prefix.empty())
      touched_prefixes.insert(delta.old_prefix);
    if (!delta.new_prefix.empty())
      touched_prefixes.insert(delta.new_prefix);
  }

  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(total_rows_touched),
       static_cast<uint64_t>(touched_prefixes.size()), true, false, true,
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

  // Load existing state
  const auto visibility = services.CurrentVisibility();
  const auto snapshot = services.StateStore().ReadPartitions(
      visibility, {TpchQ22Layout::kFull});

  core::Version existing_version = 0;
  std::shared_ptr<const TpchQ22State> parent;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ22Layout::kFull) {
      const auto *typed = AsQ22Image(image);
      if (!typed || !typed->state)
        return rebuild_all();
      parent = typed->state;
      existing_version = image.version;
      break;
    }
  }
  if (!parent)
    return rebuild_all();

  // Staged copy: shares every page, aggregate block, sparse shard and orphan
  // shard with the parent; only what a delta touches is cloned (once), so
  // published versions stay immutable and an aborted commit leaves no trace.
  auto new_state = std::make_shared<TpchQ22State>(*parent);
  using Prefix = TpchQ22State::Prefix;
  using Record = TpchQ22State::CustomerRecord;

  std::unordered_set<std::size_t> owned_pages;
  std::unordered_map<Prefix, std::shared_ptr<core::SortedCountSum>> touched_aggs;

  const auto mutable_customer = [&](int64_t custkey) -> Record & {
    if (custkey >= 0 && custkey < TpchQ22State::kDenseLimit) {
      const std::size_t page = static_cast<std::size_t>(custkey) /
                               TpchQ22State::kPageSize;
      if (new_state->pages.size() <= page)
        new_state->pages.resize(page + 1);
      if (owned_pages.insert(page).second)
        new_state->pages[page] =
            new_state->pages[page]
                ? std::make_shared<TpchQ22State::Page>(*new_state->pages[page])
                : std::make_shared<TpchQ22State::Page>();
      return new_state->pages[page]->recs[static_cast<std::size_t>(custkey) %
                                          TpchQ22State::kPageSize];
    }
    return new_state->sparse[custkey];
  };

  const auto aggregate = [&](Prefix prefix, bool create) -> core::SortedCountSum * {
    const auto it = touched_aggs.find(prefix);
    if (it != touched_aggs.end())
      return it->second.get();
    const auto base_it = new_state->prefix_aggregates.find(prefix);
    if (base_it == new_state->prefix_aggregates.end() && !create)
      return nullptr;
    // Copying a SortedCountSum shares its blocks; Add clones the touched one.
    auto clone = base_it == new_state->prefix_aggregates.end()
                     ? std::make_shared<core::SortedCountSum>()
                     : std::make_shared<core::SortedCountSum>(*base_it->second);
    return touched_aggs.emplace(prefix, std::move(clone)).first->second.get();
  };

  const auto positive_add = [&](Prefix prefix, int64_t balance) {
    auto &p = new_state->positive[prefix];
    p.sum += balance;
    p.count++;
  };
  const auto positive_remove = [&](Prefix prefix, int64_t balance) {
    auto &p = new_state->positive[prefix];
    p.sum -= balance;
    p.count--;
    if (p.count <= 0)
      new_state->positive.erase(prefix);
  };
  // Adds (sign > 0) or removes (sign < 0) a customer's contributions.
  const auto contribute = [&](const Record &rec, int sign) {
    if (rec.balance_raw <= 0 || rec.prefix == 0)
      return;
    if (sign > 0)
      positive_add(rec.prefix, rec.balance_raw);
    else
      positive_remove(rec.prefix, rec.balance_raw);
    if (rec.order_count == 0) {
      if (sign > 0)
        aggregate(rec.prefix, true)->Add(rec.balance_raw, 1);
      else if (auto *agg = aggregate(rec.prefix, false))
        agg->Add(rec.balance_raw, -1);
    }
  };

  // Apply customer deltas
  for (const auto &delta : buffer.customer_deltas) {
    const Prefix new_prefix = PackPrefix(delta.new_prefix);
    if (delta.sign == 1) {
      Record rec;
      rec.present = true;
      rec.prefix = new_prefix;
      rec.balance_raw = delta.new_balance;
      rec.order_count = 0;
      if (const int64_t *orphan = new_state->orphan_orders.Find(delta.custkey)) {
        rec.order_count = *orphan;
        new_state->orphan_orders.Erase(delta.custkey);
      }
      mutable_customer(delta.custkey) = rec;
      contribute(rec, +1);
    } else if (delta.sign == -1) {
      const Record *found = new_state->FindCustomer(delta.custkey);
      if (!found)
        return rebuild_all();
      const Record rec = *found;
      contribute(rec, -1);
      mutable_customer(delta.custkey) = Record{};
      if (rec.order_count != 0)
        new_state->orphan_orders.Upsert(delta.custkey, rec.order_count);
    } else {
      const Record *found = new_state->FindCustomer(delta.custkey);
      if (!found)
        return rebuild_all();
      const Record old_rec = *found;
      Record updated = old_rec;
      updated.prefix = new_prefix;
      updated.balance_raw = delta.new_balance;
      if (updated.prefix == old_rec.prefix &&
          updated.balance_raw == old_rec.balance_raw)
        continue;
      contribute(old_rec, -1);
      mutable_customer(delta.custkey) = updated;
      contribute(updated, +1);
    }
  }

  // Apply order deltas
  for (const auto &delta : buffer.order_deltas) {
    const Record *found = new_state->FindCustomer(delta.custkey);
    if (found) {
      Record rec = *found;
      const int64_t old_count = rec.order_count;
      rec.order_count = old_count + delta.sign;
      mutable_customer(delta.custkey).order_count = rec.order_count;
      // Only crossing the zero-order boundary moves the distribution.
      const bool was_zero = old_count == 0;
      const bool is_zero = rec.order_count == 0;
      if (was_zero != is_zero && rec.balance_raw > 0 && rec.prefix != 0) {
        if (is_zero)
          aggregate(rec.prefix, true)->Add(rec.balance_raw, 1);
        else if (auto *agg = aggregate(rec.prefix, false))
          agg->Add(rec.balance_raw, -1);
      }
    } else {
      // Customer not found — update orphan orders
      const int64_t *orphan = new_state->orphan_orders.Find(delta.custkey);
      const int64_t new_count = (orphan ? *orphan : 0) + delta.sign;
      if (new_count <= 0)
        new_state->orphan_orders.Erase(delta.custkey);
      else
        new_state->orphan_orders.Upsert(delta.custkey, new_count);
    }
  }

  // Publish the touched aggregates as immutable shared pieces.
  for (auto &[prefix, agg] : touched_aggs)
    new_state->prefix_aggregates[prefix] =
        std::static_pointer_cast<const core::SortedCountSum>(std::move(agg));

  outcome.action = core::MaintenanceAction::ApplyDelta;
  const auto typed_state =
      std::static_pointer_cast<const TpchQ22State>(std::move(new_state));
  outcome.updates.push_back(core::StateImage::Make(
      TpchQ22Layout::kAccelerator, TpchQ22Layout::kFull, existing_version,
      MakeStateImage(typed_state)));
  return outcome;
}

// ── CheckValidity ───────────────────────────────────────────────────────────
bool TpchQ22Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

// ── Execute ─────────────────────────────────────────────────────────────────
void TpchQ22Module::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  if (!services.StateStore()
           .CheckValidity({Id(), {}}, services.CurrentVisibility())
           .IsValid()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  std::shared_ptr<const TpchQ22State> state;
  if (!LoadState(services, state)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  // Extract country codes from arguments
  std::vector<std::string> country_codes;
  for (const auto &val : arguments.values) {
    if (!val.is_null && std::holds_alternative<std::string>(val.payload)) {
      country_codes.push_back(std::get<std::string>(val.payload));
    }
  }
  if (country_codes.empty()) {
    // Use fallback if no valid codes
    ExecuteFallback(services, arguments, sink);
    return;
  }

  EmitResult(*state, country_codes, sink);
}

// ── ExecuteFallback ─────────────────────────────────────────────────────────
void TpchQ22Module::ExecuteFallback(core::AuxEngineServices &services,
                                    const core::QueryArguments &arguments,
                                    core::AuxRowSink &sink) {
  // Fallback: recompute from base tables using BuildState + EmitResult
  const auto state = BuildState(services);

  std::vector<std::string> country_codes;
  for (const auto &val : arguments.values) {
    if (!val.is_null && std::holds_alternative<std::string>(val.payload)) {
      country_codes.push_back(std::get<std::string>(val.payload));
    }
  }

  EmitResult(state, country_codes, sink);
}

// ── BuildState ──────────────────────────────────────────────────────────────
TpchQ22State
TpchQ22Module::BuildState(core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error(
        "TPC-H Q22 aux requires a typed initial scan source");

  TpchQ22State state;

  // Orders per customer (GROUP BY o_custkey in SQL when supported).
  std::unordered_map<int64_t, int64_t> order_counts;
  const bool counted = initial_scan->ScanOrderCounts(
      [&](int64_t custkey, int64_t count) {
        if (custkey > 0)
          order_counts[custkey] += count;
      });
  if (!counted) {
    initial_scan->ScanOrders([&](const TpchQ22OrderInput &input) {
      if (input.custkey > 0)
        order_counts[input.custkey]++;
    });
  }

  using Prefix = TpchQ22State::Prefix;
  std::unordered_map<Prefix, std::map<int64_t, int64_t>> distributions;

  const auto record_of = [&](int64_t custkey) -> TpchQ22State::CustomerRecord & {
    if (custkey < TpchQ22State::kDenseLimit) {
      const std::size_t page = static_cast<std::size_t>(custkey) /
                               TpchQ22State::kPageSize;
      if (state.pages.size() <= page)
        state.pages.resize(page + 1);
      if (!state.pages[page])
        state.pages[page] = std::make_shared<TpchQ22State::Page>();
      return state.pages[page]->recs[static_cast<std::size_t>(custkey) %
                                     TpchQ22State::kPageSize];
    }
    return state.sparse[custkey];
  };

  initial_scan->ScanCustomers([&](const TpchQ22CustomerInput &input) {
    if (input.custkey <= 0)
      return;
    TpchQ22State::CustomerRecord &rec = record_of(input.custkey);
    rec.present = true;
    rec.prefix = PackPrefix(ExtractPrefix(input.phone));
    rec.balance_raw = input.acctbal_raw;
    const auto order_it = order_counts.find(input.custkey);
    rec.order_count = order_it != order_counts.end() ? order_it->second : 0;
    if (rec.balance_raw > 0 && rec.prefix != 0) {
      auto &p = state.positive[rec.prefix];
      p.sum += rec.balance_raw;
      p.count++;
      if (rec.order_count == 0)
        distributions[rec.prefix][rec.balance_raw]++;
    }
  });

  // Orphan orders (custkeys in orders but not in customers).
  for (const auto &[custkey, count] : order_counts) {
    if (!state.FindCustomer(custkey))
      state.orphan_orders.Upsert(custkey, count);
  }

  // Per-prefix count/sum structures over the zero-order balances.
  std::vector<decltype(distributions)::const_iterator> groups;
  for (auto it = distributions.begin(); it != distributions.end(); ++it)
    groups.push_back(it);
  std::vector<std::shared_ptr<core::SortedCountSum>> aggs(groups.size());
  services.ParallelFor(groups.size(), [&](std::size_t i) {
    aggs[i] = std::make_shared<core::SortedCountSum>();
    for (const auto &[balance, count] : groups[i]->second)
      aggs[i]->Add(balance, count);
  });
  for (std::size_t i = 0; i < groups.size(); ++i)
    state.prefix_aggregates[groups[i]->first] = std::move(aggs[i]);
  return state;
}

// ── LoadState ───────────────────────────────────────────────────────────────
bool TpchQ22Module::LoadState(
    core::AuxEngineServices &services,
    std::shared_ptr<const TpchQ22State> &state) const {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {TpchQ22Layout::kFull});
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ22Layout::kFull) {
      const auto *typed = AsQ22Image(image);
      if (!typed || !typed->state)
        return false;
      state = typed->state;
      return true;
    }
  }
  return false;
}

// ── EmitResult ──────────────────────────────────────────────────────────────
void TpchQ22Module::EmitResult(
    const TpchQ22State &state,
    const std::vector<std::string> &country_codes,
    core::AuxRowSink &sink) const {
  // Deduplicate codes (keep them ordered: output is sorted by cntrycode).
  std::vector<const std::string *> unique_codes;
  unique_codes.reserve(country_codes.size());
  for (const auto &code : country_codes)
    unique_codes.push_back(&code);
  std::sort(unique_codes.begin(), unique_codes.end(),
            [](const std::string *a, const std::string *b) { return *a < *b; });
  unique_codes.erase(
      std::unique(unique_codes.begin(), unique_codes.end(),
                  [](const std::string *a, const std::string *b) {
                    return *a == *b;
                  }),
      unique_codes.end());

  // Overall positive sum/count for the selected codes.
  int64_t global_positive_sum = 0;
  int64_t global_positive_count = 0;
  for (const auto *code : unique_codes) {
    const Prefix prefix = PackPrefix(*code);
    if (prefix == 0)
      continue;
    const auto it = state.positive.find(prefix);
    if (it == state.positive.end())
      continue;
    global_positive_sum += it->second.sum;
    global_positive_count += it->second.count;
  }

  // No positive customers → no rows (AVG is NULL in SQL)
  if (global_positive_count <= 0) {
    sink.Begin(core::OutputSchema{});
    sink.End();
    return;
  }

  // acctbal > AVG  <=>  acctbal_raw > floor(sum / count) for integer balances.
  const int64_t threshold_raw = global_positive_sum / global_positive_count;

  std::vector<TpchQ22OutputRow> output_rows;
  for (const auto *code : unique_codes) {
    const Prefix prefix = PackPrefix(*code);
    if (prefix == 0)
      continue;
    const auto agg_it = state.prefix_aggregates.find(prefix);
    if (agg_it == state.prefix_aggregates.end())
      continue;
    // Published state is shared by concurrent readers: read-only query.
    const auto totals = agg_it->second->SuffixAbove(threshold_raw);
    const int64_t count = totals.count;
    const int64_t sum = static_cast<int64_t>(totals.sum);
    if (count > 0) {
      TpchQ22OutputRow row;
      row.cntrycode = *code;
      row.numcust = count;
      row.totacctbal_raw = sum;
      output_rows.push_back(std::move(row));
    }
  }

  sink.Begin(
      {{{"cntrycode", core::AuxType::String, false},
        {"numcust", core::AuxType::Int64, false},
        {"totacctbal", core::AuxType::ScaledDecimal, false}}});

  for (const auto &row : output_rows) {
    core::AuxRow out;
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::String, row.cntrycode));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::Int64, row.numcust));
    out.values.push_back(core::AuxValue::Of(
        core::AuxType::ScaledDecimal,
        core::ScaledDecimal{row.totacctbal_raw, 2}));
    sink.WriteRow(out);
  }

  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ22Module::CalibrationWrites() const {
  return TpchQ22CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
