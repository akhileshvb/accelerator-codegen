#include "sec_edgar/q4/duckdb/aux/parallel/module.hpp"

#include "accelerator_codegen/runtime/parallel/aux_parallel_scan.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace accelerator_codegen::aux_state::query {
namespace {

struct Totals {
  std::vector<double> hi, lo;
  std::vector<int64_t> nc;
  void Ensure(size_t n) {
    if (hi.empty()) {
      hi.assign(n, 0.0);
      lo.assign(n, 0.0);
      nc.assign(n, 0);
    }
  }
};

template <typename T>
const T *Data(const duckdb::UnifiedVectorFormat &f) {
  return duckdb::UnifiedVectorFormat::GetData<T>(f);
}

} // namespace

SecEdgarQ4State
SecEdgarQ4ParallelModule::BuildState(core::AuxEngineServices &services) const {
  if (!initial_scan) {
    throw std::logic_error("SEC EDGAR Q4 parallel requires a scan source");
  }
  // 1. Qualifying keys (pre x sub x tag join, ~65K rows) without num totals.
  std::vector<SecEdgarQ4BridgeRow> rows;
  SecEdgarQ4BridgeFilter filter;
  filter.include_num = false;
  initial_scan->ScanBridge(filter, [&](const SecEdgarQ4BridgeRow &row) {
    rows.push_back(row);
  });
  // Entry id == row index (the state keeps scan order).
  const SecEdgarQ4State probe = SecEdgarQ4StateFromBridgeRows(rows);
  const auto &cohort = *probe.cohort;
  const auto &bridge = *probe.bridge;

  // 2. Parallel num aggregation into per-worker totals.
  const auto workers = std::max<std::size_t>(1, services.WorkerCount());
  std::vector<Totals> local(workers);
  const size_t n = rows.size();
  auto consume = [&](duckdb::idx_t worker, duckdb::DataChunk &chunk) {
    const auto count = chunk.size();
    duckdb::UnifiedVectorFormat a, t, v, u, x;
    chunk.data[0].ToUnifiedFormat(count, a);
    chunk.data[1].ToUnifiedFormat(count, t);
    chunk.data[2].ToUnifiedFormat(count, v);
    chunk.data[3].ToUnifiedFormat(count, u);
    chunk.data[4].ToUnifiedFormat(count, x);
    auto &totals = local[worker];
    std::string key;
    for (duckdb::idx_t r = 0; r < count; ++r) {
      const auto ui = u.sel->get_index(r);
      const auto xi = x.sel->get_index(r);
      if (!u.validity.RowIsValid(ui) || !x.validity.RowIsValid(xi)) continue;
      const auto &uom = Data<duckdb::string_t>(u)[ui];
      if (uom.GetSize() != 3 || std::memcmp(uom.GetData(), "USD", 3) != 0) continue;
      const auto ai = a.sel->get_index(r);
      const auto ti = t.sel->get_index(r);
      const auto vi = v.sel->get_index(r);
      if (!a.validity.RowIsValid(ai) || !t.validity.RowIsValid(ti) ||
          !v.validity.RowIsValid(vi)) {
        continue;
      }
      const auto &adsh = Data<duckdb::string_t>(a)[ai];
      const uint32_t adsh_id = cohort.adsh.Find(adsh.GetData(), adsh.GetSize());
      if (adsh_id == kQ4None) continue;
      const auto &tag = Data<duckdb::string_t>(t)[ti];
      const auto &version = Data<duckdb::string_t>(v)[vi];
      key.assign(tag.GetData(), tag.GetSize());
      key.push_back('\0');
      key.append(version.GetData(), version.GetSize());
      const uint32_t tv_id = bridge.tv_dict->Find(key);
      if (tv_id == kQ4None) continue;
      const uint32_t entry = bridge.Lookup(adsh_id, tv_id);
      if (entry == kQ4None) continue;
      totals.Ensure(n);
      Q4DDAdd(totals.hi[entry], totals.lo[entry], Data<double>(x)[xi]);
      totals.nc[entry] += 1;
    }
  };
  const bool scanned = ScanSecEdgarParallel(
      services, SecEdgarNumScan({"adsh", "tag", "version", "uom", "value"}), consume);
  if (!scanned) {
    // No native parallel context: aggregate through the portable scan.
    SecEdgarQ4BridgeFilter all;
    all.include_num = true;
    rows.clear();
    initial_scan->ScanBridge(all, [&](const SecEdgarQ4BridgeRow &row) {
      rows.push_back(row);
    });
    return SecEdgarQ4StateFromBridgeRows(rows);
  }
  // 3. Merge and publish.
  for (size_t e = 0; e < n; ++e) {
    double hi = 0.0, lo = 0.0;
    int64_t nc = 0;
    for (auto &totals : local) {
      if (totals.hi.empty()) continue;
      Q4DDAdd(hi, lo, totals.hi[e]);
      Q4DDAdd(hi, lo, totals.lo[e]);
      nc += totals.nc[e];
    }
    rows[e].ns = hi;
    rows[e].ns_lo = lo;
    rows[e].nc = nc;
  }
  return SecEdgarQ4StateFromBridgeRows(rows);
}

} // namespace accelerator_codegen::aux_state::query
