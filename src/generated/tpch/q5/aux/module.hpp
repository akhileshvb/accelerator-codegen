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

struct TpchQ5Config {
  core::TableId region_table = 0;
  core::TableId nation_table = 0;
  core::TableId customer_table = 0;
  core::TableId supplier_table = 0;
  core::TableId orders_table = 0;
  core::TableId lineitem_table = 0;
};

struct TpchQ5RegionInput {
  int32_t key = 0;
  std::string name;
};

struct TpchQ5NationInput {
  int32_t key = 0;
  std::string name;
  int32_t regionkey = 0;
};

struct TpchQ5CustomerInput {
  int64_t custkey = 0;
  int32_t nationkey = 0;
};

struct TpchQ5SupplierInput {
  int64_t suppkey = 0;
  int32_t nationkey = 0;
};

struct TpchQ5OrderInput {
  int64_t orderkey = 0;
  int64_t custkey = 0;
  int32_t orderdate_days = 0;
};

struct TpchQ5LineitemInput {
  int64_t orderkey = 0;
  int64_t suppkey = 0;
  int64_t extended_price_raw = 0;
  int32_t extended_price_scale = 2;
  int64_t discount_raw = 0;
  int32_t discount_scale = 2;
};

class TpchQ5InitialScan {
public:
  virtual ~TpchQ5InitialScan() = default;
  virtual void ScanRegions(
      const std::function<void(const TpchQ5RegionInput &)> &) = 0;
  virtual void ScanNations(
      const std::function<void(const TpchQ5NationInput &)> &) = 0;
  virtual void ScanCustomers(
      const std::function<void(const TpchQ5CustomerInput &)> &) = 0;
  virtual void ScanSuppliers(
      const std::function<void(const TpchQ5SupplierInput &)> &) = 0;
  virtual void ScanOrders(
      const std::function<void(const TpchQ5OrderInput &)> &) = 0;
  virtual void ScanLineitems(
      const std::function<void(const TpchQ5LineitemInput &)> &) = 0;
};

struct TpchQ5NodeRevenue {
  std::unordered_map<int32_t, int64_t> nation_revenue;
};

struct TpchQ5State {
  int32_t min_orderdate_days = 0;
  int32_t max_orderdate_days = 0;
  uint32_t tree_size = 0;
  std::vector<TpchQ5NodeRevenue> tree;
  std::unordered_map<int32_t, std::string> nation_name;
  std::unordered_map<int32_t, int32_t> nation_region;
  std::unordered_map<std::string, int32_t> region_name_to_key;
  std::unordered_map<int32_t, std::vector<int32_t>> region_nations;
  std::unordered_map<int64_t, int32_t> orderkey_to_nationkey;
  std::unordered_map<int64_t, int32_t> orderkey_to_ordinal;
  std::unordered_map<int64_t, int32_t> suppkey_to_nationkey;
};

struct TpchQ5StateImage {
  bool is_side = false;
  uint32_t node_index = 0;
  std::shared_ptr<const TpchQ5State> state;
};

class TpchQ5TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  void MarkRebuild();

  struct LineitemDelta {
    TpchQ5LineitemInput old_row;
    TpchQ5LineitemInput new_row;
    bool is_delete = false;
    bool is_insert = false;
  };
  void AddLineitemMutation(const LineitemDelta &d);
  const std::vector<LineitemDelta> &LineitemMutations() const;
  bool HasLineitemMutations() const;

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<LineitemDelta> lineitem_mutations;
};

int32_t TpchQ5DaysFromYmd(int32_t year, unsigned month, unsigned day);
int32_t TpchQ5AddMonths(int32_t days_since_epoch, int32_t months);
int64_t TpchQ5RevenueRaw(int64_t extended_price_raw, int64_t discount_raw);

void TpchQ5FinalizeAndEmit(
    const std::unordered_map<int32_t, std::string> &nation_name,
    const std::unordered_map<int32_t, int32_t> &nation_region,
    int32_t target_regionkey,
    const std::unordered_map<int32_t, int64_t> &revenue_by_nation,
    core::AuxRowSink &sink);

inline int32_t TpchQ5DateOrdinal(int32_t orderdate_days,
                                 int32_t min_orderdate_days) {
  return orderdate_days - min_orderdate_days;
}

inline uint32_t TpchQ5LeafNode(uint32_t tree_size, int32_t ordinal) {
  return tree_size + static_cast<uint32_t>(ordinal);
}

// Canonical cover of [start, end) into tree node indices.
std::vector<uint32_t> TpchQ5TreeCover(uint32_t tree_size, int32_t start_ordinal,
                                      int32_t end_ordinal);

class TpchQ5Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = core::TpchQ5Layout::kAccelerator;

  explicit TpchQ5Module(TpchQ5Config config,
                        TpchQ5InitialScan *initial_scan = nullptr)
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
  TpchQ5State BuildState(core::AuxEngineServices &) const;
  bool LoadSide(core::AuxEngineServices &, TpchQ5State &) const;
  bool LoadTreeNodes(core::AuxEngineServices &, const TpchQ5State &,
                     const std::vector<uint32_t> &node_indices,
                     std::unordered_map<int32_t, int64_t> &revenue_by_nation,
                     std::map<uint32_t, core::Version> *versions = nullptr) const;
  int32_t ResolveRegion(const std::string &region_name,
                        const TpchQ5State &,
                        std::vector<int32_t> &eligible_nations) const;
  void EmitResult(core::AuxEngineServices &, const TpchQ5State &,
                  int32_t target_regionkey,
                  const std::unordered_map<int32_t, int64_t> &revenue_by_nation,
                  core::AuxRowSink &) const;
  core::StagedMaintenanceOutcome
  StageIncrementalMaintenance(core::AuxEngineServices &,
                              const core::InvalidationPlan &,
                              TpchQ5TxnBuffer &);

  TpchQ5Config config;
  TpchQ5InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
