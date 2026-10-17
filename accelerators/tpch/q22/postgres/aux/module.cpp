#include "tpch/q22/postgres/aux/module.hpp"
#include "tpch/q22/calibration_writes.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/parallel_sort.hpp"
#include "substrate/aux/core/partitioned_build.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {

using Int128 = __int128;

// Owner runs (customer-array directory pages) per maintenance slice: a page
// clone plus ~10 key edits is ~5 us.
constexpr std::size_t kRunGrain = 64;
// Key events per sorted slice of the owner sort.
constexpr std::size_t kSortGrain = 65536;
// Below this many balance ops a commit updates its prefixes inline.
constexpr std::size_t kPrefixOpThreshold = 4096;

// c_acctbal is DECIMAL(15,2).
constexpr int32_t kBalanceScale = 2;

using core::IntCell;
using core::StringCell;

// A decimal as an exact x100 integer; false when it is not representable.
bool ToScale2(int64_t raw_in, int32_t scale, int64_t &out) {
  Int128 raw = raw_in;
  for (int32_t s = scale; s < kBalanceScale; ++s)
    raw *= 10;
  for (int32_t s = scale; s > kBalanceScale; --s) {
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
  int64_t raw = 0;
  int32_t scale = 0;
  return core::DecimalCell(row, index, raw, scale) && ToScale2(raw, scale, out);
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

// A customer with a phone prefix and a positive balance counts in the AVG
// aggregate of its prefix (and in the zero-order multiset when it has no order).
bool CountsInAverage(const TpchQ22CustomerRecord &rec) {
  return rec.prefix_id != kTpchQ22NoCustomer && rec.prefix_id != kTpchQ22NullPhone &&
         rec.balance_raw != kTpchQ22NullBalance && rec.balance_raw > 0;
}

// Adds (sign = +1) or removes (-1) a record's contribution to its prefix.
// False when the state does not contain what is being removed.
bool Contribute(StateEditor &editor, const TpchQ22CustomerRecord &rec, int sign) {
  if (!CountsInAverage(rec))
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

// One phone prefix's share of a commit: adjustments of its AVG aggregate and
// of the balances in its zero-order multiset as (balance, +-1).
struct PrefixDelta {
  int64_t positive_count = 0;
  Int128 positive_sum = 0;
  std::vector<std::pair<int64_t, int64_t>> zero_order;
};

// What a slice of owner runs computed: its prefixes' deltas, or `ok = false`
// when the state does not contain what a delta removes.
struct Staged {
  std::unordered_map<uint32_t, PrefixDelta> prefixes;
  bool ok = true;

  // The signed effect of one customer record, as Contribute applies it.
  void Contribute(const TpchQ22CustomerRecord &rec, int sign) {
    if (!CountsInAverage(rec))
      return;
    auto &delta = prefixes[rec.prefix_id];
    delta.positive_count += sign;
    delta.positive_sum += static_cast<Int128>(sign) * rec.balance_raw;
    if (rec.order_count == 0)
      delta.zero_order.emplace_back(rec.balance_raw, sign);
  }
  void Merge(Staged &&from) {
    ok = ok && from.ok;
    for (auto &[id, delta] : from.prefixes) {
      auto &into = prefixes[id];
      into.positive_count += delta.positive_count;
      into.positive_sum += delta.positive_sum;
      into.zero_order.insert(into.zero_order.end(), delta.zero_order.begin(),
                             delta.zero_order.end());
    }
  }
};

// Applies balance adjustments net per balance; false when a multiplicity would
// go negative.
bool ApplyBalanceOps(core::SortedCountSum &multiset,
                     std::vector<std::pair<int64_t, int64_t>> &ops) {
  std::sort(ops.begin(), ops.end());
  for (std::size_t i = 0; i < ops.size();) {
    int64_t net = 0;
    std::size_t j = i;
    for (; j < ops.size() && ops[j].first == ops[i].first; ++j)
      net += ops[j].second;
    if (!multiset.Add(ops[i].first, net))
      return false;
    i = j;
  }
  return true;
}

// A delta applied to the customer array by the slice owning its key.
struct KeyEvent {
  enum class Kind : uint8_t { RemoveCustomer, AddCustomer, Order };
  Kind kind;
  uint64_t owner; // customers.OwnerOf(key)
  int64_t key;
  std::size_t delta; // index into the customer or order deltas
};

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

std::shared_ptr<const TpchQ22State> LoadState(core::AuxEngineServices &services) {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {TpchQ22Layout::kFull});
  for (const auto &image : snapshot.partitions) {
    if (image.partition != TpchQ22Layout::kFull)
      continue;
    const auto *typed = image.As<TpchQ22StateImage>();
    return typed && typed->kind == TpchQ22ImageKind::FullState && typed->state
               ? typed->state
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

// The customers one build partition has seen. Prefix ids are local until the
// partitions fold in table order, which assigns them in order of first
// appearance, as in a serial scan.
struct CustomerPartial {
  struct PrefixTotals {
    int64_t positive_count = 0;
    Int128 positive_sum = 0;
    std::map<int64_t, int64_t> zero_order; // balance -> customers without orders
  };

  TpchQ22PrefixDictionary dictionary;
  std::vector<PrefixTotals> totals; // by local prefix id
  core::AnyKeyCowArray<TpchQ22CustomerRecord, 6> customers;

  uint32_t PrefixId(const std::string &prefix) {
    const auto [it, added] = dictionary.ids.emplace(prefix, totals.size());
    if (added)
      totals.emplace_back();
    return it->second;
  }
  void Add(int64_t custkey, const TpchQ22CustomerRecord &rec) {
    customers.Set(custkey, rec);
    if (!CountsInAverage(rec))
      return;
    auto &prefix = totals[rec.prefix_id];
    ++prefix.positive_count;
    prefix.positive_sum += rec.balance_raw;
    if (rec.order_count == 0)
      ++prefix.zero_order[rec.balance_raw];
  }
  void Merge(CustomerPartial &&from) {
    std::vector<const std::string *> prefixes(from.totals.size());
    for (const auto &[prefix, id] : from.dictionary.ids)
      prefixes[id] = &prefix;
    std::vector<uint32_t> remap(from.totals.size());
    for (std::size_t id = 0; id < prefixes.size(); ++id)
      remap[id] = PrefixId(*prefixes[id]);
    from.customers.ForEach([&](int64_t custkey, TpchQ22CustomerRecord rec) {
      if (rec.prefix_id == kTpchQ22NoCustomer)
        return;
      if (rec.prefix_id != kTpchQ22NullPhone)
        rec.prefix_id = remap[rec.prefix_id];
      customers.Set(custkey, rec);
    });
    for (std::size_t id = 0; id < from.totals.size(); ++id) {
      auto &into = totals[remap[id]];
      into.positive_count += from.totals[id].positive_count;
      into.positive_sum += from.totals[id].positive_sum;
      for (const auto &[balance, count] : from.totals[id].zero_order)
        into.zero_order[balance] += count;
    }
  }
};

} // namespace

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

core::AcceleratorId TpchQ22Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ22Module::Dependencies() const {
  return {{config.customer_table, {0, 1, 2}, {0, 4, 5}},
          {config.orders_table, {0}, {1}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ22Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ22TxnBuffer>();
}

std::shared_ptr<const TpchQ22State>
TpchQ22Module::BuildState(core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q22 aux requires a typed initial scan source");

  using OrderCounts = core::AnyKeyCowArray<int64_t, 12>;
  const OrderCounts order_counts = core::PartitionedBuild<OrderCounts>(
      services, [] { return OrderCounts{}; },
      [&](std::size_t partitions, auto &&sink) {
        initial_scan->ScanOrders(services, partitions,
                                 [&](std::size_t p, const TpchQ22OrderInput &input) {
                                   sink(p, core::SingleRowBatch<TpchQ22OrderInput>{input});
                                 });
      },
      [](OrderCounts &counts, const TpchQ22OrderInput &input) {
        counts.Add(input.custkey, 1);
      },
      [](OrderCounts &into, OrderCounts &&from) {
        into.MergeFrom(std::move(from),
                       [](int64_t mine, int64_t theirs) { return mine + theirs; });
      });

  auto customers = core::PartitionedBuild<CustomerPartial>(
      services, [] { return CustomerPartial{}; },
      [&](std::size_t partitions, auto &&sink) {
        initial_scan->ScanCustomers(services, partitions,
                                    [&](std::size_t p, const TpchQ22CustomerInput &input) {
                                      sink(p, core::SingleRowBatch<TpchQ22CustomerInput>{input});
                                    });
      },
      [&](CustomerPartial &partial, const TpchQ22CustomerInput &input) {
        int64_t balance = 0;
        if (!ToScale2(input.acctbal_raw, input.acctbal_scale, balance))
          throw std::logic_error("TPC-H Q22: c_acctbal not representable at scale 2");
        const int64_t orders = order_counts.Get(input.custkey);
        if (orders > std::numeric_limits<int32_t>::max())
          throw std::overflow_error("TPC-H Q22 order count overflow");
        partial.Add(input.custkey,
                    {balance, static_cast<int32_t>(orders),
                     partial.PrefixId(PhonePrefix(input.phone))});
      },
      [](CustomerPartial &into, CustomerPartial &&from) { into.Merge(std::move(from)); });

  auto state = std::make_shared<TpchQ22State>();
  state->customers = std::move(customers.customers);
  // Custkeys with orders but no customer row keep their order count.
  order_counts.ForEach([&](int64_t custkey, int64_t count) {
    if (count == 0 || state->customers.Get(custkey).prefix_id != kTpchQ22NoCustomer)
      return;
    if (count > std::numeric_limits<int32_t>::max())
      throw std::overflow_error("TPC-H Q22 order count overflow");
    state->customers.Set(custkey, {0, static_cast<int32_t>(count), kTpchQ22NoCustomer});
  });
  state->dictionary =
      std::make_shared<TpchQ22PrefixDictionary>(std::move(customers.dictionary));
  state->prefixes.resize(customers.totals.size());
  core::ParallelEach(services, customers.totals.size(), 1, [&](std::size_t id) {
    auto entry = std::make_shared<TpchQ22PrefixEntry>();
    entry->positive_count = customers.totals[id].positive_count;
    entry->positive_sum_raw = customers.totals[id].positive_sum;
    for (const auto &[balance, count] : customers.totals[id].zero_order)
      entry->zero_order.Add(balance, count);
    state->prefixes[id] = std::move(entry);
  });
  return state;
}

void TpchQ22Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  builder.Write(MakeImage(BuildState(services)));
  builder.Finish();
}

core::InvalidationPlan
TpchQ22Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.customer_table ||
      mutation.table == config.orders_table)
    return StatePlan();
  return {};
}

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
        core::MaintenanceRequest::IncrementalSingleState(
            buffer.customer_deltas.size() + buffer.order_deltas.size(),
            services.ExpectedQueriesBeforeNextWrite()));
    if (decision.action != core::MaintenanceAction::ApplyDelta) {
      core::FinishRebuildAll(outcome, decision);
      return outcome;
    }
  }

  const auto current = LoadState(services);
  if (!current) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }
  // A copy shares every chunk / prefix entry; edits clone only what they touch.
  auto state = std::make_shared<TpchQ22State>(*current);
  StateEditor editor(*state);

  // Serial prologue: prefix ids for the new rows, then the deltas as key
  // events (all customer deltas, then all order deltas), grouped by the owner
  // of their key's customer-array directory page, arrival order kept per key.
  std::vector<uint32_t> new_prefix_ids(buffer.customer_deltas.size());
  std::vector<KeyEvent> events;
  events.reserve(2 * buffer.customer_deltas.size() + buffer.order_deltas.size());
  for (std::size_t i = 0; i < buffer.customer_deltas.size(); ++i) {
    const auto &delta = buffer.customer_deltas[i];
    if (delta.has_old)
      events.push_back({KeyEvent::Kind::RemoveCustomer, 0, delta.old_custkey, i});
    if (delta.has_new) {
      if (!delta.new_phone_null)
        new_prefix_ids[i] = editor.PrefixId(delta.new_prefix);
      events.push_back({KeyEvent::Kind::AddCustomer, 0, delta.new_custkey, i});
    }
  }
  for (std::size_t i = 0; i < buffer.order_deltas.size(); ++i)
    events.push_back({KeyEvent::Kind::Order, 0, buffer.order_deltas[i].custkey, i});
  for (auto &event : events) {
    event.owner = state->customers.OwnerOf(event.key);
    state->customers.Reserve(event.key);
  }
  core::ParallelStableSort(services, events,
                     [](const KeyEvent &a, const KeyEvent &b) { return a.owner < b.owner; },
                     kSortGrain);
  const auto run_starts =
      core::RunStarts(events, [](const KeyEvent &event) { return event.owner; });

  // Slices own whole runs, so each edits only its own directory pages and
  // chunks; the sparse-key run is one run and so one slice.
  auto apply = [&](const KeyEvent &event, Staged &staged) {
    auto rec = state->customers.Get(event.key);
    switch (event.kind) {
    case KeyEvent::Kind::RemoveCustomer:
      if (rec.prefix_id == kTpchQ22NoCustomer)
        return false;
      staged.Contribute(rec, -1);
      // The row is gone; orders of this custkey are kept (they may outlive it).
      rec.prefix_id = kTpchQ22NoCustomer;
      rec.balance_raw = 0;
      break;
    case KeyEvent::Kind::AddCustomer: {
      if (rec.prefix_id != kTpchQ22NoCustomer)
        return false; // duplicate key
      const auto &delta = buffer.customer_deltas[event.delta];
      rec.prefix_id = delta.new_phone_null ? kTpchQ22NullPhone
                                           : new_prefix_ids[event.delta];
      rec.balance_raw = delta.new_balance;
      staged.Contribute(rec, +1);
      break;
    }
    case KeyEvent::Kind::Order: {
      const int64_t orders =
          static_cast<int64_t>(rec.order_count) + buffer.order_deltas[event.delta].sign;
      if (orders < 0 || orders > std::numeric_limits<int32_t>::max())
        return false;
      // Only crossing zero changes the zero-order multiset.
      if ((rec.order_count == 0) != (orders == 0) && CountsInAverage(rec))
        staged.prefixes[rec.prefix_id].zero_order.emplace_back(rec.balance_raw,
                                                               orders == 0 ? 1 : -1);
      rec.order_count = static_cast<int32_t>(orders);
      break;
    }
    }
    state->customers.Set(event.key, rec);
    return true;
  };
  Staged staged;
  core::ParallelReduce(
      services, run_starts.size() - 1, kRunGrain, staged,
      [&](Staged &part, std::size_t first_run, std::size_t last_run) {
        for (std::size_t i = run_starts[first_run]; i < run_starts[last_run] && part.ok; ++i)
          part.ok = apply(events[i], part);
      },
      [](Staged &into, Staged &&from) { into.Merge(std::move(from)); });
  if (!staged.ok) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }

  // Prefixes are independent: each clones and updates its own entry.
  std::vector<std::pair<uint32_t, PrefixDelta *>> touched;
  std::size_t balance_ops = 0;
  for (auto &[id, delta] : staged.prefixes) {
    touched.emplace_back(id, &delta);
    balance_ops += delta.zero_order.size();
  }
  std::sort(touched.begin(), touched.end());
  std::vector<char> consistent(touched.size(), 1);
  core::ParallelEachWhen(services, touched.size(), balance_ops, kPrefixOpThreshold,
                         [&](std::size_t i) {
    auto &entry = editor.Entry(touched[i].first);
    const auto &delta = *touched[i].second;
    entry.positive_count += delta.positive_count;
    entry.positive_sum_raw += delta.positive_sum;
    consistent[i] = entry.positive_count >= 0 &&
                    ApplyBalanceOps(entry.zero_order, touched[i].second->zero_order);
  });
  if (std::find(consistent.begin(), consistent.end(), 0) != consistent.end()) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }
  editor.Finish();

  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.updates.push_back(
      MakeImage(std::shared_ptr<const TpchQ22State>(std::move(state))));
  return outcome;
}

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

void TpchQ22Module::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  const auto state =
      services.StateStore().CheckValidity({Id(), {}}, services.CurrentVisibility()).IsValid()
          ? LoadState(services)
          : nullptr;
  if (!state) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  Answer(*state, arguments, sink);
}

void TpchQ22Module::Answer(const TpchQ22State &state,
                           const core::QueryArguments &arguments,
                           core::AuxRowSink &sink) {
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
    const auto it = state.dictionary->ids.find(code);
    if (it == state.dictionary->ids.end())
      continue;
    const auto *entry = state.prefixes[it->second].get();
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

std::vector<core::CalibrationWrite> TpchQ22Module::CalibrationWrites() const {
  return TpchQ22CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
