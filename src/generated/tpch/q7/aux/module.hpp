#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"
#include "accelerator_codegen/stateful/core/query_layouts.hpp"

namespace accelerator_codegen::aux_state::query {

struct TpchQ7Config {
  core::TableId nation_table = 0;
  core::TableId supplier_table = 0;
  core::TableId customer_table = 0;
  core::TableId orders_table = 0;
  core::TableId lineitem_table = 0;
};

struct TpchQ7NationInput {
  int32_t key = 0;
  std::string name;
};

struct TpchQ7SupplierInput {
  int64_t key = 0;
  int32_t nationkey = 0;
};

struct TpchQ7CustomerInput {
  int64_t key = 0;
  int32_t nationkey = 0;
};

struct TpchQ7OrderInput {
  int64_t key = 0;
  int64_t custkey = 0;
};

struct TpchQ7LineitemInput {
  int64_t orderkey = 0;
  int64_t suppkey = 0;
  int64_t extended_price_raw = 0;
  int32_t extended_price_scale = 2;
  int64_t discount_raw = 0;
  int32_t discount_scale = 2;
  int32_t shipdate_days = 0;
};

class TpchQ7InitialScan {
public:
  virtual ~TpchQ7InitialScan() = default;
  virtual void ScanNations(
      const std::function<void(const TpchQ7NationInput &)> &) = 0;
  virtual void ScanSuppliers(
      const std::function<void(const TpchQ7SupplierInput &)> &) = 0;
  virtual void ScanCustomers(
      const std::function<void(const TpchQ7CustomerInput &)> &) = 0;
  virtual void ScanOrders(
      const std::function<void(const TpchQ7OrderInput &)> &) = 0;
  virtual void ScanLineitems(
      const std::function<void(const TpchQ7LineitemInput &)> &) = 0;
};

constexpr int32_t kTpchQ7MinNationSlots = 25;
constexpr int32_t kTpchQ7MinYear = 1995;
constexpr int32_t kTpchQ7MaxYear = 1996;

struct TpchQ7LineitemIndexEntry {
  int32_t suppkey = 0;
  int32_t shipdate_days = 0;
  int64_t custkey = 0;
};

struct TpchQ7State {
  std::unordered_map<int32_t, std::string> nation_names;
  std::unordered_map<int64_t, int32_t> supplier_nations;
  std::unordered_map<int64_t, int32_t> customer_nations;
  // Transient during BuildState only; published side state keeps custkey inside
  // lineitem_index instead of a second SF-scale order map.
  std::unordered_map<int64_t, int64_t> order_custkeys;
  std::vector<int64_t> revenue_1995;
  std::vector<int64_t> revenue_1996;
  int32_t num_nation_slots = kTpchQ7MinNationSlots;

  // Delta-resolution index: orderkey → (suppkey, shipdate_days, custkey).
  // Built during BuildState for all lineitems in [1995-01-01, 1996-12-31].
  // Populated only when the order has >=1 lineitem in the window.
  // For orders with multiple lineitems the last-scanned wins; the common
  // case is a single valid lineitem per order in the probe-selection window.
  std::unordered_map<int64_t, TpchQ7LineitemIndexEntry> lineitem_index;
};

enum class TpchQ7ImageKind : uint8_t { Side, Revenue };

struct TpchQ7StateImage {
  TpchQ7ImageKind kind = TpchQ7ImageKind::Side;
  std::shared_ptr<const TpchQ7State> state;
};

struct TpchQ7LineitemDelta {
  int32_t supp_nk = 0;
  int32_t cust_nk = 0;
  int year = 0;
  int64_t revenue_raw = 0;
  int8_t sign = 0;
};

std::vector<uint8_t> EncodeTpchQ7Side(const TpchQ7State &);
bool DecodeTpchQ7Side(const std::vector<uint8_t> &, TpchQ7State &);
std::vector<uint8_t> EncodeTpchQ7Revenue(const TpchQ7State &);
bool DecodeTpchQ7Revenue(const std::vector<uint8_t> &, TpchQ7State &);

int64_t TpchQ7RevenueRaw(int64_t extended_price_raw, int32_t extended_scale,
                         int64_t discount_raw, int32_t discount_scale);
int32_t TpchQ7MonthKeyFromDays(int32_t days_since_epoch);
int32_t TpchQ7DaysFromYmd(int32_t year, unsigned month, unsigned day);

class TpchQ7TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ7LineitemDelta> &Deltas() const;
  void MarkRebuild();
  void AddDelta(TpchQ7LineitemDelta delta);

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ7LineitemDelta> deltas;
};

class TpchQ7Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = core::TpchQ7Layout::kAccelerator;

  explicit TpchQ7Module(TpchQ7Config config,
                        TpchQ7InitialScan *initial_scan = nullptr)
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

private:
  TpchQ7State BuildState(core::AuxEngineServices &) const;
  bool LoadState(core::AuxEngineServices &, const std::string &nation1,
                 const std::string &nation2,
                 std::shared_ptr<const TpchQ7State> &side,
                 std::vector<std::pair<int32_t, std::shared_ptr<const TpchQ7State>>> &months,
                 std::map<core::PartitionId, core::Version> *versions = nullptr) const;
  void EmitResult(const std::shared_ptr<const TpchQ7State> &side,
                  const std::shared_ptr<const TpchQ7State> &revenue,
                  const std::string &nation1, const std::string &nation2,
                  core::AuxRowSink &) const;

  TpchQ7Config config;
  TpchQ7InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
