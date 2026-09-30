#pragma once

// TPC-H Q21 aux module backed by the Verus-verified kernel (src/verus/q21).
// The host store keeps the kernel's immutable state as the payload of one
// partition; this module only moves rows between the engine and the kernel.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"
#include "accelerator_codegen/verus/q21_verus.h"

namespace accelerator_codegen::aux_state::query {

struct TpchQ21VerusLayout {
  static constexpr const char *kAccelerator = "tpch_q21_verus";
  static constexpr core::PartitionId kState = 0;
};

struct TpchQ21VerusConfig {
  core::TableId supplier_table = 0;
  core::TableId orders_table = 0;
  core::TableId lineitem_table = 0;
  core::TableId nation_table = 0;
};

// Engine scans.  Each must implement the SQL named in its comment exactly;
// these are the trusted inputs of the kernel's proofs.
//
// s_name / n_name (CHAR(25)) are delivered in the form the engine compares
// them (spec.rs, host contract): a space-padded CHAR whose trailing spaces
// the engine ignores (PostgreSQL bpchar) comes without them, as its
// CHAR -> text cast gives it; anything else comes unchanged.
class TpchQ21VerusScan {
public:
  virtual ~TpchQ21VerusScan() = default;
  // SELECT s_suppkey, s_name, s_nationkey FROM supplier
  virtual void ScanSuppliers(
      const std::function<void(int64_t, const std::string &, int32_t)> &) = 0;
  // SELECT n_nationkey, n_name FROM nation
  virtual void
  ScanNations(const std::function<void(int32_t, const std::string &)> &) = 0;
  // Put a captured s_name (supplier) or n_name value, in the engine's text
  // form, into the form the scans above deliver.
  virtual void NormalizeCapturedName(bool supplier, std::string &name) const {
    (void)supplier;
    (void)name;
  }
  // SELECT o_orderkey FROM orders WHERE o_orderstatus = 'F'
  virtual void ScanFOrderKeys(const std::function<void(int64_t)> &) = 0;
  // SELECT l_orderkey, l_suppkey, l_commitdate, l_receiptdate FROM lineitem l
  //  WHERE EXISTS (SELECT 1 FROM orders o
  //                WHERE o.o_orderkey = l.l_orderkey AND o.o_orderstatus = 'F')
  virtual void ScanFLineitems(const std::function<void(const q21v_li_row &)> &) = 0;
  // Same columns, WHERE l_orderkey = ANY(keys), on the database including
  // this transaction's own writes.
  virtual void ScanLineitemsForKeys(const std::vector<int64_t> &keys,
                                    const std::function<void(const q21v_li_row &)> &) = 0;
  // The catalog SQL itself (fallback path), rows in ORDER BY order, at most
  // 100 (LIMIT 100); s_name in the same form as ScanSuppliers.
  virtual void RunReference(const std::string &nation,
                            const std::function<void(const std::string &, int64_t)> &) = 0;
};

// One kernel state, owned by the host store's shared_ptr payload.
struct TpchQ21VerusImage {
  explicit TpchQ21VerusImage(q21v_state *s) : state(s) {}
  ~TpchQ21VerusImage() { q21v_free(state); }
  TpchQ21VerusImage(const TpchQ21VerusImage &) = delete;
  TpchQ21VerusImage &operator=(const TpchQ21VerusImage &) = delete;
  q21v_state *state = nullptr; // null: outside the kernel's domain
};

// A captured supplier / nation row change (sign +1 insert, -1 delete).
struct TpchQ21VerusDimDelta {
  int64_t key = 0;     // s_suppkey (supplier) or n_nationkey (nation)
  int32_t nationkey = 0; // s_nationkey (supplier only)
  std::string name;
  int64_t sign = 0;
};

class TpchQ21VerusTxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override { return pending; }
  void Clear() override {
    pending = false;
    requires_rebuild = false;
    od.clear();
    ld.clear();
    sd.clear();
    nd.clear();
  }
  void MarkRebuild() {
    pending = true;
    requires_rebuild = true;
    od.clear();
    ld.clear();
    sd.clear();
    nd.clear();
  }
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<q21v_ord_delta> od;
  std::vector<q21v_li_delta> ld;
  std::vector<TpchQ21VerusDimDelta> sd;
  std::vector<TpchQ21VerusDimDelta> nd;
};

class TpchQ21VerusModule final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = TpchQ21VerusLayout::kAccelerator;

  explicit TpchQ21VerusModule(TpchQ21VerusConfig config,
                              TpchQ21VerusScan *scan = nullptr)
      : config(config), scan(scan) {}

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

private:
  q21v_state *BuildKernelState() const;

  TpchQ21VerusConfig config;
  TpchQ21VerusScan *scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
