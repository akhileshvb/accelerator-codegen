#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"
#include "tpch/q13/layout.hpp"

namespace accelerator_codegen::aux_state::query {

struct TpchQ13Config {
  core::TableId customer_table = 0;
  core::TableId orders_table = 0;
};

struct TpchQ13CustomerInput {
  int64_t key = 0;
};

struct TpchQ13OrderInput {
  int64_t custkey = 0;
  std::string comment;
};

class TpchQ13InitialScan {
public:
  virtual ~TpchQ13InitialScan() = default;
  virtual void ScanCustomers(
      const std::function<void(const TpchQ13CustomerInput &)> &) = 0;
  virtual void ScanOrders(
      const std::function<void(const TpchQ13OrderInput &)> &) = 0;
  virtual bool LookupOrder(int64_t custkey, TpchQ13OrderInput &out) {
    (void)custkey;
    (void)out;
    return false;
  }
};

// Fast two-part LIKE matcher for "%a%b%" patterns.
struct TpchQ13TwoPartLikeMatcher {
  std::string part1;
  std::string part2;
  bool enabled = false;
  TpchQ13TwoPartLikeMatcher() = default;
  void Build(const std::string &pattern);
  bool Matches(const std::string &text) const;
  bool MatchesView(const char *data, std::size_t len) const;
};

// Sharding: custkey -> shard (custkey & mask), local index (custkey >> bits).
// Published states hold shared_ptr<const> shards; a write clones only the
// shards it touches (copy-on-write), never the whole state.
constexpr uint32_t kTpchQ13ShardBits = 14;
constexpr std::size_t kTpchQ13Shards = std::size_t{1} << kTpchQ13ShardBits;
constexpr std::size_t kTpchQ13MaxPatterns = 4;

// Orders (custkey, comment) of one shard.  Deleted orders are tombstoned and
// the shard is compacted locally once more than half is dead.
struct TpchQ13OrderShard {
  std::vector<int64_t> custkey;
  std::vector<uint32_t> start;
  std::vector<uint32_t> len;
  std::string blob;
  std::vector<uint8_t> alive;
  uint32_t dead = 0;
};

// Per-customer data of one shard, indexed by custkey >> bits.
struct TpchQ13CustShard {
  std::vector<int32_t> total;               // orders with this custkey
  std::vector<int32_t> mult;                // customer rows with this key
  std::vector<std::vector<int32_t>> match;  // [pattern][local] matching orders
};

struct TpchQ13Pattern {
  std::string text;
  TpchQ13TwoPartLikeMatcher matcher;
};

struct TpchQ13State {
  std::vector<std::shared_ptr<const TpchQ13OrderShard>> orders;  // may be null
  std::vector<std::shared_ptr<const TpchQ13CustShard>> cust;     // may be null
  // Tracked LIKE patterns; hist[p][c] = #customers whose count of orders NOT
  // matching patterns[p] equals c.  Immutable per published version.
  std::vector<TpchQ13Pattern> patterns;
  std::vector<std::vector<int64_t>> hist;
};

enum class TpchQ13ImageKind : uint8_t { CustomerCounts };

struct TpchQ13StateImage {
  TpchQ13ImageKind kind = TpchQ13ImageKind::CustomerCounts;
  std::shared_ptr<const TpchQ13State> state;
};

struct TpchQ13OutputRow {
  int64_t c_count = 0;
  int64_t custdist = 0;
};

struct TpchQ13Delta {
  int8_t sign = 0;
  int64_t custkey = 0;
  std::string comment;
  bool customer = false;  // customer-row delta (comment unused)
};

std::vector<uint8_t> EncodeTpchQ13CustomerCounts(const TpchQ13State &state);
bool DecodeTpchQ13CustomerCounts(const std::vector<uint8_t> &bytes,
                                 TpchQ13State &state);

class TpchQ13TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;
  bool RequiresRebuild() const;
  const std::vector<TpchQ13Delta> &Deltas() const;
  void MarkRebuild();
  void AddDelta(TpchQ13Delta delta);
private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ13Delta> deltas;
};

class TpchQ13Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = core::TpchQ13Layout::kAccelerator;
  explicit TpchQ13Module(TpchQ13Config config,
                         TpchQ13InitialScan *initial_scan = nullptr)
      : config(config), initial_scan(initial_scan) {}
  core::AcceleratorId Id() const override;
  std::vector<core::TableDependency> Dependencies() const override;
  std::unique_ptr<core::AuxTxnBuffer> CreateTxnBuffer() const override;
  void Build(core::AuxEngineServices &, core::StateBuilder &) override;
  core::InvalidationPlan
  InterpretMutation(const core::MutationBatch &) const override;
  void AccumulateMutation(const core::MutationBatch &,
                          core::AuxEngineServices &,
                          core::AuxTxnBuffer &) override;
  core::StagedMaintenanceOutcome
  StageMaintenance(core::AuxEngineServices &, const core::InvalidationPlan &,
                   core::AuxTxnBuffer &) override;
  bool CheckValidity(core::AuxEngineServices &,
                     const core::QueryArguments &) const override;
  void Execute(core::AuxEngineServices &, const core::QueryArguments &,
               core::AuxRowSink &) override;
  void ExecuteFallback(core::AuxEngineServices &, const core::QueryArguments &,
                       core::AuxRowSink &) override;
  std::vector<core::CalibrationWrite> CalibrationWrites() const override;
private:
  void NoteWantedPattern(const std::string &pattern) const;
  std::vector<std::string> WantedPatterns() const;
  TpchQ13State BuildState(core::AuxEngineServices &services) const;
  bool LoadState(core::AuxEngineServices &services,
                 std::shared_ptr<const TpchQ13State> &state) const;
  void EmitResult(const TpchQ13State &state, const std::string &pattern,
                  core::AuxEngineServices &services,
                  core::AuxRowSink &sink) const;
  TpchQ13Config config;
  TpchQ13InitialScan *initial_scan = nullptr;
  mutable std::mutex wanted_mu;
  mutable std::vector<std::string> wanted;  // patterns seen by reads
};

} // namespace accelerator_codegen::aux_state::query
