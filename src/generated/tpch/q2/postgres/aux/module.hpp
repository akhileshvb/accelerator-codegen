#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"

namespace accelerator_codegen::aux_state::query {

struct TpchQ2Config {
  core::TableId part_table = 0;
  core::TableId supplier_table = 0;
  core::TableId partsupp_table = 0;
  core::TableId nation_table = 0;
  core::TableId region_table = 0;
};

struct TpchQ2PartInput {
  int64_t key = 0;
  std::string mfgr;
  std::string type;
  int32_t size = 0;
};

struct TpchQ2SupplierInput {
  int64_t key = 0;
  std::string name;
  std::string address;
  int32_t nationkey = 0;
  std::string phone;
  int64_t acctbal_raw = 0;
  std::string comment;
};

struct TpchQ2PartsuppInput {
  int64_t partkey = 0;
  int64_t suppkey = 0;
  int64_t supplycost_raw = 0;
};

struct TpchQ2NationInput {
  int32_t key = 0;
  std::string name;
  int32_t regionkey = 0;
};

struct TpchQ2RegionInput {
  int32_t key = 0;
  std::string name;
};

class TpchQ2InitialScan {
public:
  virtual ~TpchQ2InitialScan() = default;
  virtual void ScanParts(
      const std::function<void(const TpchQ2PartInput &)> &) = 0;
  virtual void ScanSuppliers(
      const std::function<void(const TpchQ2SupplierInput &)> &) = 0;
  virtual void ScanPartsupps(
      const std::function<void(const TpchQ2PartsuppInput &)> &) = 0;
  virtual void ScanNations(
      const std::function<void(const TpchQ2NationInput &)> &) = 0;
  virtual void ScanRegions(
      const std::function<void(const TpchQ2RegionInput &)> &) = 0;
};

struct TpchQ2SupplierInfo {
  std::string name;
  std::string address;
  int32_t nationkey = 0;
  std::string phone;
  int64_t acctbal_raw = 0;
  std::string comment;
};

struct TpchQ2PartInfo {
  std::string mfgr;
  std::string type;
  int32_t size = 0;
};

struct TpchQ2RegionalMinCell {
  int64_t min_supplycost_raw = 0;
  std::vector<int64_t> tied_suppkeys;
};

struct TpchQ2State {
  // Immutable dimension data — shared_ptr avoids deep copies during
  // maintenance clones.  Flat vectors indexed by natural key (partkey,
  // suppkey, nationkey, regionkey) eliminate hash lookups on hot paths.
  std::shared_ptr<const std::vector<std::string>> region_names;
  std::shared_ptr<const std::vector<std::string>> nation_names;
  std::shared_ptr<const std::vector<int32_t>> nation_region;
  std::shared_ptr<const std::vector<TpchQ2SupplierInfo>> suppliers;
  std::shared_ptr<const std::vector<uint8_t>> supplier_valid;
  std::shared_ptr<const std::vector<TpchQ2PartInfo>> parts;
  std::shared_ptr<const std::vector<uint8_t>> part_valid;
  std::unordered_map<int32_t, std::vector<int64_t>> parts_by_size;
  std::unordered_map<int64_t, TpchQ2RegionalMinCell> regional_minima;

  // Two-level regional_minima for cheap incremental writes:
  //   regional_minima_base is the immutable full map built once and shared
  //   regional_minima_overlay holds only cells modified by ApplyDelta.
  // Reads check the overlay first, then fall back to the base.
  std::shared_ptr<const std::unordered_map<int64_t, TpchQ2RegionalMinCell>>
      regional_minima_base;
  std::unordered_map<int64_t, TpchQ2RegionalMinCell> regional_minima_overlay;

  const TpchQ2SupplierInfo *FindSupplier(int64_t suppkey) const {
    if (!supplier_valid || suppkey < 0 ||
        static_cast<std::size_t>(suppkey) >= supplier_valid->size())
      return nullptr;
    return (*supplier_valid)[static_cast<std::size_t>(suppkey)]
               ? &(*suppliers)[static_cast<std::size_t>(suppkey)]
               : nullptr;
  }

  const TpchQ2PartInfo *FindPart(int64_t partkey) const {
    if (!part_valid || partkey < 0 ||
        static_cast<std::size_t>(partkey) >= part_valid->size())
      return nullptr;
    return (*part_valid)[static_cast<std::size_t>(partkey)]
               ? &(*parts)[static_cast<std::size_t>(partkey)]
               : nullptr;
  }

  const TpchQ2RegionalMinCell *FindRegionalMin(int64_t key) const {
    const auto it = regional_minima_overlay.find(key);
    if (it != regional_minima_overlay.end()) {
      return it->second.tied_suppkeys.empty() ? nullptr : &it->second;
    }
    if (regional_minima_base) {
      const auto it2 = regional_minima_base->find(key);
      if (it2 != regional_minima_base->end())
        return &it2->second;
    }
    return nullptr;
  }
};

inline int64_t TpchQ2CellKey(int32_t region_key, int64_t partkey) {
  return (static_cast<int64_t>(region_key) << 32) | (partkey & 0xFFFFFFFFLL);
}

// Pre-parsed LIKE pattern accelerator for fast type matching.
struct TpchQ2LikeAccelerator {
  bool enabled = false;
  bool prefix_wild = false;
  bool suffix_wild = false;
  std::string pattern;
};

TpchQ2LikeAccelerator TpchQ2BuildLikeAccelerator(const std::string &pattern);
bool TpchQ2LikeAcceleratedMatch(const TpchQ2LikeAccelerator &acc,
                                const std::string &text);

enum class TpchQ2ImageKind : uint8_t { Full };

struct TpchQ2StateImage {
  TpchQ2ImageKind kind = TpchQ2ImageKind::Full;
  std::shared_ptr<const TpchQ2State> state;
};

struct TpchQ2PartsuppDelta {
  int64_t partkey = 0;
  int64_t suppkey = 0;
  int64_t supplycost_raw = 0;
  int8_t sign = 0;
};

std::vector<uint8_t> EncodeTpchQ2State(const TpchQ2State &);
bool DecodeTpchQ2State(const std::vector<uint8_t> &, TpchQ2State &);

bool TpchQ2MatchesLike(const std::string &text, const std::string &pattern);

void TpchQ2FinalizeAndEmit(const TpchQ2State &state, int32_t part_size,
                           const std::string &part_type_pattern,
                           const std::string &region_name,
                           core::AuxRowSink &sink);

constexpr std::size_t kTpchQ2ResultLimit = 100;

class TpchQ2TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ2PartsuppDelta> &PartsuppDeltas() const;
  void MarkRebuild();
  void AddPartsuppDelta(TpchQ2PartsuppDelta delta);

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ2PartsuppDelta> partsupp_deltas;
};

class TpchQ2Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = "tpch_q2";

  explicit TpchQ2Module(TpchQ2Config config,
                        TpchQ2InitialScan *initial_scan = nullptr)
      : config(config), initial_scan(initial_scan) {}

  core::AcceleratorId Id() const override;
  std::vector<core::TableDependency> Dependencies() const override;
  std::unique_ptr<core::AuxTxnBuffer> CreateTxnBuffer() const override;
  void Build(core::AuxEngineServices &, core::StateBuilder &) override;
  core::InvalidationPlan
  InterpretMutation(const core::MutationBatch &) const override;
  void AccumulateMutation(const core::MutationBatch &, core::AuxEngineServices &,
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
  TpchQ2State BuildState(core::AuxEngineServices &) const;
  std::shared_ptr<const TpchQ2State>
  LoadState(core::AuxEngineServices &) const;
  void EmitResult(const TpchQ2State &, int32_t part_size,
                  const std::string &part_type_pattern,
                  const std::string &region_name, core::AuxRowSink &) const;

  TpchQ2Config config;
  TpchQ2InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
