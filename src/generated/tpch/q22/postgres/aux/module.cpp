#include "tpch/q22/postgres/aux/module.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
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
  return {{config.customer_table, {0, 1, 2}},
          {config.orders_table, {0}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ22Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ22TxnBuffer>();
}

// ── Build ───────────────────────────────────────────────────────────────────
void TpchQ22Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  const auto state = BuildState(services);
  const auto typed_state = std::make_shared<const TpchQ22State>(state);
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
  // Order deltas don't touch prefixes directly; they affect customer records

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
  std::shared_ptr<TpchQ22State> new_state;
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ22Layout::kFull) {
      const auto *typed = AsQ22Image(image);
      if (!typed || !typed->state) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
      new_state = std::make_shared<TpchQ22State>();
      new_state->prefix_positive_sum = typed->state->prefix_positive_sum;
      new_state->prefix_positive_count = typed->state->prefix_positive_count;
      new_state->prefix_aggregates = typed->state->prefix_aggregates;
      new_state->orphan_orders = typed->state->orphan_orders;
      new_state->customers = typed->state->customers;
      new_state->customer_overlay = typed->state->customer_overlay;
      new_state->customer_erased = typed->state->customer_erased;
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

  // Apply customer deltas
  for (const auto &delta : buffer.customer_deltas) {
    if (delta.sign == 1) {
      // Insert
      TpchQ22State::CustomerRecord rec;
      rec.prefix = delta.new_prefix;
      rec.balance_raw = delta.new_balance;
      rec.order_count = 0;

      // Check for orphan orders
      auto orphan_it = new_state->orphan_orders.find(delta.custkey);
      if (orphan_it != new_state->orphan_orders.end()) {
        rec.order_count = orphan_it->second;
        new_state->orphan_orders.erase(orphan_it);
      }

      new_state->MutableCustomer(delta.custkey) = rec;

      // Update positive aggregates
      if (rec.balance_raw > 0 && !rec.prefix.empty()) {
        new_state->prefix_positive_sum[rec.prefix] += rec.balance_raw;
        new_state->prefix_positive_count[rec.prefix]++;
      }

      // Update prefix aggregate: if zero-order, add to distribution
      if (rec.order_count == 0 && rec.balance_raw > 0 && !rec.prefix.empty()) {
        auto &agg = new_state->prefix_aggregates[rec.prefix];
        agg.AddBalance(rec.balance_raw);
      }
    } else if (delta.sign == -1) {
      // Delete
      const auto *found = new_state->FindCustomer(delta.custkey);
      if (!found) {
        // Not found — need rebuild
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }

      auto rec = *found;

      // Remove from positive aggregates
      if (rec.balance_raw > 0 && !rec.prefix.empty()) {
        new_state->prefix_positive_sum[rec.prefix] -= rec.balance_raw;
        new_state->prefix_positive_count[rec.prefix]--;
        if (new_state->prefix_positive_count[rec.prefix] <= 0) {
          new_state->prefix_positive_sum.erase(rec.prefix);
          new_state->prefix_positive_count.erase(rec.prefix);
        }
      }

      // Remove from prefix aggregate if zero-order
      if (rec.order_count == 0 && rec.balance_raw > 0 &&
          !rec.prefix.empty()) {
        auto agg_it = new_state->prefix_aggregates.find(rec.prefix);
        if (agg_it != new_state->prefix_aggregates.end()) {
          agg_it->second.RemoveBalance(rec.balance_raw);
        }
      }

      new_state->EraseCustomer(delta.custkey);
    } else {
      // Update
      if (!new_state->FindCustomer(delta.custkey)) {
        outcome.action = core::MaintenanceAction::RebuildAll;
        outcome.invalidations = All();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }

      auto &rec = new_state->MutableCustomer(delta.custkey);

      // Handle prefix change
      if (rec.prefix != delta.new_prefix) {
        // Remove from old prefix positive aggregates
        if (rec.balance_raw > 0 && !rec.prefix.empty()) {
          new_state->prefix_positive_sum[rec.prefix] -= rec.balance_raw;
          new_state->prefix_positive_count[rec.prefix]--;
          if (new_state->prefix_positive_count[rec.prefix] <= 0) {
            new_state->prefix_positive_sum.erase(rec.prefix);
            new_state->prefix_positive_count.erase(rec.prefix);
          }
        }

        // Remove from old prefix aggregate if zero-order
        if (rec.order_count == 0 && rec.balance_raw > 0 &&
            !rec.prefix.empty()) {
          auto agg_it = new_state->prefix_aggregates.find(rec.prefix);
          if (agg_it != new_state->prefix_aggregates.end()) {
            agg_it->second.RemoveBalance(rec.balance_raw);
          }
        }

        rec.prefix = delta.new_prefix;

        // Add to new prefix positive aggregates
        if (rec.balance_raw > 0 && !rec.prefix.empty()) {
          new_state->prefix_positive_sum[rec.prefix] += rec.balance_raw;
          new_state->prefix_positive_count[rec.prefix]++;
        }

        // Add to new prefix aggregate
        if (rec.order_count == 0 && rec.balance_raw > 0 &&
            !rec.prefix.empty()) {
          auto &agg = new_state->prefix_aggregates[rec.prefix];
          agg.AddBalance(rec.balance_raw);
        }
      }

      // Handle balance change
      if (rec.balance_raw != delta.new_balance) {
        int64_t old_balance = rec.balance_raw;
        int64_t new_balance = delta.new_balance;
        rec.balance_raw = new_balance;

        bool old_positive = old_balance > 0;
        bool new_positive = new_balance > 0;

        if (!rec.prefix.empty()) {
          if (old_positive) {
            new_state->prefix_positive_sum[rec.prefix] -= old_balance;
            new_state->prefix_positive_count[rec.prefix]--;
          }
          if (new_positive) {
            new_state->prefix_positive_sum[rec.prefix] += new_balance;
            new_state->prefix_positive_count[rec.prefix]++;
          }
          if (new_state->prefix_positive_count[rec.prefix] <= 0) {
            new_state->prefix_positive_sum.erase(rec.prefix);
            new_state->prefix_positive_count.erase(rec.prefix);
          }

          // Update prefix aggregate
          if (rec.order_count == 0 && !rec.prefix.empty()) {
            auto agg_it = new_state->prefix_aggregates.find(rec.prefix);
            if (agg_it != new_state->prefix_aggregates.end()) {
              if (old_positive)
                agg_it->second.RemoveBalance(old_balance);
              if (new_positive)
                agg_it->second.AddBalance(new_balance);
            } else if (new_positive) {
              auto &agg = new_state->prefix_aggregates[rec.prefix];
              agg.AddBalance(new_balance);
            }
          }
        }
      }
    }
  }

  // Apply order deltas
  for (const auto &delta : buffer.order_deltas) {
    int64_t old_count = 0;
    int64_t new_count = 0;
    std::string prefix;

    auto *customer = new_state->FindCustomer(delta.custkey)
                         ? &new_state->MutableCustomer(delta.custkey)
                         : nullptr;
    if (customer) {
      old_count = customer->order_count;
      new_count = old_count + delta.sign;
      prefix = customer->prefix;
      customer->order_count = new_count;

      // If order count crossed zero boundary, update prefix aggregate
      bool was_zero_order = (old_count == 0);
      bool is_zero_order = (new_count == 0);

      if (was_zero_order != is_zero_order) {
        if (!is_zero_order) {
          // Was zero-order, now has orders → remove from distribution
          if (customer->balance_raw > 0 && !prefix.empty()) {
            auto agg_it = new_state->prefix_aggregates.find(prefix);
            if (agg_it != new_state->prefix_aggregates.end()) {
              agg_it->second.RemoveBalance(customer->balance_raw);
            }
          }
        } else {
          // Now zero-order → add to distribution
          if (customer->balance_raw > 0 && !prefix.empty()) {
            auto &agg = new_state->prefix_aggregates[prefix];
            agg.AddBalance(customer->balance_raw);
          }
        }
      }
    } else {
      // Customer not found — update orphan orders
      auto orphan_it = new_state->orphan_orders.find(delta.custkey);
      old_count = (orphan_it != new_state->orphan_orders.end())
                      ? orphan_it->second
                      : 0;
      new_count = old_count + delta.sign;

      if (new_count <= 0) {
        if (orphan_it != new_state->orphan_orders.end())
          new_state->orphan_orders.erase(orphan_it);
      } else {
        new_state->orphan_orders[delta.custkey] = new_count;
      }
    }
  }

  outcome.action = core::MaintenanceAction::ApplyDelta;
  const auto typed_state =
      std::static_pointer_cast<const TpchQ22State>(std::move(new_state));
  outcome.updates.push_back(core::StateImage::Make(
      TpchQ22Layout::kAccelerator, TpchQ22Layout::kFull, existing_version,
      MakeStateImage(typed_state)));

  // Populate interference_us from cost model
  const auto &cost_model = services.MaintenanceCostModel();
  (void)cost_model; // interference is handled by the harness via cost model

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

  // Extract 7 country codes from arguments
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
  (void)services;
  if (!initial_scan)
    throw std::logic_error(
        "TPC-H Q22 aux requires a typed initial scan source");

  TpchQ22State state;

  // First, scan orders to determine which custkeys have orders
  std::unordered_map<int64_t, int64_t> order_counts;
  initial_scan->ScanOrders([&](const TpchQ22OrderInput &input) {
    if (input.custkey > 0) {
      order_counts[input.custkey]++;
    }
  });

  // Then scan customers
  initial_scan->ScanCustomers([&](const TpchQ22CustomerInput &input) {
    if (input.custkey <= 0)
      return;

    std::string prefix = ExtractPrefix(input.phone);
    TpchQ22State::CustomerRecord rec;
    rec.prefix = prefix;
    rec.balance_raw = input.acctbal_raw;

    // Determine order count
    auto order_it = order_counts.find(input.custkey);
    rec.order_count = (order_it != order_counts.end()) ? order_it->second : 0;

    (*state.customers)[input.custkey] = rec;

    // Positive balance aggregate
    if (rec.balance_raw > 0 && !prefix.empty()) {
      state.prefix_positive_sum[prefix] += rec.balance_raw;
      state.prefix_positive_count[prefix]++;
    }

    // Zero-order positive balance → add to prefix aggregate
    if (rec.order_count == 0 && rec.balance_raw > 0 && !prefix.empty()) {
      auto &agg = state.prefix_aggregates[prefix];
      agg.AddBalance(rec.balance_raw);
    }
  });

  // Handle orphan orders (custkeys in orders but not in customers)
  for (const auto &[custkey, count] : order_counts) {
    if (state.customers->find(custkey) == state.customers->end()) {
      state.orphan_orders[custkey] = count;
    }
  }

  // Rebuild all the suffix arrays for querying
  for (auto &[prefix, agg] : state.prefix_aggregates) {
    agg.RebuildSuffixes();
  }

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
  // Deduplicate codes
  std::set<std::string> unique_codes(country_codes.begin(),
                                     country_codes.end());

  // Compute overall positive sum/count for the selected codes
  int64_t global_positive_sum = 0;
  int64_t global_positive_count = 0;
  for (const auto &code : unique_codes) {
    auto sum_it = state.prefix_positive_sum.find(code);
    if (sum_it != state.prefix_positive_sum.end()) {
      global_positive_sum += sum_it->second;
      auto cnt_it = state.prefix_positive_count.find(code);
      global_positive_count += (cnt_it != state.prefix_positive_count.end())
                                   ? cnt_it->second
                                   : 0;
    }
  }

  // No positive customers → no rows (AVG is NULL in SQL)
  if (global_positive_count <= 0) {
    sink.Begin(core::OutputSchema{});
    sink.End();
    return;
  }

  // Compute the threshold balance (strictly greater than AVG)
  // AVG = global_positive_sum / global_positive_count
  // threshold is the integer scaled balance: acctbal > AVG means
  // acctbal_raw > global_positive_sum / global_positive_count
  // i.e., acctbal_raw * global_positive_count > global_positive_sum
  int64_t threshold_raw = 0;
  if (global_positive_count > 0) {
    // For strictly greater: acctbal_raw * count > sum
    // So we use floor division to find the strict boundary
    // threshold_raw = floor(global_positive_sum / global_positive_count)
    // A balance > AVG iff balance_raw > threshold_raw or
    // (balance_raw == threshold_raw + 1 when remainder != 0)
    // Actually: balance_raw > sum / count
    // iff count * balance_raw > sum
    threshold_raw = global_positive_sum / global_positive_count;
  }

  std::vector<TpchQ22OutputRow> output_rows;

  for (const auto &code : unique_codes) {
    auto agg_it = state.prefix_aggregates.find(code);
    if (agg_it == state.prefix_aggregates.end()) {
      continue;
    }

    auto &agg = const_cast<TpchQ22State::PrefixAggregate &>(agg_it->second);
    auto [count, sum] = agg.QueryGreaterThan(threshold_raw);

    if (count > 0) {
      TpchQ22OutputRow row;
      row.cntrycode = code;
      row.numcust = count;
      row.totacctbal_raw = sum;
      output_rows.push_back(row);
    }
  }

  // Sort by cntrycode
  std::sort(output_rows.begin(), output_rows.end(),
            [](const TpchQ22OutputRow &a, const TpchQ22OutputRow &b) {
              return a.cntrycode < b.cntrycode;
            });

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

} // namespace accelerator_codegen::aux_state::query
