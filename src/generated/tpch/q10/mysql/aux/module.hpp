#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"
#include "accelerator_codegen/stateful/core/dense_id_array.hpp"
#include "accelerator_codegen/stateful/core/query_layouts.hpp"

namespace accelerator_codegen::aux_state::query {

struct TpchQ10Config {
  core::TableId customer_table = 0;
  core::TableId nation_table = 0;
  core::TableId orders_table = 0;
  core::TableId lineitem_table = 0;
};

// Query-specific initial-scan contract.  Engine adapters implement this
// using their native column/vector APIs; no generic variant row crosses the
// bulk-scan boundary.
struct TpchQ10CustomerInput {
  int64_t key = 0;
  std::string name;
  int64_t acctbal_raw = 0;
  int32_t nationkey = 0;
  std::string address;
  std::string phone;
  std::string comment;
};

struct TpchQ10NationInput {
  int32_t key = 0;
  std::string name;
};

struct TpchQ10OrderInput {
  int64_t key = 0;
  int64_t custkey = 0;
  int32_t orderdate_days = 0;
};

struct TpchQ10LineitemInput {
  int64_t orderkey = 0;
  int64_t extended_price_raw = 0;
  int32_t extended_price_scale = 2;
  int64_t discount_raw = 0;
  int32_t discount_scale = 2;
  bool returned = false;
};

// Pre-aggregated returned revenue for the SQL-pushdown build path.
struct TpchQ10ReturnedRevenueInput {
  int32_t month_key = 0; // YYYYMM
  int64_t custkey = 0;
  int64_t revenue_raw = 0;
};

struct TpchQ10Customer {
  std::string name;
  int64_t acctbal_raw = 0;
  int32_t nationkey = 0;
  std::string address;
  std::string phone;
  std::string comment;
};

struct TpchQ10Order {
  int64_t custkey = 0;
  int32_t orderdate_days = 0;
};

struct TpchQ10CustomerRank {
  int64_t acctbal_raw = 0;
  int32_t nationkey = 0;
  uint8_t present = 0;
};

class TpchQ10InitialScan {
public:
  virtual ~TpchQ10InitialScan() = default;
  virtual void ScanCustomers(
      const std::function<void(const TpchQ10CustomerInput &)> &) = 0;
  virtual void ScanNations(
      const std::function<void(const TpchQ10NationInput &)> &) = 0;
  virtual void ScanOrders(
      const std::function<void(const TpchQ10OrderInput &)> &) = 0;
  virtual void ScanLineitems(
      const std::function<void(const TpchQ10LineitemInput &)> &) = 0;

  virtual bool ScanReturnedCustomerRevenue(
      const std::function<void(const TpchQ10ReturnedRevenueInput &)> &consumer) {
    (void)consumer;
    return false;
  }

  virtual bool ScanReturnedCustomerRevenueForMonths(
      const std::set<int32_t> &months,
      const std::function<void(const TpchQ10ReturnedRevenueInput &)> &consumer) {
    if (months.empty())
      return true;
    return ScanReturnedCustomerRevenue(
        [&](const TpchQ10ReturnedRevenueInput &input) {
          if (months.count(input.month_key))
            consumer(input);
        });
  }

  virtual bool LookupOrders(
      const std::vector<int64_t> &orderkeys,
      std::unordered_map<int64_t, TpchQ10Order> &out) {
    (void)orderkeys;
    (void)out;
    return false;
  }

};

using TpchQ10CustomerMap = std::unordered_map<int64_t, TpchQ10Customer>;

struct TpchQ10State {
  core::DenseIdArray<TpchQ10CustomerRank> customer_ranks;
  // Every customer's result columns (name, address, phone, comment), so
  // Execute emits the top-k without querying the database. Shared: maintenance
  // copies of the state do not copy it (customer changes rebuild the state).
  std::shared_ptr<const TpchQ10CustomerMap> customers =
      std::make_shared<const TpchQ10CustomerMap>();
  std::unordered_map<int32_t, std::string> nations;
  std::unordered_map<int64_t, TpchQ10Order> orders;
  std::map<int32_t, std::unordered_map<int64_t, int64_t>> month_revenue;

  void SetCustomerRank(int64_t custkey, int64_t acctbal_raw, int32_t nationkey) {
    TpchQ10CustomerRank rank;
    rank.acctbal_raw = acctbal_raw;
    rank.nationkey = nationkey;
    rank.present = 1;
    customer_ranks.Set(custkey, rank);
  }
  bool HasCustomer(int64_t custkey) const {
    return customer_ranks.Get(custkey).present != 0;
  }
  TpchQ10CustomerRank RankOf(int64_t custkey) const {
    return customer_ranks.Get(custkey);
  }
};

enum class TpchQ10ImageKind : uint8_t { Side, Orders, Month };

struct TpchQ10StateImage {
  TpchQ10ImageKind kind = TpchQ10ImageKind::Side;
  int32_t month = 0;
  std::shared_ptr<const TpchQ10State> state;
};

struct TpchQ10LineitemDelta {
  int64_t orderkey = 0;
  int64_t revenue_raw = 0;
  int8_t sign = 0;
};

struct TpchQ10OrderDelta {
  int64_t orderkey = 0;
  int64_t custkey = 0;
  int32_t orderdate_days = 0;
  int8_t sign = 0; // +1 insert/update-new, -1 delete/update-old
};

std::vector<uint8_t> EncodeTpchQ10Side(const TpchQ10State &);
std::vector<uint8_t> EncodeTpchQ10Orders(const TpchQ10State &);
std::vector<uint8_t> EncodeTpchQ10Month(int32_t month, const TpchQ10State &);
bool DecodeTpchQ10Side(const std::vector<uint8_t> &, TpchQ10State &);
bool DecodeTpchQ10Orders(const std::vector<uint8_t> &, TpchQ10State &);
bool DecodeTpchQ10Month(const std::vector<uint8_t> &, int32_t month,
                        TpchQ10State &);
// Order metadata is encoded as sorted fixed-width records. Maintenance can
// look up one order without reconstructing the full order hash table.
bool FindTpchQ10Order(const std::vector<uint8_t> &, int64_t orderkey,
                      TpchQ10Order &);

int32_t TpchQ10MonthKeyFromDays(int32_t days_since_epoch);
int32_t TpchQ10MonthStartDays(int32_t month_key);
int32_t TpchQ10AddMonths(int32_t days_since_epoch, int32_t months);
int64_t TpchQ10RevenueRaw(double extended_price, double discount);

// TPC-H Q10 returns the top 20 customers by revenue.
constexpr std::size_t kTpchQ10ResultLimit = 20;

std::vector<std::pair<int64_t, int64_t>> TpchQ10SelectTopK(
    const core::DenseIdArray<TpchQ10CustomerRank> &ranks,
    const std::vector<const std::unordered_map<int64_t, int64_t> *>
        &month_revenues,
    std::size_t limit = kTpchQ10ResultLimit);

void TpchQ10FinalizeAndEmit(const TpchQ10State &state, int32_t start_days,
                            core::AuxRowSink &sink);

// Hit-path finalize: rank from month maps + stored acctbal/nationkey, then
// emit top-k with the stored customer columns (typically 20 rows).
void TpchQ10FinalizeAndEmit(
    const core::DenseIdArray<TpchQ10CustomerRank> &ranks,
    const std::unordered_map<int32_t, std::string> &nations,
    const std::vector<const std::unordered_map<int64_t, int64_t> *>
        &month_revenues,
    const std::unordered_map<int64_t, TpchQ10Customer> &customer_strings,
    int32_t start_days, core::AuxRowSink &sink,
    std::size_t limit = kTpchQ10ResultLimit);

class TpchQ10TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ10LineitemDelta> &Deltas() const;
  const std::vector<TpchQ10OrderDelta> &OrderDeltas() const;
  void MarkRebuild();
  void AddDelta(TpchQ10LineitemDelta delta);
  void AddOrderDelta(TpchQ10OrderDelta delta);

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ10LineitemDelta> deltas;
  std::vector<TpchQ10OrderDelta> order_deltas;
};

class TpchQ10Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = core::TpchQ10Layout::kAccelerator;

  explicit TpchQ10Module(TpchQ10Config config,
                         TpchQ10InitialScan *initial_scan = nullptr)
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
  TpchQ10State BuildState(core::AuxEngineServices &) const;
  bool LoadState(core::AuxEngineServices &, TpchQ10State &, int32_t start_days,
                 std::map<int32_t, core::Version> *versions = nullptr) const;
  void EmitResult(const TpchQ10State &, int32_t start_days,
                  core::AuxRowSink &) const;

  TpchQ10Config config;
  TpchQ10InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
