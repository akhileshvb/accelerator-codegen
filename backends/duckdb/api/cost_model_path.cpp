// GENERATED BACKEND
#include "backends/duckdb/api/cost_model_path.hpp"

#include <cstdlib>
#include <string>

#include "substrate/aux/core/aux_cost_model_path.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {

namespace core = accelerator_codegen::aux_state::core;

std::string SharedCostModelConfigPath(const SharedQueryCatalogEntry &entry) {
  const char *override_path =
      std::getenv("ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL");
  if (override_path && *override_path) {
    return override_path;
  }
  core::AuxCostModelLookup lookup;
  // ACCEL_AUX_BENCH_DB_LABEL=<workload>_sf<X> (e.g. tpch_sf20): the models
  // calibrated at that scale, <stem>_sf<X>.json, when they exist; otherwise
  // the default <stem>.json
  const char *db_label = std::getenv("ACCEL_AUX_BENCH_DB_LABEL");
  const std::string label = db_label ? db_label : "";
  const std::string prefix = std::string(entry.workload_name ? entry.workload_name : "") + "_";
  if (prefix.size() > 1 && label.rfind(prefix + "sf", 0) == 0 && label.size() > prefix.size() + 2) {
    lookup.scale = label.substr(prefix.size());
  }
  // ACCEL_AUX_COST_MODEL_DIR: a subdirectory of the query's config dir with
  // models of the same names (e.g. verus: <bench>/q<N>/config/verus for the
  // Verus kits), searched first; a query it has no model
  // for falls back to its own directory
  if (const char *dir = std::getenv("ACCEL_AUX_COST_MODEL_DIR"); dir && *dir) {
    const std::string own = entry.cost_model.dir ? entry.cost_model.dir : "";
    lookup.dirs_before.push_back(own.empty() ? std::string(dir) : own + "/" + dir);
  }
  lookup.default_when_missing = true;
  return core::ResolveAuxCostModelPath(entry.cost_model, lookup);
}

} // namespace accelerator_codegen::aux_state::engine::duckdb
