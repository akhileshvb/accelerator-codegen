#include "tpch/q21/verus/module.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace accelerator_codegen::aux_state::query {

namespace {

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
  if (std::holds_alternative<core::AuxDate>(cell.payload)) {
    out = std::get<core::AuxDate>(cell.payload).days_since_epoch;
    return true;
  }
  return false;
}

bool DateCell(const core::AuxRow &row, std::size_t index, int32_t &days) {
  int64_t value = 0;
  if (!IntCell(row, index, value) ||
      value < std::numeric_limits<int32_t>::min() ||
      value > std::numeric_limits<int32_t>::max())
    return false;
  days = static_cast<int32_t>(value);
  return true;
}

bool ReadLineitem(const core::AuxRow &row, q21v_li_row &out) {
  return IntCell(row, 0, out.orderkey) && IntCell(row, 1, out.suppkey) &&
         DateCell(row, 2, out.commitdate) && DateCell(row, 3, out.receiptdate);
}

// orders capture projects (o_orderkey, o_orderstatus).
bool ReadOrder(const core::AuxRow &row, q21v_ord_delta &out) {
  std::string status;
  if (!IntCell(row, 0, out.orderkey) || !StringCell(row, 1, status))
    return false;
  out.status_is_f = status == "F" ? 1 : 0;
  return true;
}

// supplier capture projects (s_suppkey, s_name, s_nationkey).
bool ReadSupplier(const core::AuxRow &row, TpchQ21VerusDimDelta &out) {
  int64_t nk = 0;
  if (!IntCell(row, 0, out.key) || !StringCell(row, 1, out.name) ||
      !IntCell(row, 2, nk) || nk < std::numeric_limits<int32_t>::min() ||
      nk > std::numeric_limits<int32_t>::max())
    return false;
  out.nationkey = static_cast<int32_t>(nk);
  return true;
}

// nation capture projects (n_nationkey, n_name).
bool ReadNation(const core::AuxRow &row, TpchQ21VerusDimDelta &out) {
  int64_t nk = 0;
  if (!IntCell(row, 0, nk) || !StringCell(row, 1, out.name) ||
      nk < std::numeric_limits<int32_t>::min() ||
      nk > std::numeric_limits<int32_t>::max())
    return false;
  out.key = nk;
  out.nationkey = static_cast<int32_t>(nk);
  return true;
}

core::InvalidationPlan All() {
  core::InvalidationPlan plan;
  plan.scope = ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions;
  return plan;
}

void Rebuild(core::StagedMaintenanceOutcome &outcome) {
  outcome.action = core::MaintenanceAction::RebuildAll;
  outcome.invalidations = All();
  outcome.pending_rebuild = outcome.invalidations;
}

const TpchQ21VerusImage *ReadImage(core::AuxEngineServices &services,
                                   const core::VisibilityToken &visibility) {
  const auto snapshot = services.StateStore().ReadPartitions(
      visibility, {TpchQ21VerusLayout::kState});
  for (const auto &image : snapshot.partitions) {
    if (image.partition == TpchQ21VerusLayout::kState)
      return image.As<TpchQ21VerusImage>();
  }
  return nullptr;
}

struct ResultDeleter {
  void operator()(q21v_result *r) const { q21v_result_free(r); }
};

} // namespace

core::AcceleratorId TpchQ21VerusModule::Id() const { return kId; }

std::vector<core::TableDependency> TpchQ21VerusModule::Dependencies() const {
  return {{config.supplier_table, {0, 1, 2}, {0, 1, 3}},
          {config.orders_table, {0, 1}, {0, 2}},
          {config.lineitem_table, {0, 1, 2, 3}, {0, 2, 11, 12}},
          {config.nation_table, {0, 1}, {0, 1}}};
}

std::unique_ptr<core::AuxTxnBuffer> TpchQ21VerusModule::CreateTxnBuffer() const {
  return std::make_unique<TpchQ21VerusTxnBuffer>();
}

q21v_state *TpchQ21VerusModule::BuildKernelState() const {
  if (!scan)
    throw std::logic_error("TPC-H Q21 (Verus) requires a scan source");
  std::vector<int64_t> sup_sk;
  std::vector<int32_t> sup_nk;
  std::vector<uint8_t> sup_names;
  std::vector<size_t> sup_off{0};
  scan->ScanSuppliers([&](int64_t sk, const std::string &name, int32_t nk) {
    sup_sk.push_back(sk);
    sup_nk.push_back(nk);
    sup_names.insert(sup_names.end(), name.begin(), name.end());
    sup_off.push_back(sup_names.size());
  });
  std::vector<int32_t> nat_nk;
  std::vector<uint8_t> nat_names;
  std::vector<size_t> nat_off{0};
  scan->ScanNations([&](int32_t nk, const std::string &name) {
    nat_nk.push_back(nk);
    nat_names.insert(nat_names.end(), name.begin(), name.end());
    nat_off.push_back(nat_names.size());
  });
  std::vector<int64_t> fkeys;
  scan->ScanFOrderKeys([&](int64_t ok) { fkeys.push_back(ok); });
  std::vector<q21v_li_row> lis;
  scan->ScanFLineitems([&](const q21v_li_row &row) { lis.push_back(row); });
  return q21v_build(sup_sk.size(), sup_sk.data(), sup_nk.data(),
                    sup_names.data(), sup_off.data(), nat_nk.size(),
                    nat_nk.data(), nat_names.data(), nat_off.data(),
                    fkeys.size(), fkeys.data(), lis.size(), lis.data());
}

void TpchQ21VerusModule::Build(core::AuxEngineServices &,
                               core::StateBuilder &builder) {
  auto image = std::make_shared<const TpchQ21VerusImage>(BuildKernelState());
  builder.Write(core::StateImage::Make(TpchQ21VerusLayout::kAccelerator,
                                       TpchQ21VerusLayout::kState, 0,
                                       std::move(image)));
  builder.Finish();
}

core::InvalidationPlan
TpchQ21VerusModule::InterpretMutation(const core::MutationBatch &mutation) const {
  if (mutation.table == config.lineitem_table ||
      mutation.table == config.orders_table ||
      mutation.table == config.supplier_table ||
      mutation.table == config.nation_table) {
    core::InvalidationPlan plan;
    plan.scope = ::accelerator_codegen::aux_state::InvalidationScope::PartitionSet;
    return plan;
  }
  return {};
}

void TpchQ21VerusModule::AccumulateMutation(const core::MutationBatch &mutation,
                                            core::AuxEngineServices &,
                                            core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ21VerusTxnBuffer &>(base_buffer);
  if (buffer.requires_rebuild)
    return;
  const bool is_orders = mutation.table == config.orders_table;
  const bool is_supplier = mutation.table == config.supplier_table;
  const bool is_nation = mutation.table == config.nation_table;
  if (!is_orders && !is_supplier && !is_nation &&
      mutation.table != config.lineitem_table)
    return;
  // Every change is a signed row: deletes (and the old side of updates) -1,
  // inserts (and the new side of updates) +1.
  auto add = [&](const core::AuxRow &row, int64_t sign) {
    if (is_supplier || is_nation) {
      TpchQ21VerusDimDelta d;
      if (!(is_supplier ? ReadSupplier(row, d) : ReadNation(row, d)))
        return false;
      if (scan)
        scan->NormalizeCapturedName(is_supplier, d.name);
      d.sign = sign;
      (is_supplier ? buffer.sd : buffer.nd).push_back(std::move(d));
    } else if (is_orders) {
      q21v_ord_delta d{};
      if (!ReadOrder(row, d))
        return false;
      d.sign = sign;
      buffer.od.push_back(d);
    } else {
      q21v_li_delta d{};
      if (!ReadLineitem(row, d.row))
        return false;
      d.sign = sign;
      buffer.ld.push_back(d);
    }
    buffer.pending = true;
    return true;
  };
  for (const auto &row : mutation.old_rows) {
    if ((mutation.kind == core::MutationKind::Delete ||
         mutation.kind == core::MutationKind::Update) &&
        !add(row, -1)) {
      buffer.MarkRebuild();
      return;
    }
  }
  for (const auto &row : mutation.new_rows) {
    if ((mutation.kind == core::MutationKind::Insert ||
         mutation.kind == core::MutationKind::Update) &&
        !add(row, +1)) {
      buffer.MarkRebuild();
      return;
    }
  }
}

core::StagedMaintenanceOutcome
TpchQ21VerusModule::StageMaintenance(core::AuxEngineServices &services,
                                     const core::InvalidationPlan &,
                                     core::AuxTxnBuffer &base_buffer) {
  auto &buffer = dynamic_cast<TpchQ21VerusTxnBuffer &>(base_buffer);
  core::StagedMaintenanceOutcome outcome;

  // Same policy consultation as the C++ Q21 module.
  if (buffer.requires_rebuild) {
    const auto decision = services.ChooseMaintenance(
        {0, 0, false, false, true, true, false, core::MaintenanceAction::NoOp,
         services.ExpectedQueriesBeforeNextWrite()});
    outcome.action = decision.action == core::MaintenanceAction::ApplyDelta
                         ? core::MaintenanceAction::RebuildAll
                         : decision.action;
    outcome.invalidations = All();
    outcome.pending_rebuild = outcome.invalidations;
    return outcome;
  }
  if (buffer.od.empty() && buffer.ld.empty() && buffer.sd.empty() &&
      buffer.nd.empty())
    return outcome;
  const auto total = buffer.od.size() + buffer.ld.size() + buffer.sd.size() +
                     buffer.nd.size();
  const auto decision = services.ChooseMaintenance(
      {static_cast<uint64_t>(total), 1, true, false, true, false, false,
       core::MaintenanceAction::NoOp, services.ExpectedQueriesBeforeNextWrite()});
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

  const auto *image = ReadImage(services, services.CurrentVisibility());
  if (!image || !image->state) {
    Rebuild(outcome);
    return outcome;
  }
  // Orders that become 'F' without being tracked: scan their lineitems as of
  // this transaction's end.
  std::vector<int64_t> keys(buffer.od.size());
  keys.resize(q21v_needs_scan(image->state, buffer.od.size(), buffer.od.data(),
                              keys.data()));
  std::vector<q21v_li_row> rows;
  if (!keys.empty())
    scan->ScanLineitemsForKeys(keys, [&](const q21v_li_row &r) { rows.push_back(r); });
  // Supplier / nation deltas as parallel arrays (names concatenated).
  auto flatten = [](const std::vector<TpchQ21VerusDimDelta> &ds,
                    std::vector<int64_t> &key, std::vector<int32_t> &nk,
                    std::vector<uint8_t> &names, std::vector<size_t> &off,
                    std::vector<int64_t> &sign) {
    off.assign(1, 0);
    for (const auto &d : ds) {
      key.push_back(d.key);
      nk.push_back(d.nationkey);
      names.insert(names.end(), d.name.begin(), d.name.end());
      off.push_back(names.size());
      sign.push_back(d.sign);
    }
  };
  std::vector<int64_t> sd_sk, sd_sign, nd_key, nd_sign;
  std::vector<int32_t> sd_nk, nd_nk;
  std::vector<uint8_t> sd_names, nd_names;
  std::vector<size_t> sd_off, nd_off;
  flatten(buffer.sd, sd_sk, sd_nk, sd_names, sd_off, sd_sign);
  flatten(buffer.nd, nd_key, nd_nk, nd_names, nd_off, nd_sign);
  int32_t err = 0;
  q21v_state *next = q21v_apply(
      image->state, buffer.od.size(), buffer.od.data(), buffer.ld.size(),
      buffer.ld.data(), buffer.sd.size(), sd_sk.data(), sd_nk.data(),
      sd_names.data(), sd_off.data(), sd_sign.data(), buffer.nd.size(),
      nd_nk.data(), nd_names.data(), nd_off.data(), nd_sign.data(), keys.size(),
      keys.data(), rows.size(), rows.data(), &err);
  if (!next) {
    Rebuild(outcome);
    return outcome;
  }
  outcome.action = core::MaintenanceAction::ApplyDelta;
  outcome.updates.push_back(core::StateImage::Make(
      TpchQ21VerusLayout::kAccelerator, TpchQ21VerusLayout::kState, 0,
      std::make_shared<const TpchQ21VerusImage>(next)));
  return outcome;
}

bool TpchQ21VerusModule::CheckValidity(core::AuxEngineServices &services,
                                       const core::QueryArguments &arguments) const {
  if (arguments.values.empty())
    return false;
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ21VerusModule::Execute(core::AuxEngineServices &services,
                                 const core::QueryArguments &arguments,
                                 core::AuxRowSink &sink) {
  if (arguments.values.empty() || arguments.values[0].is_null ||
      !std::holds_alternative<std::string>(arguments.values[0].payload) ||
      services.StateStore().IsTransactionDirty()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  const auto &nation = std::get<std::string>(arguments.values[0].payload);
  const auto visibility = services.CurrentVisibility();
  if (!services.StateStore().CheckValidity({Id(), {}}, visibility).IsValid()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  const auto *image = ReadImage(services, visibility);
  if (!image || !image->state) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  std::unique_ptr<q21v_result, ResultDeleter> result(q21v_execute(
      image->state, reinterpret_cast<const uint8_t *>(nation.data()),
      nation.size()));
  if (!result) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  sink.Begin(core::OutputSchema{});
  const size_t n = q21v_result_len(result.get());
  core::AuxRow out;
  for (size_t i = 0; i < n; ++i) {
    size_t len = 0;
    const uint8_t *name = q21v_result_name(result.get(), i, &len);
    out.values.clear();
    out.values.push_back(core::AuxValue::Of(
        core::AuxType::String,
        std::string(reinterpret_cast<const char *>(name), len)));
    out.values.push_back(
        core::AuxValue::Of(core::AuxType::Int64, q21v_result_count(result.get(), i)));
    sink.WriteRow(out);
  }
  sink.End();
}

void TpchQ21VerusModule::ExecuteFallback(core::AuxEngineServices &,
                                         const core::QueryArguments &arguments,
                                         core::AuxRowSink &sink) {
  sink.Begin(core::OutputSchema{});
  // n_name = NULL matches no row: a NULL (or missing) parameter has no groups.
  if (arguments.values.empty() || arguments.values[0].is_null ||
      !std::holds_alternative<std::string>(arguments.values[0].payload)) {
    sink.End();
    return;
  }
  const auto &nation = std::get<std::string>(arguments.values[0].payload);
  core::AuxRow out;
  scan->RunReference(nation, [&](const std::string &name, int64_t numwait) {
    out.values.clear();
    out.values.push_back(core::AuxValue::Of(core::AuxType::String, name));
    out.values.push_back(core::AuxValue::Of(core::AuxType::Int64, numwait));
    sink.WriteRow(out);
  });
  sink.End();
}

} // namespace accelerator_codegen::aux_state::query
