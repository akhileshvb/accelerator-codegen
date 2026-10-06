#include "tpch/q18/postgres/aux/module.hpp"
#include "tpch/q18/calibration_writes.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
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
  if (std::holds_alternative<core::AuxDate>(cell.payload)) {
    out = std::get<core::AuxDate>(cell.payload).days_since_epoch;
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

bool ReadLineitem(const core::AuxRow &row, int64_t &orderkey,
                  int64_t &quantity_raw) {
  if (!IntCell(row, 0, orderkey))
    return false;
  int64_t raw = 0;
  int32_t scale = 0;
  if (!RawDecimalCell(row, 1, raw, scale))
    return false;
  if (scale != 2)
    return false;
  quantity_raw = raw;
  return true;
}

std::shared_ptr<const TpchQ18StateImage>
MakeStateImage(const std::shared_ptr<const TpchQ18State> &state) {
  auto image = std::make_shared<TpchQ18StateImage>();
  image->state = state;
  return image;
}

const TpchQ18StateImage *AsQ18Image(const core::StateImage &image) {
  const auto *payload = image.As<TpchQ18StateImage>();
  return payload && payload->state ? payload : nullptr;
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope =
      ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

bool ReadOrder(const core::AuxRow &row, TpchQ18OrderInput &out) {
  int64_t raw = 0;
  int32_t scale = 0;
  if (!IntCell(row, 0, out.key) || !IntCell(row, 1, out.custkey))
    return false;
  int64_t days = 0;
  if (!IntCell(row, 2, days) || days < std::numeric_limits<int32_t>::min() ||
      days > std::numeric_limits<int32_t>::max())
    return false;
  out.orderdate_days = static_cast<int32_t>(days);
  if (!RawDecimalCell(row, 3, raw, scale) || scale != 2)
    return false;
  out.totalprice_raw = raw;
  return true;
}

bool ReadCustomer(const core::AuxRow &row, TpchQ18CustomerInput &out) {
  return IntCell(row, 0, out.key) && StringCell(row, 1, out.name);
}

// ── The QUANTITY parameter ──────────────────────────────────────────────────
// The query keeps an order when SUM(l_quantity) > QUANTITY. Sums are integers
// at scale 2, so for any exact decimal QUANTITY the test is
// sum_raw > floor(QUANTITY * 100): `Value` carries that floor, `All` and
// `None` cover values beyond the int64 range, -Infinity / NULL / NaN.
struct QuantityBound {
  enum class Kind { None, All, Value } kind = Kind::None;
  int64_t raw = 0;
};

// floor(value * 100) of a decimal text ("-12.345", "1e3", ".5", "Infinity").
QuantityBound ParseQuantityText(std::string text) {
  QuantityBound bound; // None: unparsable text behaves like NULL
  const auto not_space = [](unsigned char c) { return !std::isspace(c); };
  text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
  text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(),
             text.end());
  std::string lower;
  for (const char c : text)
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  if (lower == "infinity" || lower == "+infinity" || lower == "inf" ||
      lower == "+inf" || lower == "nan")
    return bound; // nothing is greater than +Infinity or NaN
  if (lower == "-infinity" || lower == "-inf") {
    bound.kind = QuantityBound::Kind::All;
    return bound;
  }

  std::size_t pos = 0;
  bool negative = false;
  if (pos < text.size() && (text[pos] == '+' || text[pos] == '-'))
    negative = text[pos++] == '-';
  std::string digits;
  std::size_t fraction_digits = 0;
  bool seen_digit = false, seen_point = false;
  for (; pos < text.size(); ++pos) {
    const char c = text[pos];
    if (c >= '0' && c <= '9') {
      digits.push_back(c);
      seen_digit = true;
      if (seen_point)
        ++fraction_digits;
    } else if (c == '.' && !seen_point) {
      seen_point = true;
    } else {
      break;
    }
  }
  if (!seen_digit)
    return bound;
  long exponent = 0;
  if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
    char *end = nullptr;
    exponent = std::strtol(text.c_str() + pos + 1, &end, 10);
    if (end == text.c_str() + pos + 1)
      return bound;
    pos = static_cast<std::size_t>(end - text.c_str());
  }
  if (pos != text.size())
    return bound;

  // value * 100 = digits * 10^shift
  const long shift =
      exponent - static_cast<long>(fraction_digits) + 2;
  const std::size_t first = digits.find_first_not_of('0');
  digits = first == std::string::npos ? std::string() : digits.substr(first);
  bool dropped_nonzero = false;
  if (shift < 0) {
    const std::size_t drop = static_cast<std::size_t>(-shift);
    if (drop >= digits.size()) {
      dropped_nonzero = !digits.empty();
      digits.clear();
    } else {
      dropped_nonzero =
          digits.find_first_not_of('0', digits.size() - drop) != std::string::npos;
      digits.resize(digits.size() - drop);
    }
  } else if (!digits.empty()) {
    if (static_cast<std::size_t>(shift) > 40)
      digits.assign(40, '9');
    else
      digits.append(static_cast<std::size_t>(shift), '0');
  }
  if (digits.size() > 30)
    digits.assign(30, '9');
  unsigned __int128 magnitude = 0;
  for (const char c : digits)
    magnitude = magnitude * 10 + static_cast<unsigned>(c - '0');
  __int128 value = static_cast<__int128>(magnitude);
  if (negative) {
    value = -value;
    if (dropped_nonzero)
      --value; // floor
  }
  if (value > std::numeric_limits<int64_t>::max())
    return bound; // beyond every sum
  if (value < std::numeric_limits<int64_t>::min()) {
    bound.kind = QuantityBound::Kind::All;
    return bound;
  }
  bound.kind = QuantityBound::Kind::Value;
  bound.raw = static_cast<int64_t>(value);
  return bound;
}

QuantityBound ParseQuantity(const core::QueryArguments &arguments) {
  QuantityBound none;
  if (arguments.values.empty() || arguments.values[0].is_null)
    return none;
  const auto &payload = arguments.values[0].payload;
  if (const auto *text = std::get_if<std::string>(&payload))
    return ParseQuantityText(*text);
  if (const auto *i = std::get_if<int64_t>(&payload))
    return ParseQuantityText(std::to_string(*i));
  if (const auto *u = std::get_if<uint64_t>(&payload))
    return ParseQuantityText(std::to_string(*u));
  if (const auto *d = std::get_if<core::ScaledDecimal>(&payload)) {
    std::string text = std::to_string(d->value);
    text += "e" + std::to_string(-d->scale);
    return ParseQuantityText(text);
  }
  if (const auto *f = std::get_if<double>(&payload)) {
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%.17g", *f);
    return ParseQuantityText(buffer);
  }
  return none;
}

// ORDER BY o_totalprice DESC, o_orderdate ASC (ties: custkey, orderkey, so
// the output is deterministic).
bool BetterOutputRow(const TpchQ18OutputRow &lhs, const TpchQ18OutputRow &rhs) {
  if (lhs.totalprice_raw != rhs.totalprice_raw)
    return lhs.totalprice_raw > rhs.totalprice_raw;
  if (lhs.orderdate_days != rhs.orderdate_days)
    return lhs.orderdate_days < rhs.orderdate_days;
  if (lhs.orderkey != rhs.orderkey)
    return lhs.orderkey < rhs.orderkey;
  return lhs.custkey < rhs.custkey;
}

void EmitEmpty(core::AuxRowSink &sink) {
  sink.Begin(core::OutputSchema{});
  sink.End();
}

// Customers of at most this many keys are fetched by key, more by one scan.
constexpr std::size_t kMaxKeyedCustomerLookup = 4096;

} // namespace

void TpchQ18InitialScan::LookupCustomerNames(
    const std::vector<int64_t> &custkeys,
    const std::function<void(const TpchQ18CustomerInput &)> &consumer) {
  const std::unordered_set<int64_t> wanted(custkeys.begin(), custkeys.end());
  ScanCustomers([&](const TpchQ18CustomerInput &input) {
    if (wanted.find(input.key) != wanted.end())
      consumer(input);
  });
}

// ── State ───────────────────────────────────────────────────────────────────
void TpchQ18State::Reindex(int64_t orderkey) {
  const TpchQ18OrderRec *rec = orders.Find(orderkey);
  if (!rec)
    return;
  if (rec->Empty()) {
    orders.Erase(orderkey);
    hot.Erase(orderkey);
    return;
  }
  const bool should_be_hot = rec->Qualifies(kHotFloorRaw);
  const bool is_hot = hot.Contains(orderkey);
  if (should_be_hot && !is_hot)
    hot.Upsert(orderkey, 1);
  else if (!should_be_hot && is_hot)
    hot.Erase(orderkey);
}

bool TpchQ18State::AddLineitem(int64_t orderkey, int64_t quantity_raw,
                               int sign) {
  TpchQ18OrderRec &rec = MutableRec(orderkey);
  if (sign < 0) {
    if (rec.Lines() == 0)
      return false;
    rec.sum_raw -= quantity_raw;
    --rec.flags_lines;
  } else {
    if (rec.Lines() == ~TpchQ18OrderRec::kHasOrder)
      return false;
    rec.sum_raw += quantity_raw;
    ++rec.flags_lines;
  }
  return true;
}

void TpchQ18State::SetOrder(int64_t orderkey, int64_t custkey,
                            int32_t orderdate_days, int64_t totalprice_raw) {
  TpchQ18OrderRec &rec = MutableRec(orderkey);
  rec.custkey = custkey;
  rec.orderdate_days = orderdate_days;
  rec.totalprice_raw = totalprice_raw;
  rec.flags_lines |= TpchQ18OrderRec::kHasOrder;
}

bool TpchQ18State::ClearOrder(int64_t orderkey) {
  const TpchQ18OrderRec *found = orders.Find(orderkey);
  if (!found || !found->HasOrder())
    return false;
  TpchQ18OrderRec &rec = MutableRec(orderkey);
  rec.custkey = 0;
  rec.orderdate_days = 0;
  rec.totalprice_raw = 0;
  rec.flags_lines &= ~TpchQ18OrderRec::kHasOrder;
  return true;
}

// ── TxnBuffer ───────────────────────────────────────────────────────────────
bool TpchQ18TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ18TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  lineitems.clear();
  orders.clear();
  customers.clear();
}
bool TpchQ18TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
void TpchQ18TxnBuffer::MarkRebuild() {
  pending = true;
  requires_rebuild = true;
  lineitems.clear();
  orders.clear();
  customers.clear();
}
void TpchQ18TxnBuffer::AddLineitem(TpchQ18LineitemDelta delta) {
  pending = true;
  lineitems.push_back(delta);
}
void TpchQ18TxnBuffer::AddOrder(TpchQ18OrderDelta delta) {
  pending = true;
  orders.push_back(std::move(delta));
}
void TpchQ18TxnBuffer::AddCustomer(TpchQ18CustomerDelta delta) {
  pending = true;
  customers.push_back(std::move(delta));
}
const std::vector<TpchQ18LineitemDelta> &TpchQ18TxnBuffer::Lineitems() const {
  return lineitems;
}
const std::vector<TpchQ18OrderDelta> &TpchQ18TxnBuffer::Orders() const {
  return orders;
}
const std::vector<TpchQ18CustomerDelta> &TpchQ18TxnBuffer::Customers() const {
  return customers;
}
uint64_t TpchQ18TxnBuffer::Size() const {
  return lineitems.size() + orders.size() + customers.size();
}

// ── Module ──────────────────────────────────────────────────────────────────
core::AcceleratorId TpchQ18Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ18Module::Dependencies() const {
  return {{config.lineitem_table, {0, 1}, {0, 4}},
          {config.orders_table, {0, 1, 2, 3}, {0, 1, 4, 3}},
          {config.customer_table, {0, 1}, {0, 1}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ18Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ18TxnBuffer>();
}

void TpchQ18Module::Build(core::AuxEngineServices &,
                          core::StateBuilder &builder) {
  const auto typed_state = std::make_shared<const TpchQ18State>(BuildState());
  builder.Write(core::StateImage::Make(
      TpchQ18Layout::kAccelerator, TpchQ18Layout::kOrderQuantitySums, 0,
      MakeStateImage(typed_state)));
  builder.Finish();
}

core::InvalidationPlan
TpchQ18Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table ||
      mutation.table == config.orders_table ||
      mutation.table == config.customer_table) {
    core::InvalidationPlan plan;
    plan.scope =
        ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  return {};
}

// Every write on the three tables is a per-key delta: no lookup in the
// database and no rebuild. A row that cannot be read (NULL key, odd decimal
// scale) is the only thing that sends the batch to a rebuild.
void TpchQ18Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ18TxnBuffer &>(base_buffer);
  const bool paired = mutation.kind == core::MutationKind::Update;
  if (paired && mutation.old_rows.size() != mutation.new_rows.size()) {
    buffer.MarkRebuild();
    return;
  }
  const bool has_old = mutation.kind != core::MutationKind::Insert;
  const bool has_new = mutation.kind != core::MutationKind::Delete;
  const std::size_t count =
      has_old ? mutation.old_rows.size() : mutation.new_rows.size();

  for (std::size_t i = 0; i < count; ++i) {
    const core::AuxRow *old_row = has_old ? &mutation.old_rows[i] : nullptr;
    const core::AuxRow *new_row = has_new ? &mutation.new_rows[i] : nullptr;
    if (mutation.table == config.lineitem_table) {
      auto add = [&](const core::AuxRow *row, int8_t sign) {
        if (!row)
          return true;
        TpchQ18LineitemDelta delta;
        delta.sign = sign;
        if (!ReadLineitem(*row, delta.orderkey, delta.quantity_raw))
          return false;
        buffer.AddLineitem(delta);
        return true;
      };
      if (!add(old_row, -1) || !add(new_row, 1)) {
        buffer.MarkRebuild();
        return;
      }
    } else if (mutation.table == config.orders_table) {
      auto add = [&](const core::AuxRow *row, int8_t sign) {
        if (!row)
          return true;
        TpchQ18OrderDelta delta;
        delta.sign = sign;
        if (!ReadOrder(*row, delta.order))
          return false;
        buffer.AddOrder(std::move(delta));
        return true;
      };
      if (!add(old_row, -1) || !add(new_row, 1)) {
        buffer.MarkRebuild();
        return;
      }
    } else if (mutation.table == config.customer_table) {
      TpchQ18CustomerDelta delta;
      delta.has_old = has_old;
      delta.has_new = has_new;
      TpchQ18CustomerInput old_customer;
      if ((old_row && !ReadCustomer(*old_row, old_customer)) ||
          (new_row && !ReadCustomer(*new_row, delta.new_row))) {
        buffer.MarkRebuild();
        return;
      }
      delta.old_key = old_customer.key;
      buffer.AddCustomer(std::move(delta));
    }
  }
}

core::StagedMaintenanceOutcome
TpchQ18Module::StageMaintenance(core::AuxEngineServices &services,
                                const core::InvalidationPlan &,
                                core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ18TxnBuffer &>(base_buffer);
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

  std::shared_ptr<const TpchQ18State> current;
  if (!LoadState(services, current)) {
    core::FinishRebuildAll(outcome);
    return outcome;
  }
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {TpchQ18Layout::kOrderQuantitySums});
  const core::Version existing_version =
      snapshot.partitions.empty() ? 0 : snapshot.partitions.front().version;

  // Copy-on-write copy: the shard directories are copied, a shard is cloned
  // the first time a delta touches it.
  auto new_state = std::make_shared<TpchQ18State>(*current);

  // orders rows (a delete then an insert of one key is an update).
  for (const auto &delta : buffer.Orders()) {
    const auto &order = delta.order;
    if (delta.sign < 0) {
      if (!new_state->ClearOrder(order.key)) {
        core::FinishRebuildAll(outcome); // state and tables disagree
        return outcome;
      }
    } else {
      new_state->SetOrder(order.key, order.custkey, order.orderdate_days,
                          order.totalprice_raw);
    }
    new_state->Reindex(order.key);
  }
  for (const auto &delta : buffer.Lineitems()) {
    if (!new_state->AddLineitem(delta.orderkey, delta.quantity_raw,
                                delta.sign)) {
      core::FinishRebuildAll(outcome);
      return outcome;
    }
    new_state->Reindex(delta.orderkey);
  }

  // Customer names: every customer is kept, so a result never needs SQL.
  for (const auto &delta : buffer.Customers()) {
    if (delta.has_old && (!delta.has_new || delta.old_key != delta.new_row.key))
      new_state->customer_names.Erase(delta.old_key);
    if (delta.has_new)
      new_state->customer_names.Upsert(delta.new_row.key, delta.new_row.name);
  }

  outcome.action = core::MaintenanceAction::ApplyDelta;
  const auto typed_state =
      std::static_pointer_cast<const TpchQ18State>(std::move(new_state));
  outcome.updates.push_back(core::StateImage::Make(
      TpchQ18Layout::kAccelerator, TpchQ18Layout::kOrderQuantitySums,
      existing_version, MakeStateImage(typed_state)));
  return outcome;
}

bool TpchQ18Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &arguments) const {
  if (arguments.values.empty())
    return false;
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

// Execute cost: O(hot orders + result) for QUANTITY >= 300; below that O(all
// orders) (an in-memory pass, no SQL), the result being every order above it.
void TpchQ18Module::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  const QuantityBound bound = ParseQuantity(arguments);
  if (bound.kind == QuantityBound::Kind::None) {
    EmitEmpty(sink); // SUM(..) > NULL / NaN / +Infinity keeps no order
    return;
  }

  std::shared_ptr<const TpchQ18State> state;
  if (!services.StateStore()
           .CheckValidity({Id(), {}}, services.CurrentVisibility())
           .IsValid() ||
      !LoadState(services, state)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  const int64_t threshold =
      bound.kind == QuantityBound::Kind::All
          ? std::numeric_limits<int64_t>::min()
          : bound.raw;
  std::vector<TpchQ18OutputRow> rows;
  auto consider = [&](int64_t orderkey, const TpchQ18OrderRec &rec) {
    if (!rec.Qualifies(threshold))
      return;
    TpchQ18OutputRow row;
    row.custkey = rec.custkey;
    row.orderkey = orderkey;
    row.orderdate_days = rec.orderdate_days;
    row.totalprice_raw = rec.totalprice_raw;
    row.quantity_sum_raw = rec.sum_raw;
    rows.push_back(std::move(row));
  };
  if (threshold >= TpchQ18State::kHotFloorRaw) {
    state->hot.ForEach([&](int64_t orderkey, const uint8_t &) {
      if (const auto *rec = state->orders.Find(orderkey))
        consider(orderkey, *rec);
    });
  } else {
    state->orders.ForEach(consider);
  }
  EmitRows(std::move(rows), state.get(), sink);
}

// No usable state (a writer in flight, this transaction wrote the tables):
// stream the per-order sums from SQL keeping only the qualifying orders, then
// the orders and the customers of those.
void TpchQ18Module::ExecuteFallback(core::AuxEngineServices &,
                                    const core::QueryArguments &arguments,
                                    core::AuxRowSink &sink) {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q18 aux requires a typed initial scan source");
  const QuantityBound bound = ParseQuantity(arguments);
  if (bound.kind == QuantityBound::Kind::None) {
    EmitEmpty(sink);
    return;
  }
  const int64_t threshold = bound.kind == QuantityBound::Kind::All
                                ? std::numeric_limits<int64_t>::min()
                                : bound.raw;

  std::unordered_map<int64_t, int64_t> qualifying; // orderkey -> sum
  const bool aggregated = initial_scan->ScanOrderQuantityAggregates(
      [&](const TpchQ18OrderQuantityAggregate &agg) {
        if (agg.line_count > 0 && agg.quantity_sum_raw > threshold)
          qualifying[agg.orderkey] = agg.quantity_sum_raw;
      });
  if (!aggregated) {
    std::unordered_map<int64_t, int64_t> sums;
    initial_scan->ScanLineitems([&](const TpchQ18LineitemInput &input) {
      sums[input.orderkey] += input.quantity_raw;
    });
    for (const auto &[orderkey, sum] : sums)
      if (sum > threshold)
        qualifying[orderkey] = sum;
  }

  std::vector<TpchQ18OutputRow> rows;
  if (!qualifying.empty()) {
    initial_scan->ScanOrders([&](const TpchQ18OrderInput &input) {
      const auto it = qualifying.find(input.key);
      if (it == qualifying.end())
        return;
      TpchQ18OutputRow row;
      row.custkey = input.custkey;
      row.orderkey = input.key;
      row.orderdate_days = input.orderdate_days;
      row.totalprice_raw = input.totalprice_raw;
      row.quantity_sum_raw = it->second;
      rows.push_back(std::move(row));
    });
  }
  EmitRows(std::move(rows), nullptr, sink);
}

TpchQ18State TpchQ18Module::BuildState() const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q18 aux requires a typed initial scan source");
  TpchQ18State state;

  // Streamed: every row lands directly in the (sharded) final state.
  initial_scan->ScanOrders([&](const TpchQ18OrderInput &input) {
    state.SetOrder(input.key, input.custkey, input.orderdate_days,
                   input.totalprice_raw);
  });
  const bool aggregated = initial_scan->ScanOrderQuantityAggregates(
      [&](const TpchQ18OrderQuantityAggregate &agg) {
        TpchQ18OrderRec &rec = state.MutableRec(agg.orderkey);
        rec.sum_raw += agg.quantity_sum_raw;
        rec.flags_lines += static_cast<uint32_t>(agg.line_count);
      });
  if (!aggregated) {
    initial_scan->ScanLineitems([&](const TpchQ18LineitemInput &input) {
      state.AddLineitem(input.orderkey, input.quantity_raw, 1);
    });
  }

  // Hot set and the names of its customers.
  std::vector<int64_t> hot_orders;
  state.orders.ForEach([&](int64_t orderkey, const TpchQ18OrderRec &rec) {
    if (!rec.Qualifies(TpchQ18State::kHotFloorRaw))
      return;
    hot_orders.push_back(orderkey);
  });
  for (const int64_t orderkey : hot_orders)
    state.hot.Upsert(orderkey, 1);
  initial_scan->ScanCustomers([&](const TpchQ18CustomerInput &input) {
    state.customer_names.Upsert(input.key, input.name);
  });
  return state;
}

bool TpchQ18Module::LoadState(core::AuxEngineServices &services,
                              std::shared_ptr<const TpchQ18State> &state) const {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(), {TpchQ18Layout::kOrderQuantitySums});
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ18Layout::kOrderQuantitySums) {
      const auto *typed = AsQ18Image(image);
      if (!typed)
        return false;
      state = typed->state;
      return true;
    }
  }
  return false;
}

void TpchQ18Module::EmitRows(std::vector<TpchQ18OutputRow> rows,
                             const TpchQ18State *state,
                             core::AuxRowSink &sink) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q18 aux requires a typed initial scan source");

  // The join with customer: cached names first, the rest by key.
  std::unordered_map<int64_t, std::string> looked_up;
  std::vector<int64_t> missing;
  {
    std::unordered_set<int64_t> seen;
    for (const auto &row : rows) {
      if (state && state->customer_names.Contains(row.custkey))
        continue;
      if (seen.insert(row.custkey).second)
        missing.push_back(row.custkey);
    }
  }
  if (!missing.empty()) {
    initial_scan->LookupCustomerNames(
        missing, [&](const TpchQ18CustomerInput &input) {
          looked_up.emplace(input.key, input.name);
        });
  }

  std::vector<TpchQ18OutputRow> output_rows;
  output_rows.reserve(rows.size());
  for (auto &row : rows) {
    const std::string *name = state ? state->customer_names.Find(row.custkey)
                                    : nullptr;
    if (!name) {
      const auto it = looked_up.find(row.custkey);
      if (it == looked_up.end())
        continue; // no such customer: the inner join drops the order
      name = &it->second;
    }
    row.name = *name;
    output_rows.push_back(std::move(row));
  }
  std::sort(output_rows.begin(), output_rows.end(), BetterOutputRow);

  sink.Begin(core::OutputSchema{});
  for (const auto &row : output_rows) {
    core::AuxRow out;
    out.values.reserve(6);
    out.values.push_back(core::AuxValue::Of(core::AuxType::String, row.name));
    out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, row.custkey));
    out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, row.orderkey));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::Date, core::AuxDate{row.orderdate_days}));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::ScaledDecimal,
                           core::ScaledDecimal{row.totalprice_raw, 2}));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::ScaledDecimal,
                           core::ScaledDecimal{row.quantity_sum_raw, 2}));
    sink.WriteRow(out);
  }
  sink.End();
}

std::vector<core::CalibrationWrite> TpchQ18Module::CalibrationWrites() const {
  return TpchQ18CalibrationWrites();
}

} // namespace accelerator_codegen::aux_state::query
