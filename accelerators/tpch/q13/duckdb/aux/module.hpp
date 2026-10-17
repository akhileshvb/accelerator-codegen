#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "tpch/q13/layout.hpp"

namespace accelerator_codegen::aux_state::query {

struct TpchQ13Config {
  core::TableId customer_table = 0;
  core::TableId orders_table = 0;
};

struct TpchQ13CustomerInput {
  int64_t key = 0;
};

class TpchQ13InitialScan {
public:
  virtual ~TpchQ13InitialScan() = default;
  // Partitioned scans for builds: rows of one partition arrive in table order
  // on one thread; partitions run concurrently on the pool.
  virtual void ScanCustomers(
      const core::AuxEngineServices &, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ13CustomerInput &)> &) = 0;
  virtual void ScanOrders(
      const core::AuxEngineServices &, std::size_t partitions,
      const std::function<void(std::size_t, int64_t custkey, std::string_view comment)> &) = 0;
};

// Exact SQL LIKE matcher ('%' any run, '_' one byte, no ESCAPE, bytewise),
// compiled once per pattern.  Literal-only segments use a vectorised search.
struct TpchQ13Like {
  struct Seg {
    std::string text;
    bool wild = false;  // contains '_'
  };
  std::string pattern;
  std::vector<Seg> parts;      // pattern split on '%'
  bool has_pct = false;
  bool all_pct = false;        // only '%' characters (matches everything)
  bool single_lit = false;     // exactly "%literal%" (no '_')
  std::vector<std::string> chunks;  // literal runs between wildcards
  std::string anchor;          // longest literal chunk ("" = none)
  void Build(const std::string &pattern);
  bool Matches(const char *data, std::size_t len) const;
};

// Sharding: custkey -> shard (custkey & mask), local index (custkey >> bits).
// Published states hold shared_ptr<const> shards; a write clones only the
// shards it touches (copy-on-write), never the whole state.
constexpr uint32_t kTpchQ13ShardBits = 14;
constexpr std::size_t kTpchQ13Shards = std::size_t{1} << kTpchQ13ShardBits;
constexpr std::size_t kTpchQ13MaxPatterns = 32;  // tracked (maintained) patterns

// Word-token inverted index of one order shard.  A token is a maximal run of
// ASCII letters/digits; postings list the shard-local orders containing it.
// Orders whose tokens cannot all be indexed (very long token, full vocabulary)
// are listed in `always` and verified for every query.  Postings of deleted
// orders stay until the shard is compacted (queries check liveness).
struct TpchQ13TokIndex {
  std::vector<uint16_t> toks;    // sorted token ids present in the shard
  std::vector<uint32_t> off;     // toks.size()+1 offsets into `ords`
  std::vector<uint16_t> ords;    // order indices, ascending within a token
  std::vector<uint16_t> always;  // unindexable orders
  uint32_t covered = 0;          // orders [0, covered) are indexed
  bool disabled = false;         // > 65535 orders: queries scan this shard
};

// Token vocabulary shared by all shards of one published version.
struct TpchQ13Vocab {
  std::vector<std::string> toks;
  std::unordered_map<std::string, uint16_t> id;
};

// A run of orders (custkey, comment) with its own blob and token index.
// Order indices inside a segment (and in its postings) are segment-local.
struct TpchQ13OrderSeg {
  std::vector<int64_t> custkey;
  std::vector<uint32_t> start;
  std::vector<uint32_t> len;
  std::string blob;
  TpchQ13TokIndex ix;
};

// Orders of one shard = an immutable shared `base` segment (cloned never, only
// shared between published versions) followed by a small per-version `tail`
// segment of recently appended orders.  `alive` covers base then tail orders.
// Deleted orders are tombstoned; the shard is compacted locally once more than
// half is dead, and the tail is folded into the base when it grows large.
struct TpchQ13OrderShard {
  std::shared_ptr<const TpchQ13OrderSeg> base;  // may be null
  TpchQ13OrderSeg tail;
  std::vector<uint8_t> alive;
  uint32_t dead = 0;
  std::size_t BaseSize() const { return base ? base->custkey.size() : 0; }
  std::size_t Size() const { return BaseSize() + tail.custkey.size(); }
};

// Per-customer data of one shard, one slot per customer key seen (local id =
// custkey >> bits). Dense key spaces use the identity layout; a sparse one lists
// the local ids of its slots, so memory follows the customers, not the largest key.
struct TpchQ13CustShard {
  static constexpr std::size_t kNone = ~std::size_t{0};
  std::vector<uint32_t> ids;                // ascending local ids; empty: slot i = id i
  std::vector<int32_t> total;               // orders with this custkey
  std::vector<int32_t> mult;                // customer rows with this key
  std::vector<std::vector<int32_t>> match;  // [pattern][slot] matching orders

  uint32_t IdOf(std::size_t slot) const {
    return ids.empty() ? static_cast<uint32_t>(slot) : ids[slot];
  }
  std::size_t SlotOf(uint32_t id) const {
    if (ids.empty())
      return id < total.size() ? id : kNone;
    const auto it = std::lower_bound(ids.begin(), ids.end(), id);
    return it != ids.end() && *it == id ? static_cast<std::size_t>(it - ids.begin()) : kNone;
  }
};

struct TpchQ13Pattern {
  std::string text;
  TpchQ13Like like;
  uint64_t need = 0;       // literal bits an order must contain (0: verify all)
};

// Literals (distinct pattern chunks of the tracked patterns, <= 64): a
// per-order bitmask of the literals it contains cheaply rules patterns out.
struct TpchQ13LitTable {
  std::vector<std::string> lits;
};


struct TpchQ13OutputRow {
  int64_t c_count = 0;
  int64_t custdist = 0;
};

struct TpchQ13State {
  std::vector<std::shared_ptr<const TpchQ13OrderShard>> orders;  // may be null
  std::vector<std::shared_ptr<const TpchQ13CustShard>> cust;     // may be null
  // Tracked LIKE patterns; hist[p][c] = #customers whose count of orders NOT
  // matching patterns[p] equals c.  Immutable per published version.
  std::vector<TpchQ13Pattern> patterns;
  std::vector<std::vector<int64_t>> hist;
  // Read-ready: hist[p] as (c_count, custdist) rows already in output order
  // (custdist DESC, c_count DESC), rebuilt from hist on every publish.
  std::vector<std::vector<TpchQ13OutputRow>> sorted;
  // Histogram when no order matches (c_count = all orders).  An untracked
  // pattern's histogram is this one adjusted only for customers owning
  // matching orders.
  std::vector<int64_t> base;
  int64_t live_orders = 0;  // live (non-tombstoned) orders
  std::shared_ptr<const TpchQ13LitTable> lits;
  std::shared_ptr<const TpchQ13Vocab> vocab;
  std::vector<int32_t> df;  // postings per token id (document frequency)
};

enum class TpchQ13ImageKind : uint8_t { CustomerCounts };

struct TpchQ13StateImage {
  TpchQ13ImageKind kind = TpchQ13ImageKind::CustomerCounts;
  std::shared_ptr<const TpchQ13State> state;
};

struct TpchQ13Delta {
  int8_t sign = 0;
  int64_t custkey = 0;
  std::string comment;
  bool customer = false;  // customer-row delta (comment unused)
};


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
  std::vector<core::CalibrationWrite> CalibrationWrites() const override;
private:
  TpchQ13State BuildState(core::AuxEngineServices &services) const;
  bool LoadState(core::AuxEngineServices &services,
                 std::shared_ptr<const TpchQ13State> &state) const;
  void EmitResult(const TpchQ13State &state, const std::string &pattern,
                  core::AuxEngineServices &services,
                  core::AuxRowSink &sink) const;
  TpchQ13Config config;
  TpchQ13InitialScan *initial_scan = nullptr;
  core::QueryKey hit_key{core::AcceleratorId(core::TpchQ13Layout::kAccelerator), {}};
};

} // namespace accelerator_codegen::aux_state::query
