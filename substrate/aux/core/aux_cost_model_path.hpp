// SCAFFOLDING
#pragma once

#include <string>
#include <vector>

namespace accelerator_codegen::aux_state::core {

// Where a query keeps its maintenance cost models (LoadAuxCostModelJson):
// <dir>/<stem>[_<engine>][_<scale>].json. Each query declares it once, and
// every engine's runtime looks its models up from it. `dir` is relative to
// the working directory (as the engines' search directories are).
struct AuxCostModelLocation {
  const char *dir = nullptr;  // e.g. "config"
  const char *stem = nullptr; // e.g. "q6_aux_cost_model"
};

// How an engine looks a query's model up. In each directory, in order, the
// first file that exists wins, most specific first:
//   <stem>_<engine>_<scale>.json, <stem>_<engine>.json, <stem>_<scale>.json,
//   <stem>.json
// A name with a scale falls back to the nearest smaller calibrated scale of
// the same prefix (sf20 -> sf1 when no sf20 file exists), before the next,
// less specific name. (an empty engine or scale drops the names that carry it).
struct AuxCostModelLookup {
  std::string engine; // "postgres", "mysql"; "": no engine-specific models
  std::string scale;  // "sf20": models calibrated at that scale; "": none
  // Searched before the query's own directory, then after it, each with the
  // same file names; empty entries are skipped.
  std::vector<std::string> dirs_before;
  std::vector<std::string> dirs_after;
  // No file exists: <dir>/<stem>.json of the location (true) or "" (false).
  bool default_when_missing = false;
};

// <dir>/<stem>.json: the query's default model, for any engine and scale.
std::string AuxCostModelDefaultPath(const AuxCostModelLocation &location);

// The path of `location`'s cost model under `lookup` ("" when none exists and
// !default_when_missing; LoadAuxCostModelJson then keeps its fallback).
std::string ResolveAuxCostModelPath(const AuxCostModelLocation &location,
                                    const AuxCostModelLookup &lookup);

} // namespace accelerator_codegen::aux_state::core
