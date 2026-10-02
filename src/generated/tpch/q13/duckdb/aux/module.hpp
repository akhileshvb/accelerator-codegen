#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
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

struct TpchQ13State {
  std::vector<int32_t> order_counts;
  std::vector<uint8_t> customer_valid;
  std::vector<int64_t> active_customers;
  int64_t max_custkey = 0;

  std::vector<int64_t> order_custkeys;
  std::vector<uint32_t> order_comment_start;
  std::vector<uint32_t> order_comment_len;
  std::string comments_blob;
  std::vector<uint8_t> order_alive;

  // Inverted trigram index: maps 3-gram hash to the sorted list of order
  // indices containing that trigram.  Used when the pattern is `%a%b%` with
  // two non-empty literal parts.  At query time we intersect the lists for a
  // rare trigram from each part, then verify survivors with a full LIKE match.
  std::vector<uint32_t> trigram_index_keys;      // distinct trigram hashes
  std::vector<uint64_t> trigram_index_offsets;   // start offset into rows
  std::vector<uint32_t> trigram_index_rows;      // concatenated order index lists
  std::unordered_map<uint32_t, std::size_t> trigram_index_lookup;
  bool trigram_index_built = false;
  uint64_t trigram_index_pending = 0;            // deltas since last rebuild
  uint64_t trigram_index_order_count = 0;        // orders covered by last rebuild

  void AddCustomer(int64_t custkey);
  void RemoveCustomer(int64_t custkey);
  void AddOrder(int64_t custkey, const std::string &comment);
  void RemoveOrder(int64_t custkey, const std::string &comment);
  void RebuildTrigramIndex();
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
  void EnsureCustomerCapacity(TpchQ13State &state, int64_t custkey) const;
  TpchQ13State BuildState(core::AuxEngineServices &services) const;
  bool LoadState(core::AuxEngineServices &services,
                 std::shared_ptr<const TpchQ13State> &state) const;
  void EmitResult(const TpchQ13State &state, const std::string &pattern,
                  core::AuxEngineServices &services,
                  core::AuxRowSink &sink) const;
  void EmitResultIndexed(const TpchQ13State &state,
                         const TpchQ13TwoPartLikeMatcher &matcher,
                         core::AuxEngineServices &services,
                         core::AuxRowSink &sink) const;
  TpchQ13Config config;
  TpchQ13InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
