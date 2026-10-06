#include "tpch/q22/postgres/aux/module.hpp"
#include "tpch/q22/calibration_writes.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

using Int128 = __int128;

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

// A decimal as an exact x100 integer; false when it is not representable.
bool ToScale2(int64_t raw_in, int32_t scale, int64_t &out) {
  Int128 raw = raw_in;
  for (int32_t s = scale; s < 2; ++s)
    raw *= 10;
  for (int32_t s = scale; s > 2; --s) {
    if (raw % 10 != 0)
      return false;
    raw /= 10;
  }
  if (raw > std::numeric_limits<int64_t>::max() ||
      raw <= std::numeric_limits<int64_t>::min())
    return false;
  out = static_cast<int64_t>(raw);
  return true;
}

bool Scale2Cell(const core::AuxRow &row, std::size_t index, int64_t &out) {
  if (index >= row.values.size() || row.values[index].is_null ||
      !std::holds_alternative<core::ScaledDecimal>(row.values[index].payload))
    return false;
  const auto decimal = std::get<core::ScaledDecimal>(row.values[index].payload);
  return ToScale2(decimal.value, decimal.scale, out);
}

// substring(c_phone from 1 for 2): the first two CHARACTERS (UTF-8 aware); a
// shorter phone yields what there is (possibly the empty string).
std::string PhonePrefix(const std::string &phone) {
  std::size_t i = 0;
  for (int chars = 0; i < phone.size() && chars < 2; ++chars) {
    ++i;
    while (i < phone.size() &&
           (static_cast<unsigned char>(phone[i]) & 0xC0) == 0x80)
      ++i;
  }
  return phone.substr(0, i);
}

// ── State editing ───────────────────────────────────────────────────────────

// Edits a private copy of the state: clones the prefix dictionary only when a
// new prefix appears and each touched prefix entry once.
class StateEditor {
public:
  explicit StateEditor(TpchQ22State &state, bool fresh = false)
      : state_(state), fresh_(fresh) {
    owned_.assign(state.prefixes.size(), fresh ? 1 : 0);
  }

  uint32_t PrefixId(const std::string &prefix) {
    const auto &ids = own_dictionary_ ? own_dictionary_->ids
                                      : state_.dictionary->ids;
    const auto it = ids.find(prefix);
    if (it != ids.end())
      return it->second;
    if (!own_dictionary_)
      own_dictionary_ =
          std::make_shared<TpchQ22PrefixDictionary>(*state_.dictionary);
    const auto id = static_cast<uint32_t>(state_.prefixes.size());
    own_dictionary_->ids.emplace(prefix, id);
    state_.prefixes.push_back(std::make_shared<const TpchQ22PrefixEntry>());
    owned_.push_back(1);
    return id;
  }

  TpchQ22PrefixEntry &Entry(uint32_t id) {
    if (!owned_[id]) {
      state_.prefixes[id] =
          std::make_shared<const TpchQ22PrefixEntry>(*state_.prefixes[id]);
      owned_[id] = 1;
    }
    return const_cast<TpchQ22PrefixEntry &>(*state_.prefixes[id]);
  }

  void Finish() {
    if (own_dictionary_)
      state_.dictionary = std::move(own_dictionary_);
  }

private:
  TpchQ22State &state_;
  bool fresh_;
  std::shared_ptr<TpchQ22PrefixDictionary> own_dictionary_;
  std::vector<char> owned_;
};

// Adds (sign = +1) or removes (-1) a record's contribution to its prefix:
// the AVG aggregate for balance > 0, and the zero-order balance multiset.
// False when the state does not contain what is being removed.
bool Contribute(StateEditor &editor, const TpchQ22CustomerRecord &rec, int sign) {
  if (rec.prefix_id == kTpchQ22NoCustomer || rec.prefix_id == kTpchQ22NullPhone ||
      rec.balance_raw == kTpchQ22NullBalance || rec.balance_raw <= 0)
    return true;
  auto &entry = editor.Entry(rec.prefix_id);
  entry.positive_count += sign;
  entry.positive_sum_raw += static_cast<Int128>(sign) * rec.balance_raw;
  if (entry.positive_count < 0)
    return false;
  if (rec.order_count == 0)
    return entry.zero_order.Add(rec.balance_raw, sign);
  return true;
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

core::InvalidationPlan StatePlan() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
  plan.partitions.insert(static_cast<::accelerator_codegen::aux_state::PartitionKey>(
      TpchQ22Layout::kFull));
  return plan;
}

core::StateImage MakeImage(std::shared_ptr<const TpchQ22State> state) {
  auto image = std::make_shared<TpchQ22StateImage>();
  image->kind = TpchQ22ImageKind::FullState;
  image->state = std::move(state);
  return core::StateImage::Make(
      TpchQ22Layout::kAccelerator, TpchQ22Layout::kFull, 0,
      std::shared_ptr<const TpchQ22StateImage>(std::move(image)));
}

const TpchQ22State *ReadState(core::AuxEngineServices &services) {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {TpchQ22Layout::kFull});
  for (const auto &image : snapshot.partitions) {
    if (image.partition != TpchQ22Layout::kFull)
      continue;
    const auto *typed = image.As<TpchQ22StateImage>();
    return typed && typed->kind == TpchQ22ImageKind::FullState && typed->state
               ? typed->state.get()
               : nullptr;
  }
  return nullptr;
}

// The IN-list: non-NULL strings, de-duplicated (NULL never matches).
std::set<std::string> CountryCodes(const core::QueryArguments &arguments) {
  std::set<std::string> codes;
  for (const auto &value : arguments.values)
    if (!value.is_null && std::holds_alternative<std::string>(value.payload))
      codes.insert(std::get<std::string>(value.payload));
  return codes;
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
void TpchQ22Module::Build(core::AuxEngineServices &,
                          core::StateBuilder &builder) {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q22 aux requires a typed initial scan source");
  auto state = std::make_shared<TpchQ22State>();
  StateEditor editor(*state, /*fresh=*/true);

  // Order counts first (custkeys without a customer row keep theirs), straight
  // into the per-custkey column.
  const bool counted = initial_scan->ScanOrderCounts([&](const TpchQ22OrderCount &c) {
    auto &rec = state->customers.Mutable(c.custkey);
    const int64_t total = static_cast<int64_t>(rec.order_count) + c.count;
    if (total > std::numeric_limits<int32_t>::max())
      throw std::overflow_error("TPC-H Q22 order count overflow");
    rec.order_count = static_cast<int32_t>(total);
  });
  if (!counted) {
    initial_scan->ScanOrders([&](const TpchQ22OrderInput &input) {
      auto &rec = state->customers.Mutable(input.custkey);
      if (rec.order_count == std::numeric_limits<int32_t>::max())
        throw std::overflow_error("TPC-H Q22 order count overflow");
      ++rec.order_count;
    });
  }

  initial_scan->ScanCustomers([&](const TpchQ22CustomerInput &input) {
    int64_t balance = 0;
    if (!ToScale2(input.acctbal_raw, input.acctbal_scale, balance))
      throw std::logic_error("TPC-H Q22: c_acctbal not representable at scale 2");
    auto &rec = state->customers.Mutable(input.custkey);
    rec.prefix_id = editor.PrefixId(PhonePrefix(input.phone));
    rec.balance_raw = balance;
    Contribute(editor, rec, +1);
  });
  editor.Finish();

  builder.Write(MakeImage(std::move(state)));
  builder.Finish();
}

// ── Mutation interpretation ─────────────────────────────────────────────────
core::InvalidationPlan
TpchQ22Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.customer_table ||
      mutation.table == config.orders_table)
    return StatePlan();
  return {};
}

// ── Mutation accumulation ───────────────────────────────────────────────────
void TpchQ22Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ22TxnBuffer &>(base_buffer);

  if (mutation.table == config.customer_table) {
    // Rows are [custkey, phone, acctbal].
    auto make = [&](const core::AuxRow *old_row, const core::AuxRow *new_row) {
      TpchQ22TxnBuffer::CustomerDelta delta;
      if (old_row) {
        if (!IntCell(*old_row, 0, delta.old_custkey)) {
          buffer.MarkRebuild();
          return;
        }
        delta.has_old = true;
      }
      if (new_row) {
        std::string phone;
        if (!IntCell(*new_row, 0, delta.new_custkey)) {
          buffer.MarkRebuild();
          return;
        }
        delta.has_new = true;
        delta.new_phone_null = new_row->values.size() > 1 && new_row->values[1].is_null;
        if (!delta.new_phone_null) {
          if (!StringCell(*new_row, 1, phone)) {
            buffer.MarkRebuild();
            return;
          }
          delta.new_prefix = PhonePrefix(phone);
        }
        delta.new_balance = kTpchQ22NullBalance;
        if (new_row->values.size() > 2 && !new_row->values[2].is_null &&
            !Scale2Cell(*new_row, 2, delta.new_balance)) {
          buffer.MarkRebuild();
          return;
        }
      }
      buffer.customer_deltas.push_back(std::move(delta));
    };
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows)
        make(nullptr, &row);
      break;
    case core::MutationKind::Delete:
      for (const auto &row : mutation.old_rows)
        make(&row, nullptr);
      break;
    case core::MutationKind::Update:
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i)
        make(&mutation.old_rows[i], &mutation.new_rows[i]);
      break;
    }
    return;
  }

  if (mutation.table == config.orders_table) {
    // Rows are [o_custkey]; a NULL custkey never matches a customer.
    auto order = [&](const core::AuxRow &row, int8_t sign) {
      if (row.values.empty() || row.values[0].is_null)
        return;
      int64_t custkey = 0;
      if (!IntCell(row, 0, custkey)) {
        buffer.MarkRebuild();
        return;
      }
      buffer.order_deltas.push_back({custkey, sign});
    };
    switch (mutation.kind) {
    case core::MutationKind::Insert:
      for (const auto &row : mutation.new_rows)
        order(row, 1);
      break;
    case core::MutationKind::Delete:
      for (const auto &row : mutation.old_rows)
        order(row, -1);
      break;
    case core::MutationKind::Update:
      if (mutation.old_rows.size() != mutation.new_rows.size()) {
        buffer.MarkRebuild();
        return;
      }
      for (std::size_t i = 0; i < mutation.old_rows.size(); ++i) {
        order(mutation.old_rows[i], -1);
        order(mutation.new_rows[i], 1);
      }
      break;
    }
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
    core::FinishRebuildAll(outcome);
    return outcome;
  }
  if (buffer.customer_deltas.empty() && buffer.order_deltas.empty())
    return outcome;

  // Honor the cost model's (or a forced) choice: this state has no finer
  // partitions, so anything but ApplyDelta is a whole-state rebuild.
  {
    const auto decision = services.ChooseMaintenance(
        {static_cast<uint64_t>(buffer.customer_deltas.size() + buffer.order_deltas.size()), 1, true, false, true, false, false,
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
  }

  const TpchQ22State *current = ReadState(services);
  if (!current) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }
  // A copy shares every chunk / prefix entry; edits clone only what they touch.
  auto state = std::make_shared<TpchQ22State>(*current);
  StateEditor editor(*state);

  auto inconsistent = [&] {
    core::FinishRebuildAll(outcome);
    return outcome;
  };

  for (const auto &delta : buffer.customer_deltas) {
    if (delta.has_old) {
      auto rec = state->customers.Get(delta.old_custkey);
      if (rec.prefix_id == kTpchQ22NoCustomer || !Contribute(editor, rec, -1))
        return inconsistent();
      // The row is gone; orders of this custkey are kept (they may outlive it).
      rec.prefix_id = kTpchQ22NoCustomer;
      rec.balance_raw = 0;
      state->customers.Set(delta.old_custkey, rec);
    }
    if (delta.has_new) {
      auto rec = state->customers.Get(delta.new_custkey);
      if (rec.prefix_id != kTpchQ22NoCustomer)
        return inconsistent(); // duplicate key
      rec.prefix_id = delta.new_phone_null ? kTpchQ22NullPhone
                                           : editor.PrefixId(delta.new_prefix);
      rec.balance_raw = delta.new_balance;
      if (!Contribute(editor, rec, +1))
        return inconsistent();
      state->customers.Set(delta.new_custkey, rec);
    }
  }

  for (const auto &delta : buffer.order_deltas) {
    auto rec = state->customers.Get(delta.custkey);
    const int64_t orders = static_cast<int64_t>(rec.order_count) + delta.sign;
    if (orders < 0 || orders > std::numeric_limits<int32_t>::max())
      return inconsistent();
    // Only crossing zero changes the zero-order multiset.
    const bool crossing = (rec.order_count == 0) != (orders == 0);
    if (crossing && !Contribute(editor, rec, -1))
      return inconsistent();
    rec.order_count = static_cast<int32_t>(orders);
    if (crossing && !Contribute(editor, rec, +1))
      return inconsistent();
    state->customers.Set(delta.custkey, rec);
  }
  editor.Finish();

  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.updates.push_back(
      MakeImage(std::shared_ptr<const TpchQ22State>(std::move(state))));
  return outcome;
}

// ── CheckValidity ───────────────────────────────────────────────────────────
bool TpchQ22Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ22Module::EmitRows(const std::vector<TpchQ22OutputRow> &rows,
                             core::AuxRowSink &sink) {
  sink.Begin({{{"cntrycode", core::AuxType::String, false},
               {"numcust", core::AuxType::Int64, false},
               {"totacctbal", core::AuxType::ScaledDecimal, false}}});
  for (const auto &row : rows) {
    core::AuxRow out;
    out.values.push_back(core::AuxValue::Of(core::AuxType::String, row.cntrycode));
    out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, row.numcust));
    out.values.push_back(core::AuxValue::Of(
        core::AuxType::ScaledDecimal, core::ScaledDecimal{row.totacctbal_raw, 2}));
    sink.WriteRow(out);
  }
  sink.End();
}

// ── Execute ─────────────────────────────────────────────────────────────────
void TpchQ22Module::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  const TpchQ22State *state = nullptr;
  if (services.StateStore()
          .CheckValidity({Id(), {}}, services.CurrentVisibility())
          .IsValid())
    state = ReadState(services);
  if (!state) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  // AVG(c_acctbal) over customers with c_acctbal > 0 in the listed prefixes,
  // as the exact rational S / C.
  struct Selected {
    const std::string *code;
    const TpchQ22PrefixEntry *entry;
  };
  const auto codes = CountryCodes(arguments);
  std::vector<Selected> selected;
  int64_t positive_count = 0;
  Int128 positive_sum = 0;
  for (const auto &code : codes) {
    const auto it = state->dictionary->ids.find(code);
    if (it == state->dictionary->ids.end())
      continue;
    const auto *entry = state->prefixes[it->second].get();
    selected.push_back({&code, entry});
    positive_count += entry->positive_count;
    positive_sum += entry->positive_sum_raw;
  }
  std::vector<TpchQ22OutputRow> rows;
  if (positive_count > 0) {
    // balance > S / C  <=>  balance > floor(S / C) for integral balances
    // (S > 0 here), so one suffix query per code answers it exactly.
    const auto threshold = static_cast<int64_t>(positive_sum / positive_count);
    for (const auto &sel : selected) { // codes iterate in sorted order
      const auto above = sel.entry->zero_order.SuffixAbove(threshold);
      if (above.count <= 0)
        continue;
      if (above.sum > std::numeric_limits<int64_t>::max())
        throw std::overflow_error("TPC-H Q22 totacctbal exceeds int64");
      rows.push_back({*sel.code, above.count, static_cast<int64_t>(above.sum)});
    }
  }
  EmitRows(rows, sink);
}

// ── ExecuteFallback ─────────────────────────────────────────────────────────
// Exact answer straight from the tables (own uncommitted writes).
void TpchQ22Module::ExecuteFallback(core::AuxEngineServices &,
                                    const core::QueryArguments &arguments,
                                    core::AuxRowSink &sink) {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q22 aux requires a typed initial scan source");
  const auto codes = CountryCodes(arguments);

  std::unordered_map<int64_t, bool> has_orders; // custkeys with an order
  const bool counted = initial_scan->ScanOrderCounts(
      [&](const TpchQ22OrderCount &c) { has_orders[c.custkey] = c.count > 0; });
  if (!counted)
    initial_scan->ScanOrders(
        [&](const TpchQ22OrderInput &input) { has_orders[input.custkey] = true; });

  struct Candidate {
    std::string code;
    int64_t balance;
  };
  std::vector<Candidate> zero_order;
  int64_t positive_count = 0;
  Int128 positive_sum = 0;
  initial_scan->ScanCustomers([&](const TpchQ22CustomerInput &input) {
    const auto code = PhonePrefix(input.phone);
    int64_t balance = 0;
    if (!codes.count(code) || !ToScale2(input.acctbal_raw, input.acctbal_scale, balance) ||
        balance <= 0)
      return;
    ++positive_count;
    positive_sum += balance;
    const auto it = has_orders.find(input.custkey);
    if (it == has_orders.end() || !it->second)
      zero_order.push_back({code, balance});
  });

  std::map<std::string, std::pair<int64_t, Int128>> groups;
  if (positive_count > 0)
    for (const auto &c : zero_order)
      if (static_cast<Int128>(c.balance) * positive_count > positive_sum) {
        auto &g = groups[c.code];
        ++g.first;
        g.second += c.balance;
      }
  std::vector<TpchQ22OutputRow> rows;
  for (const auto &[code, g] : groups) {
    if (g.second > std::numeric_limits<int64_t>::max())
      throw std::overflow_error("TPC-H Q22 totacctbal exceeds int64");
    rows.push_back({code, g.first, static_cast<int64_t>(g.second)});
  }
  EmitRows(rows, sink);
}

std::vector<core::CalibrationWrite> TpchQ22Module::CalibrationWrites() const {
  return TpchQ22CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
