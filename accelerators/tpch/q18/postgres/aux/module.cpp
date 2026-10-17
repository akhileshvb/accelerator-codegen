#include "tpch/q18/postgres/aux/module.hpp"
#include "tpch/q18/calibration_writes.hpp"
#include "substrate/aux/core/aux_cells.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "substrate/aux/core/partitioned_build.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace accelerator_codegen::aux_state::query {
namespace {
// Lineitem deltas per slice when netting a write batch by order.
constexpr std::size_t kDeltaGrain = 16384;

bool ReadLineitem(const core::AuxRow &row, int64_t &orderkey,
                  int64_t &quantity_raw) {
  if (!core::IntCell(row, 0, orderkey))
    return false;
  int64_t raw = 0;
  int32_t scale = 0;
  if (!core::DecimalCell(row, 1, raw, scale))
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

bool ReadOrder(const core::AuxRow &row, TpchQ18OrderInput &out) {
  if (!core::IntCell(row, 0, out.key) || !core::IntCell(row, 1, out.custkey))
    return false;
  int64_t days = 0;
  if (!core::IntCell(row, 2, days) || days < std::numeric_limits<int32_t>::min() ||
      days > std::numeric_limits<int32_t>::max())
    return false;
  out.orderdate_days = static_cast<int32_t>(days);
  return core::DecimalCellAtScale(row, 3, 2, out.totalprice_raw);
}

bool ReadCustomer(const core::AuxRow &row, TpchQ18CustomerInput &out) {
  return core::IntCell(row, 0, out.key) && core::StringCell(row, 1, out.name);
}

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

// Net effect of a write batch's lineitems on one order.
struct LineitemNet {
  int64_t sum_raw = 0;
  int64_t lines = 0;
};
using LineitemNets = std::unordered_map<int64_t, LineitemNet>;

LineitemNets NetLineitems(const core::AuxEngineServices &services,
                          const std::vector<TpchQ18LineitemDelta> &deltas) {
  LineitemNets nets;
  core::ParallelReduce(
      services, deltas.size(), kDeltaGrain, nets,
      [&](LineitemNets &into, std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
          LineitemNet &net = into[deltas[i].orderkey];
          net.sum_raw += deltas[i].sign * deltas[i].quantity_raw;
          net.lines += deltas[i].sign;
        }
      },
      [](LineitemNets &into, LineitemNets &&from) {
        for (const auto &[orderkey, net] : from) {
          into[orderkey].sum_raw += net.sum_raw;
          into[orderkey].lines += net.lines;
        }
      });
  return nets;
}

} // namespace

void TpchQ18InitialScan::LookupCustomerNames(
    const std::vector<int64_t> &custkeys,
    const std::function<void(const TpchQ18CustomerInput &)> &consumer) {
  const std::unordered_set<int64_t> wanted(custkeys.begin(), custkeys.end());
  ScanCustomersSerial([&](const TpchQ18CustomerInput &input) {
    if (wanted.find(input.key) != wanted.end())
      consumer(input);
  });
}

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

bool TpchQ18State::AddLineitems(int64_t orderkey, int64_t sum_delta_raw,
                                int64_t lines_delta) {
  TpchQ18OrderRec &rec = MutableRec(orderkey);
  const int64_t lines = static_cast<int64_t>(rec.Lines()) + lines_delta;
  if (lines < 0 || lines > static_cast<int64_t>(~TpchQ18OrderRec::kHasOrder))
    return false;
  rec.sum_raw += sum_delta_raw;
  rec.flags_lines = (rec.flags_lines & TpchQ18OrderRec::kHasOrder) |
                    static_cast<uint32_t>(lines);
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

core::AcceleratorId TpchQ18Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ18Module::Dependencies() const {
  return {{config.lineitem_table, {0, 1}, {0, 4}},
          {config.orders_table, {0, 1, 2, 3}, {0, 1, 4, 3}},
          {config.customer_table, {0, 1}, {0, 1}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ18Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ18TxnBuffer>();
}

void TpchQ18Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  const auto typed_state =
      std::make_shared<const TpchQ18State>(BuildState(services));
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
    core::FinishRebuildAll(outcome, services.ChooseMaintenance(core::MaintenanceRequest::RebuildOnly(
                                        services.ExpectedQueriesBeforeNextWrite())));
    return outcome;
  }
  if (buffer.Size() == 0)
    return outcome;

  const auto decision = services.ChooseMaintenance(core::MaintenanceRequest::IncrementalSingleState(
      buffer.Size(), services.ExpectedQueriesBeforeNextWrite()));
  if (decision.action != core::MaintenanceAction::ApplyDelta) {
    core::FinishRebuildAll(outcome, decision);
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
  // Lineitems are netted per order in parallel slices; the COW state is
  // edited serially (a shard clone is the cost, and shards are not shareable).
  for (const auto &[orderkey, net] : NetLineitems(services, buffer.Lineitems())) {
    if (net.sum_raw == 0 && net.lines == 0)
      continue;
    if (!new_state->AddLineitems(orderkey, net.sum_raw, net.lines)) {
      core::FinishRebuildAll(outcome);
      return outcome;
    }
    new_state->Reindex(orderkey);
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
  EmitRows(std::move(rows), *state, sink);
}

namespace {
struct OrderQuantity {
  int64_t orderkey;
  int64_t quantity_raw;
};

// The build's rows of the orders whose key falls into one bucket.
struct BuildBucket {
  std::vector<OrderQuantity> lineitems;
  std::vector<TpchQ18OrderInput> orders;
};

struct BucketRecords {
  std::vector<std::pair<int64_t, TpchQ18OrderRec>> records;
  std::vector<int64_t> hot;
};

// The record of every order of the bucket (its orders row, SUM(l_quantity) and
// COUNT(*) of its lineitems) and which of them are hot.
BucketRecords BuildRecords(BuildBucket &bucket) {
  std::sort(bucket.lineitems.begin(), bucket.lineitems.end(),
            [](const OrderQuantity &a, const OrderQuantity &b) {
              return a.orderkey < b.orderkey;
            });
  std::stable_sort(bucket.orders.begin(), bucket.orders.end(),
                   [](const TpchQ18OrderInput &a, const TpchQ18OrderInput &b) {
                     return a.key < b.key;
                   });
  BucketRecords out;
  auto line = bucket.lineitems.begin();
  auto order = bucket.orders.begin();
  while (line != bucket.lineitems.end() || order != bucket.orders.end()) {
    constexpr int64_t kEnd = std::numeric_limits<int64_t>::max();
    const int64_t key = std::min(line != bucket.lineitems.end() ? line->orderkey : kEnd,
                                 order != bucket.orders.end() ? order->key : kEnd);
    TpchQ18OrderRec rec;
    for (; line != bucket.lineitems.end() && line->orderkey == key; ++line) {
      rec.sum_raw += line->quantity_raw;
      ++rec.flags_lines;
    }
    for (; order != bucket.orders.end() && order->key == key; ++order) {
      rec.custkey = order->custkey; // a repeated key keeps the last row
      rec.orderdate_days = order->orderdate_days;
      rec.totalprice_raw = order->totalprice_raw;
      rec.flags_lines |= TpchQ18OrderRec::kHasOrder;
    }
    out.records.emplace_back(key, rec);
    if (rec.Qualifies(TpchQ18State::kHotFloorRaw))
      out.hot.push_back(key);
  }
  return out;
}

} // namespace

TpchQ18State TpchQ18Module::BuildState(core::AuxEngineServices &services) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q18 aux requires a typed initial scan source");
  TpchQ18State state;

  // The scans only deal the rows out to buckets by order; the buckets are
  // joined and summed in parallel slices and then merged into the state.
  const auto bucket_of = [](int64_t orderkey) {
    return static_cast<uint64_t>(orderkey) % core::kBuildBuckets;
  };
  const std::size_t partitions = core::BuildPartitions(services);
  auto orders = core::PartitionedBuild<core::Buckets<TpchQ18OrderInput>>(
      services, [] { return core::Buckets<TpchQ18OrderInput>(core::kBuildBuckets); },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanOrders(
            services, parts, [&](std::size_t partition, const TpchQ18OrderInput &input) {
              sink(partition, core::SingleRowBatch<TpchQ18OrderInput>{input});
            });
      },
      [&](core::Buckets<TpchQ18OrderInput> &into, const TpchQ18OrderInput &input) {
        into[bucket_of(input.key)].push_back(input);
      },
      core::AppendBuckets<TpchQ18OrderInput>, partitions);
  auto lineitems = core::PartitionedBuild<core::Buckets<OrderQuantity>>(
      services, [] { return core::Buckets<OrderQuantity>(core::kBuildBuckets); },
      [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanLineitems(
            services, parts, [&](std::size_t partition, const TpchQ18LineitemInput &input) {
              sink(partition, core::SingleRowBatch<TpchQ18LineitemInput>{input});
            });
      },
      [&](core::Buckets<OrderQuantity> &into, const TpchQ18LineitemInput &input) {
        into[bucket_of(input.orderkey)].push_back({input.orderkey, input.quantity_raw});
      },
      core::AppendBuckets<OrderQuantity>, partitions);
  std::vector<BuildBucket> buckets(core::kBuildBuckets);
  for (std::size_t b = 0; b < core::kBuildBuckets; ++b) {
    buckets[b].orders = std::move(orders[b]);
    buckets[b].lineitems = std::move(lineitems[b]);
  }
  std::vector<BucketRecords> built(core::kBuildBuckets);
  core::ParallelEach(services, core::kBuildBuckets, 1, [&](std::size_t i) {
    built[i] = BuildRecords(buckets[i]);
    buckets[i] = {};
  });
  for (const auto &bucket : built) {
    for (const auto &[orderkey, rec] : bucket.records)
      state.MutableRec(orderkey) = rec;
    for (const int64_t orderkey : bucket.hot)
      state.hot.Upsert(orderkey, 1);
  }
  // Names are decoded in partitions; the COW name map is filled serially.
  const auto customers = core::CollectRows<TpchQ18CustomerInput>(
      services, [&](std::size_t parts, auto &&sink) {
        initial_scan->ScanCustomers(services, parts, sink);
      });
  for (const auto &customer : customers)
    state.customer_names.Upsert(customer.key, customer.name);
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
                             const TpchQ18State &state,
                             core::AuxRowSink &sink) const {
  if (!initial_scan)
    throw std::logic_error("TPC-H Q18 aux requires a typed initial scan source");

  // The join with customer: cached names first, the rest by key.
  std::unordered_map<int64_t, std::string> looked_up;
  std::vector<int64_t> missing;
  {
    std::unordered_set<int64_t> seen;
    for (const auto &row : rows) {
      if (state.customer_names.Contains(row.custkey))
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
    const std::string *name = state.customer_names.Find(row.custkey);
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
