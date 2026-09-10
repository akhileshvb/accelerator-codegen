#include "tpch/q13/postgres/aux/module.hpp"
#include "accelerator_codegen/stateful/core/byte_stream.hpp"

#include <algorithm>
#include <functional>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <chrono>
#include <cstdio>

// ── Custom profiling (no-op unless ACCEL_AUX_CUSTOM_PROFILE is set) ─────────
struct CustomTimer {
  using Clock = std::chrono::high_resolution_clock;
  Clock::time_point start;
  bool active;
  explicit CustomTimer(bool enabled) : start(Clock::now()), active(enabled) {}
  double us() const {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - start)
            .count()) /
           1000.0;
  }
};

static bool CustomProfilingEnabled() {
  static const bool enabled = []() {
    const char *v = std::getenv("ACCEL_AUX_CUSTOM_PROFILE");
    return v && v[0] != '\0';
  }();
  return enabled;
}

static void EmitCustomProfile(const char *phase, double us_val, int64_t rows,
                              int64_t keys, int64_t groups, int64_t scanned) {
  if (!CustomProfilingEnabled())
    return;
  fprintf(stderr,
          "ACCEL_CUSTOM_PROFILE {"
          "\"phase\":\"%s\","
          "\"us\":%.3f,"
          "\"rows\":%ld,"
          "\"keys\":%ld,"
          "\"groups\":%ld,"
          "\"scanned\":%ld"
          "}\n",
          phase, us_val, (long)rows, (long)keys, (long)groups, (long)scanned);
}

namespace accelerator_codegen::aux_state::query {
namespace {

using core::ByteReader;
using core::ByteWriter;

constexpr uint32_t kCustomerMagic = 0x43313351; // Q13C
constexpr uint32_t kVersion = 1;

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

// Recursive SQL LIKE matcher. Pattern is kept in original case.
bool SqlLikeAt(const char *text, std::size_t text_len,
               const char *pattern, std::size_t pattern_len,
               std::size_t text_pos, std::size_t pattern_pos) {
  while (pattern_pos < pattern_len) {
    const char pattern_ch = pattern[pattern_pos];
    if (pattern_ch == '%') {
      if (pattern_pos + 1 == pattern_len)
        return true;
      for (std::size_t start = text_pos; start <= text_len; ++start) {
        if (SqlLikeAt(text, text_len, pattern, pattern_len, start, pattern_pos + 1))
          return true;
      }
      return false;
    }
    if (pattern_ch == '_') {
      if (text_pos >= text_len)
        return false;
      ++text_pos;
      ++pattern_pos;
      continue;
    }
    if (text_pos >= text_len || text[text_pos] != pattern_ch)
      return false;
    ++text_pos;
    ++pattern_pos;
  }
  return text_pos == text_len;
}

bool SqlLike(const std::string &text, const std::string &pattern) {
  return SqlLikeAt(text.data(), text.size(), pattern.data(), pattern.size(), 0, 0);
}

bool SqlLikeSlice(const char *text, std::size_t text_len, const std::string &pattern) {
  return SqlLikeAt(text, text_len, pattern.data(), pattern.size(), 0, 0);
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope = ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

std::shared_ptr<const TpchQ13StateImage>
MakeStateImage(TpchQ13ImageKind kind,
               const std::shared_ptr<const TpchQ13State> &state) {
  auto image = std::make_shared<TpchQ13StateImage>();
  image->kind = kind;
  image->state = state;
  return image;
}

const TpchQ13StateImage *AsQ13Image(const core::StateImage &image,
                                    TpchQ13ImageKind kind) {
  const auto *payload = image.As<TpchQ13StateImage>();
  return payload && payload->kind == kind && payload->state ? payload : nullptr;
}

bool Q13OutputRowBetter(const TpchQ13OutputRow &lhs,
                        const TpchQ13OutputRow &rhs) {
  if (lhs.custdist != rhs.custdist)
    return lhs.custdist > rhs.custdist;
  return lhs.c_count > rhs.c_count;
}

} // namespace

// ── TwoPartLikeMatcher ──────────────────────────────────────────────────────
void TpchQ13TwoPartLikeMatcher::Build(const std::string &pattern) {
  enabled = false;
  part1.clear();
  part2.clear();
  if (pattern.size() < 4 || pattern.front() != '%' || pattern.back() != '%')
    return;
  const std::size_t first_end = pattern.find('%', 1);
  if (first_end == std::string::npos || first_end == 1)
    return;
  const std::size_t second_start = first_end + 1;
  const std::size_t second_end = pattern.find('%', second_start);
  if (second_end == std::string::npos || second_end == second_start)
    return;
  if (pattern.find('%', second_end + 1) != std::string::npos)
    return;
  part1 = pattern.substr(1, first_end - 1);
  part2 = pattern.substr(second_start, second_end - second_start);
  enabled = !part1.empty() && !part2.empty();
}

bool TpchQ13TwoPartLikeMatcher::Matches(const std::string &text) const {
  if (!enabled)
    return false;
  const std::string_view haystack(text.data(), text.size());
  std::size_t pos = 0;
  while (pos + part1.size() <= haystack.size()) {
    const auto found = haystack.find(part1, pos);
    if (found == std::string_view::npos)
      return false;
    if (haystack.find(part2, found + part1.size()) != std::string_view::npos)
      return true;
    pos = found + 1;
  }
  return false;
}

bool TpchQ13TwoPartLikeMatcher::MatchesView(const char *data, std::size_t len) const {
  if (!enabled)
    return false;
  const std::string_view haystack(data, len);
  std::size_t pos = 0;
  while (pos + part1.size() <= haystack.size()) {
    const auto found = haystack.find(part1, pos);
    if (found == std::string_view::npos)
      return false;
    if (haystack.find(part2, found + part1.size()) != std::string_view::npos)
      return true;
    pos = found + 1;
  }
  return false;
}

// ── TpchQ13State implementation ─────────────────────────────────────────────
void TpchQ13State::AddCustomer(int64_t custkey) {
  if (custkey < 0)
    return;
  const auto required = static_cast<std::size_t>(custkey) + 1;
  if (required > order_counts.size()) {
    order_counts.resize(required, 0);
    customer_valid.resize(required, 0);
  }
  if (customer_valid[static_cast<std::size_t>(custkey)] == 0) {
    customer_valid[static_cast<std::size_t>(custkey)] = 1;
    active_customers.push_back(custkey);
  }
  if (static_cast<int64_t>(custkey) > max_custkey)
    max_custkey = custkey;
}

void TpchQ13State::RemoveCustomer(int64_t custkey) {
  if (custkey < 0 || static_cast<std::size_t>(custkey) >= customer_valid.size())
    return;
  if (customer_valid[static_cast<std::size_t>(custkey)] == 0)
    return;
  customer_valid[static_cast<std::size_t>(custkey)] = 0;
  order_counts[static_cast<std::size_t>(custkey)] = 0;
  active_customers.erase(
      std::remove(active_customers.begin(), active_customers.end(), custkey),
      active_customers.end());
}

void TpchQ13State::AddOrder(int64_t custkey, const std::string &comment) {
  if (custkey < 0) return;
  AddCustomer(custkey);
  ++order_counts[static_cast<std::size_t>(custkey)];
  order_custkeys.push_back(custkey);
  order_comment_start.push_back(static_cast<uint32_t>(comments_blob.size()));
  order_comment_len.push_back(static_cast<uint32_t>(comment.size()));
  comments_blob.append(comment);
  order_alive.push_back(1);
}

void TpchQ13State::RemoveOrder(int64_t custkey, const std::string &comment) {
  if (custkey < 0 || static_cast<std::size_t>(custkey) >= order_counts.size()) return;
  if (order_counts[static_cast<std::size_t>(custkey)] > 0)
    --order_counts[static_cast<std::size_t>(custkey)];
  for (std::size_t i = order_custkeys.size(); i > 0; --i) {
    const std::size_t idx = i - 1;
    if (!order_alive[idx]) continue;
    if (order_custkeys[idx] != custkey) continue;
    const char *cp = comments_blob.data() + order_comment_start[idx];
    if (comment.size() != order_comment_len[idx] ||
        std::memcmp(cp, comment.data(), comment.size()) != 0)
      continue;
    order_alive[idx] = 0;
    return;
  }
}

void TpchQ13State::RebuildTrigramIndex() {
  trigram_index_built = false;
  trigram_index_pending = 0;
  trigram_index_keys.clear();
  trigram_index_offsets.clear();
  trigram_index_rows.clear();
  trigram_index_lookup.clear();

  struct TrigramEntry {
    uint32_t hash;
    uint32_t order_idx;
    bool operator<(const TrigramEntry &o) const {
      return hash < o.hash || (hash == o.hash && order_idx < o.order_idx);
    }
  };

  // For each comment, extract every length-3 substring (trigram) and index by
  // hash.  This guarantees any LIKE pattern %a%b% with literal parts >= 3 chars
  // is captured because at query time we intersect the trigram lists for one
  // trigram each from part1 and part2, then verify survivors with full LIKE.
  // Unlike word-level indexing, this correctly handles substring matches
  // inside longer words (e.g. "special" within "especially").
  //
  // Per-order dedup happens before the global sort.  Comments are short and
  // have many repeated trigrams, so this roughly halves the sort input.
  std::vector<TrigramEntry> entries;
  entries.reserve(trigram_index_rows.capacity() / 2 + 128);
  const std::size_t oc = order_custkeys.size();
  for (std::size_t idx = 0; idx < oc; ++idx) {
    if (!order_alive[idx]) continue;
    const char *cp = comments_blob.data() + order_comment_start[idx];
    const std::size_t cl = order_comment_len[idx];
    if (cl < 3) continue;
    uint32_t last_hash = UINT32_MAX;
    for (std::size_t i = 0; i + 3 <= cl; ++i) {
      uint32_t h = 5381;
      h = ((h << 5) + h) + static_cast<uint8_t>(cp[i]);
      h = ((h << 5) + h) + static_cast<uint8_t>(cp[i + 1]);
      h = ((h << 5) + h) + static_cast<uint8_t>(cp[i + 2]);
      if (h != last_hash) {
        last_hash = h;
        entries.push_back({h, static_cast<uint32_t>(idx)});
      }
    }
  }

  if (entries.empty()) return;

  std::sort(entries.begin(), entries.end());

  // Compact into (key, offset, rows).
  uint32_t prev_hash = entries[0].hash;
  uint32_t last_idx = entries[0].order_idx;
  trigram_index_keys.push_back(prev_hash);
  trigram_index_offsets.push_back(0);
  trigram_index_rows.push_back(last_idx);

  for (size_t ei = 1; ei < entries.size(); ++ei) {
    const auto &e = entries[ei];
    if (e.hash != prev_hash) {
      trigram_index_keys.push_back(e.hash);
      trigram_index_offsets.push_back(static_cast<uint64_t>(trigram_index_rows.size()));
      prev_hash = e.hash;
      last_idx = e.order_idx;
      trigram_index_rows.push_back(last_idx);
    } else {
      if (e.order_idx != last_idx) {
        last_idx = e.order_idx;
        trigram_index_rows.push_back(last_idx);
      }
    }
  }

  // Build the hash table used by the query probe to avoid binary search over
  // the distinct-trigram key array.
  trigram_index_lookup.reserve(trigram_index_keys.size());
  for (std::size_t i = 0; i < trigram_index_keys.size(); ++i)
    trigram_index_lookup.emplace(trigram_index_keys[i], i);

  trigram_index_order_count = oc;
  trigram_index_built = true;
}


// ── Encode / Decode ─────────────────────────────────────────────────────────
std::vector<uint8_t> EncodeTpchQ13CustomerCounts(const TpchQ13State &state) {
  ByteWriter writer;
  core::WriteImageHeader(writer, kCustomerMagic, kVersion,
                         static_cast<uint32_t>(state.active_customers.size()));

  writer.I64(state.max_custkey);
  writer.U64(static_cast<uint64_t>(state.order_counts.size()));
  writer.U64(static_cast<uint64_t>(state.customer_valid.size()));

  for (const int32_t count : state.order_counts)
    writer.I32(count);
  for (const uint8_t valid : state.customer_valid)
    writer.U8(valid);

  writer.U64(static_cast<uint64_t>(state.active_customers.size()));
  for (const int64_t custkey : state.active_customers)
    writer.I64(custkey);
  return writer.bytes;
}

bool DecodeTpchQ13CustomerCounts(const std::vector<uint8_t> &bytes,
                                 TpchQ13State &state) {
  ByteReader reader(bytes);
  uint32_t magic = 0, version = 0, record_count = 0;
  if (!core::ReadImageHeader(reader, magic, version, record_count) ||
      magic != kCustomerMagic)
    return false;
  (void)record_count;

  int64_t max_custkey = 0;
  uint64_t order_size = 0;
  uint64_t valid_size = 0;
  if (!reader.I64(max_custkey) || !reader.U64(order_size) || !reader.U64(valid_size))
    return false;

  state.max_custkey = max_custkey;
  state.order_counts.clear();
  state.order_counts.reserve(static_cast<std::size_t>(order_size));
  for (uint64_t i = 0; i < order_size; ++i) {
    int32_t count = 0;
    if (!reader.I32(count))
      return false;
    state.order_counts.push_back(count);
  }

  state.customer_valid.clear();
  state.customer_valid.reserve(static_cast<std::size_t>(valid_size));
  for (uint64_t i = 0; i < valid_size; ++i) {
    uint8_t valid = 0;
    if (!reader.U8(valid))
      return false;
    state.customer_valid.push_back(valid);
  }

  uint64_t active_size = 0;
  if (!reader.U64(active_size))
    return false;
  state.active_customers.clear();
  state.active_customers.reserve(static_cast<std::size_t>(active_size));
  for (uint64_t i = 0; i < active_size; ++i) {
    int64_t custkey = 0;
    if (!reader.I64(custkey))
      return false;
    state.active_customers.push_back(custkey);
  }
  return true;
}

// ── TxnBuffer ───────────────────────────────────────────────────────────────
bool TpchQ13TxnBuffer::HasPendingChanges() const { return pending; }
void TpchQ13TxnBuffer::Clear() {
  pending = false;
  requires_rebuild = false;
  deltas.clear();
}
bool TpchQ13TxnBuffer::RequiresRebuild() const { return requires_rebuild; }
const std::vector<TpchQ13Delta> &TpchQ13TxnBuffer::Deltas() const { return deltas; }
void TpchQ13TxnBuffer::MarkRebuild() {
  requires_rebuild = true;
  pending = true;
}
void TpchQ13TxnBuffer::AddDelta(TpchQ13Delta delta) {
  pending = true;
  deltas.push_back(std::move(delta));
}

// ── TpchQ13Module ───────────────────────────────────────────────────────────
core::AcceleratorId TpchQ13Module::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ13Module::Dependencies() const {
  return {
      {config.customer_table, {0}},   // c_custkey is col 0
      {config.orders_table, {0, 1}},  // o_custkey, o_comment
  };
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ13Module::CreateTxnBuffer() const {
  return std::make_unique<TpchQ13TxnBuffer>();
}

void TpchQ13Module::Build(core::AuxEngineServices &services,
                          core::StateBuilder &builder) {
  const auto state = std::make_shared<TpchQ13State>(BuildState(services));
  builder.Write(core::StateImage::Make(
      kId, core::TpchQ13Layout::kCustomerCounts, 0,
      MakeStateImage(TpchQ13ImageKind::CustomerCounts, state)));
  builder.Finish();
}

void TpchQ13Module::EnsureCustomerCapacity(TpchQ13State &state,
                                           int64_t custkey) const {
  if (custkey < 0)
    return;
  const auto required = static_cast<std::size_t>(custkey) + 1;
  if (required > state.order_counts.size()) {
    state.order_counts.resize(required, 0);
    state.customer_valid.resize(required, 0);
  }
}

TpchQ13State TpchQ13Module::BuildState(
    core::AuxEngineServices &services) const {
  (void)services;
  TpchQ13State state;

  if (!initial_scan)
    throw std::logic_error("TPC-H Q13 requires a typed initial scan source");

  const bool profiling = CustomProfilingEnabled();

  CustomTimer t_cust(profiling);
  initial_scan->ScanCustomers([&](const TpchQ13CustomerInput &input) {
    state.AddCustomer(input.key);
  });
  if (profiling) EmitCustomProfile("build_customers", t_cust.us(),
      (int64_t)state.active_customers.size(), 0, 0, 0);

  CustomTimer t_orders(profiling);
  initial_scan->ScanOrders([&](const TpchQ13OrderInput &input) {
    state.AddOrder(input.custkey, input.comment);
  });
  if (profiling) EmitCustomProfile("build_orders", t_orders.us(),
      (int64_t)state.order_custkeys.size(),
      (int64_t)state.comments_blob.size(), 0, 0);

  // Do not build the full inverted trigram index: at SF1 it materializes
  // ~tens of millions of postings (hundreds of MB) for a modest hit-path win.
  // EmitResult falls back to the linear TwoPartLikeMatcher scan over comments.
  // Comments themselves stay in-state: rescanning orders via InitialScan during
  // Execute re-enters the same DuckDB connection and deadlocks.
  state.trigram_index_built = false;
  state.trigram_index_pending = 0;
  state.trigram_index_order_count = 0;
  state.trigram_index_keys.clear();
  state.trigram_index_offsets.clear();
  state.trigram_index_rows.clear();
  state.trigram_index_lookup.clear();

  return state;
}

core::InvalidationPlan
TpchQ13Module::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.customer_table)
    return All();
  if (mutation.table == config.orders_table)
    return All();
  return {};
}

void TpchQ13Module::AccumulateMutation(const core::MutationBatch &mutation,
                                       core::AuxEngineServices &,
                                       core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ13TxnBuffer &>(base_buffer);

  if (mutation.table == config.customer_table) {
    // Customer insert/delete can be applied as a delta, but conservative
    // rebuild is safer for the flat vector resize path.
    buffer.MarkRebuild();
    return;
  }
  if (mutation.table != config.orders_table)
    return;

  auto add = [&](const core::AuxRow &row, int8_t sign) {
    int64_t custkey = 0;
    std::string comment;
    if (!IntCell(row, 0, custkey) || !StringCell(row, 1, comment)) {
      buffer.MarkRebuild();
      return;
    }
    if (custkey < 0)
      return;
    buffer.AddDelta({sign, custkey, comment});
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

core::StagedMaintenanceOutcome
TpchQ13Module::StageMaintenance(core::AuxEngineServices &services,
                                const core::InvalidationPlan &,
                                core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ13TxnBuffer &>(base_buffer);
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
  if (buffer.Deltas().empty())
    return outcome;

  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(buffer.Deltas().size()), 1, true, false, true,
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

  const auto visibility = services.CurrentVisibility();
  const auto snapshot = services.StateStore().ReadPartitions(
      visibility, {core::TpchQ13Layout::kCustomerCounts});

  bool found = false;
  std::shared_ptr<TpchQ13State> updated_state;
  core::Version existing_version = 0;

  for (const auto &image : snapshot.partitions) {
    if (image.partition != core::TpchQ13Layout::kCustomerCounts)
      continue;
    const auto *typed = AsQ13Image(image, TpchQ13ImageKind::CustomerCounts);
    if (!typed)
      return outcome;
    updated_state = std::make_shared<TpchQ13State>(*typed->state);
    existing_version = image.version;
    found = true;
    break;
  }

  if (!found)
    return outcome;

  const std::size_t delta_count = buffer.Deltas().size();
  for (const auto &delta : buffer.Deltas()) {
    if (delta.sign > 0)
      updated_state->AddOrder(delta.custkey, delta.comment);
    else
      updated_state->RemoveOrder(delta.custkey, delta.comment);
  }

  // Rebuilding the trigram index is a 30s+ full-table sort at SF1 and scales
  // with the entire order set.  For small point deltas the indexed probe is
  // not worth this maintenance cliff: keep the already-built index but mark it
  // stale so the query path uses the still-correct O(orders) matcher scan.
  updated_state->trigram_index_pending += static_cast<uint64_t>(delta_count);

  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.updates.push_back(core::StateImage::Make(
      kId, core::TpchQ13Layout::kCustomerCounts, existing_version,
      MakeStateImage(TpchQ13ImageKind::CustomerCounts,
                     std::static_pointer_cast<const TpchQ13State>(updated_state))));
  return outcome;
}

bool TpchQ13Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ13Module::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  std::string pattern;
  if (!arguments.values.empty() && !arguments.values[0].is_null &&
      std::holds_alternative<std::string>(arguments.values[0].payload))
    pattern = std::get<std::string>(arguments.values[0].payload);

  if (!services.StateStore()
           .CheckValidity({Id(), {}}, services.CurrentVisibility())
           .IsValid()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  std::shared_ptr<const TpchQ13State> state;
  if (!LoadState(services, state)) {
    ExecuteFallback(services, arguments, sink);
    return;
  }

  EmitResult(*state, pattern, services, sink);
}

void TpchQ13Module::ExecuteFallback(core::AuxEngineServices &services,
                                    const core::QueryArguments &arguments,
                                    core::AuxRowSink &sink) {
  std::string pattern;
  if (!arguments.values.empty() && !arguments.values[0].is_null &&
      std::holds_alternative<std::string>(arguments.values[0].payload))
    pattern = std::get<std::string>(arguments.values[0].payload);

  const auto state = BuildState(services);
  EmitResult(state, pattern, services, sink);
}

bool TpchQ13Module::LoadState(
    core::AuxEngineServices &services,
    std::shared_ptr<const TpchQ13State> &state) const {
  const auto snapshot = services.StateStore().ReadPartitions(
      services.CurrentVisibility(),
      {core::TpchQ13Layout::kCustomerCounts});
  for (const auto &image : snapshot.partitions) {
    if (image.partition != core::TpchQ13Layout::kCustomerCounts)
      continue;
    const auto *typed = AsQ13Image(image, TpchQ13ImageKind::CustomerCounts);
    if (!typed)
      return false;
    state = typed->state;
    return true;
  }
  return false;
}

void TpchQ13Module::EmitResultIndexed(
    const TpchQ13State &state,
    const TpchQ13TwoPartLikeMatcher &two_part,
    core::AuxEngineServices &services,
    core::AuxRowSink &sink) const {
  (void)services;
  const bool profiling = CustomProfilingEnabled();

  const std::size_t cc = state.customer_valid.size();
  const bool short_literal = (two_part.part1.size() < 3 || two_part.part2.size() < 3);
  if (short_literal) {
    // Cannot filter by trigrams when a literal is too short.  Run the exact
    // O(orders) LIKE scan inline to preserve correctness.
    CustomTimer t_exclude(profiling);
    std::vector<int32_t> excluded(cc, 0);
    int64_t excluded_count = 0;
    const std::size_t oc = state.order_custkeys.size();
    for (std::size_t idx = 0; idx < oc; ++idx) {
      if (!state.order_alive[idx]) continue;
      const int64_t ck = state.order_custkeys[idx];
      if (ck < 0 || static_cast<std::size_t>(ck) >= cc) continue;
      const char *cp = state.comments_blob.data() + state.order_comment_start[idx];
      const std::size_t cl = state.order_comment_len[idx];
      if (!two_part.MatchesView(cp, cl)) continue;
      ++excluded[static_cast<std::size_t>(ck)];
      ++excluded_count;
    }
    if (profiling) EmitCustomProfile("exclude_scan_short", t_exclude.us(),
        (int64_t)oc, excluded_count, (int64_t)cc, 0);

    // Counting histogram: max_eff ≪ ac, no sort needed.
    CustomTimer t_hist(profiling);
    const std::size_t ac = state.active_customers.size();
    int32_t max_eff = 0;
    for (std::size_t i = 0; i < ac; ++i) {
      const int64_t ck = state.active_customers[i];
      if (ck < 0 || static_cast<std::size_t>(ck) >= cc) continue;
      int32_t eff = state.order_counts[static_cast<std::size_t>(ck)] -
                    excluded[static_cast<std::size_t>(ck)];
      if (eff < 0) eff = 0;
      if (eff > max_eff) max_eff = eff;
    }
    std::vector<int64_t> buckets(static_cast<std::size_t>(max_eff) + 1, 0);
    for (std::size_t i = 0; i < ac; ++i) {
      const int64_t ck = state.active_customers[i];
      if (ck < 0 || static_cast<std::size_t>(ck) >= cc) continue;
      int32_t eff = state.order_counts[static_cast<std::size_t>(ck)] -
                    excluded[static_cast<std::size_t>(ck)];
      if (eff < 0) eff = 0;
      ++buckets[static_cast<std::size_t>(eff)];
    }
    std::vector<TpchQ13OutputRow> rows;
    rows.reserve(buckets.size());
    for (int32_t eff = 0; eff <= max_eff; ++eff) {
      if (buckets[static_cast<std::size_t>(eff)] > 0)
        rows.push_back({static_cast<int64_t>(eff), buckets[static_cast<std::size_t>(eff)]});
    }
    if (profiling) EmitCustomProfile("histogram_short", t_hist.us(),
        (int64_t)ac, (int64_t)rows.size(), (int64_t)max_eff, 0);
    std::sort(rows.begin(), rows.end(), Q13OutputRowBetter);
    sink.Begin({{{"c_count", core::AuxType::Int64, false},
                 {"custdist", core::AuxType::Int64, false}}});
    for (const auto &r : rows)
      sink.WriteRow({{core::AuxValue::Of(core::AuxType::Int64, r.c_count),
                      core::AuxValue::Of(core::AuxType::Int64, r.custdist)}});
    sink.End();
    return;
  }

  CustomTimer t_probe(profiling);
  std::vector<int32_t> excluded(cc, 0);
  int64_t excluded_count = 0;
  int64_t candidates = 0;

  CustomTimer t_tail(profiling);
  const std::size_t oc = state.order_custkeys.size();
  const std::size_t tail_start = std::min(oc, state.trigram_index_order_count);
  for (std::size_t idx = tail_start; idx < oc; ++idx) {
    if (!state.order_alive[idx]) continue;
    const int64_t ck = state.order_custkeys[idx];
    if (ck < 0 || static_cast<std::size_t>(ck) >= cc) continue;
    const char *cp = state.comments_blob.data() + state.order_comment_start[idx];
    const std::size_t cl = state.order_comment_len[idx];
    if (!two_part.MatchesView(cp, cl)) continue;
    ++excluded[static_cast<std::size_t>(ck)];
    ++excluded_count;
  }
  if (profiling) EmitCustomProfile("tail_scan_indexed", t_tail.us(),
      (int64_t)(oc - tail_start), excluded_count, (int64_t)cc, 0);

  const auto &offs = state.trigram_index_offsets;
  const auto &idx_rows = state.trigram_index_rows;
  const auto &lookup_map = state.trigram_index_lookup;

  auto trigram_hash = [](const char *p) {
    uint32_t h = 5381;
    h = ((h << 5) + h) + static_cast<uint8_t>(p[0]);
    h = ((h << 5) + h) + static_cast<uint8_t>(p[1]);
    h = ((h << 5) + h) + static_cast<uint8_t>(p[2]);
    return h;
  };

  // Return [start, end) range into idx_rows for a trigram hash.
  auto trigram_range = [&](uint32_t h) -> std::pair<std::size_t, std::size_t> {
    auto it = lookup_map.find(h);
    if (it == lookup_map.end()) return {0, 0};
    const std::size_t idx = it->second;
    const std::size_t start = static_cast<std::size_t>(offs[idx]);
    const std::size_t end = (idx + 1 < offs.size())
        ? static_cast<std::size_t>(offs[idx + 1])
        : idx_rows.size();
    return {start, end};
  };

  // Pick the rarest trigram from a literal part.
  auto select_rarest_range = [&](const std::string &part) -> std::pair<std::size_t, std::size_t> {
    auto best = trigram_range(trigram_hash(part.data()));
    if (part.size() > 3) {
      for (std::size_t pos = 1; pos + 3 <= part.size(); ++pos) {
        auto cur = trigram_range(trigram_hash(part.data() + pos));
        if ((cur.second - cur.first) < (best.second - best.first))
          best = cur;
      }
    }
    return best;
  };

  auto r1 = select_rarest_range(two_part.part1);
  auto r2 = select_rarest_range(two_part.part2);
  candidates = std::min((int64_t)(r1.second - r1.first), (int64_t)(r2.second - r2.first));

  // Intersect directly from source arrays — no copy into temporary vectors.
  if (r1.second > r1.first && r2.second > r2.first) {
    std::size_t i1 = r1.first, i2 = r2.first;
    std::size_t e1 = r1.second, e2 = r2.second;
    while (i1 < e1 && i2 < e2) {
      uint32_t v1 = idx_rows[i1], v2 = idx_rows[i2];
      if (v1 < v2) { ++i1; continue; }
      if (v2 < v1) { ++i2; continue; }
      const uint32_t idx = v1;
      ++i1; ++i2;
      if (idx >= state.order_alive.size() || !state.order_alive[idx])
        continue;
      const char *cp = state.comments_blob.data() + state.order_comment_start[idx];
      const std::size_t cl = state.order_comment_len[idx];
      if (!two_part.MatchesView(cp, cl))
        continue;
      const int64_t ck = state.order_custkeys[idx];
      if (ck < 0 || static_cast<std::size_t>(ck) >= cc)
        continue;
      ++excluded[static_cast<std::size_t>(ck)];
      ++excluded_count;
    }
  }

  if (profiling) EmitCustomProfile("index_probe", t_probe.us(),
      candidates, excluded_count, (int64_t)cc, 0);

  // ── Counting histogram: max_eff ≪ ac, no comparison sort ──
  CustomTimer t_hist(profiling);
  const std::size_t ac = state.active_customers.size();
  int32_t max_eff = 0;

  // Pass 1: find max effective count for bucket allocation
  for (std::size_t i = 0; i < ac; ++i) {
    const int64_t ck = state.active_customers[i];
    if (ck < 0 || static_cast<std::size_t>(ck) >= cc) continue;
    int32_t eff = state.order_counts[static_cast<std::size_t>(ck)] -
                  excluded[static_cast<std::size_t>(ck)];
    if (eff < 0) eff = 0;
    if (eff > max_eff) max_eff = eff;
  }

  // Pass 2: count into buckets
  std::vector<int64_t> buckets(static_cast<std::size_t>(max_eff) + 1, 0);
  for (std::size_t i = 0; i < ac; ++i) {
    const int64_t ck = state.active_customers[i];
    if (ck < 0 || static_cast<std::size_t>(ck) >= cc) continue;
    int32_t eff = state.order_counts[static_cast<std::size_t>(ck)] -
                  excluded[static_cast<std::size_t>(ck)];
    if (eff < 0) eff = 0;
    ++buckets[static_cast<std::size_t>(eff)];
  }

  std::vector<TpchQ13OutputRow> rows;
  rows.reserve(buckets.size());
  for (int32_t eff = 0; eff <= max_eff; ++eff) {
    if (buckets[static_cast<std::size_t>(eff)] > 0)
      rows.push_back({static_cast<int64_t>(eff), buckets[static_cast<std::size_t>(eff)]});
  }
  if (profiling) EmitCustomProfile("histogram_indexed", t_hist.us(),
      (int64_t)ac, (int64_t)rows.size(), (int64_t)max_eff, 0);

  CustomTimer t_sort(profiling);
  std::sort(rows.begin(), rows.end(), Q13OutputRowBetter);
  if (profiling) EmitCustomProfile("sort_emit_indexed", t_sort.us(),
      (int64_t)rows.size(), 0, 0, 0);

  sink.Begin({{{"c_count", core::AuxType::Int64, false},
               {"custdist", core::AuxType::Int64, false}}});
  for (const auto &r : rows)
    sink.WriteRow({{core::AuxValue::Of(core::AuxType::Int64, r.c_count),
                    core::AuxValue::Of(core::AuxType::Int64, r.custdist)}});
  sink.End();
}

void TpchQ13Module::EmitResult(const TpchQ13State &state,
                               const std::string &pattern,
                               core::AuxEngineServices &services,
                               core::AuxRowSink &sink) const {
  (void)services;
  const bool profiling = CustomProfilingEnabled();

  CustomTimer t_build(profiling);
  TpchQ13TwoPartLikeMatcher two_part;
  two_part.Build(pattern);
  if (profiling) EmitCustomProfile("build_matcher", t_build.us(), 0, 0, 0, 0);

  const std::size_t cc = state.customer_valid.size();
  const std::size_t oc = state.order_custkeys.size();

  // Use the token-pair index when the pattern is a two-part LIKE and the
  // index has been built.  The index covers the first
  // trigram_index_order_count rows; rows appended by point deltas are scanned
  // as a tiny tail in EmitResultIndexed, so maintenance stays O(new orders)
  // without a full-table rebuild per write.
  if (two_part.enabled && state.trigram_index_built &&
      state.trigram_index_order_count > 0) {
    EmitResultIndexed(state, two_part, services, sink);
    return;
  }

  // Flat excluded array indexed by custkey (avoids std::map overhead)
  CustomTimer t_exclude(profiling);
  std::vector<int32_t> excluded(cc, 0);
  // Pre-capture to avoid repeated bounds checks in inner loop
  const int64_t * __restrict custkeys = state.order_custkeys.data();
  const uint8_t * __restrict alive = state.order_alive.data();
  const uint32_t * __restrict starts = state.order_comment_start.data();
  const uint32_t * __restrict lens = state.order_comment_len.data();
  const char * __restrict blob = state.comments_blob.data();
  int64_t excluded_count = 0;

  for (std::size_t idx = 0; idx < oc; ++idx) {
    if (!alive[idx]) continue;
    const int64_t ck = custkeys[idx];
    if (ck < 0 || static_cast<std::size_t>(ck) >= cc) continue;
    const char *cp = blob + starts[idx];
    const std::size_t cl = lens[idx];
    if (two_part.enabled ? two_part.MatchesView(cp, cl)
                         : SqlLikeSlice(cp, cl, pattern)) {
      ++excluded[static_cast<std::size_t>(ck)];
      ++excluded_count;
    }
  }
  if (profiling) EmitCustomProfile("exclude_scan", t_exclude.us(),
      (int64_t)oc, (int64_t)excluded_count, (int64_t)cc, 0);

  // Counting histogram: max_eff ≪ ac, no sort needed
  CustomTimer t_hist(profiling);
  const std::size_t ac = state.active_customers.size();
  int32_t max_eff = 0;
  for (std::size_t i = 0; i < ac; ++i) {
    const int64_t ck = state.active_customers[i];
    if (ck < 0 || static_cast<std::size_t>(ck) >= cc) continue;
    int32_t eff = state.order_counts[static_cast<std::size_t>(ck)] -
                  excluded[static_cast<std::size_t>(ck)];
    if (eff < 0) eff = 0;
    if (eff > max_eff) max_eff = eff;
  }
  std::vector<int64_t> buckets(static_cast<std::size_t>(max_eff) + 1, 0);
  for (std::size_t i = 0; i < ac; ++i) {
    const int64_t ck = state.active_customers[i];
    if (ck < 0 || static_cast<std::size_t>(ck) >= cc) continue;
    int32_t eff = state.order_counts[static_cast<std::size_t>(ck)] -
                  excluded[static_cast<std::size_t>(ck)];
    if (eff < 0) eff = 0;
    ++buckets[static_cast<std::size_t>(eff)];
  }

  std::vector<TpchQ13OutputRow> rows;
  rows.reserve(buckets.size());
  for (int32_t eff = 0; eff <= max_eff; ++eff) {
    if (buckets[static_cast<std::size_t>(eff)] > 0)
      rows.push_back({static_cast<int64_t>(eff), buckets[static_cast<std::size_t>(eff)]});
  }
  if (profiling) EmitCustomProfile("histogram", t_hist.us(),
      (int64_t)ac, (int64_t)rows.size(), (int64_t)max_eff, 0);

  CustomTimer t_sort(profiling);
  std::sort(rows.begin(), rows.end(), Q13OutputRowBetter);
  if (profiling) EmitCustomProfile("sort_emit", t_sort.us(),
      (int64_t)rows.size(), 0, 0, 0);

  sink.Begin({{{"c_count", core::AuxType::Int64, false},
               {"custdist", core::AuxType::Int64, false}}});
  for (const auto &r : rows)
    sink.WriteRow({{core::AuxValue::Of(core::AuxType::Int64, r.c_count),
                    core::AuxValue::Of(core::AuxType::Int64, r.custdist)}});
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query
