// SCAFFOLDING
// Cost-model file lookup (see aux_cost_model_path.hpp).
#include "substrate/aux/core/aux_cost_model_path.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace accelerator_codegen::aux_state::core {
namespace {

bool FileReadable(const std::string &path) {
  std::ifstream input(path);
  return input.good();
}

std::string InDir(const std::string &dir, const std::string &name) {
  return dir.empty() ? name : dir + "/" + name;
}

// "sf20" -> ("sf", 20); false when the scale has no numeric tail.
bool SplitScale(const std::string &scale, std::string &prefix, double &value) {
  const auto digits = scale.find_first_of("0123456789");
  if (digits == std::string::npos) {
    return false;
  }
  char *end = nullptr;
  value = std::strtod(scale.c_str() + digits, &end);
  prefix = scale.substr(0, digits);
  return *end == '\0';
}

// <base>_<scale>.json in `dir`, else the file of the nearest smaller scale
// with the same prefix (<base>_sf1.json for sf20); "" if none.
std::string ScaledModelPath(const std::string &dir, const std::string &base,
                            const std::string &scale) {
  const auto exact = InDir(dir, base + "_" + scale + ".json");
  if (FileReadable(exact)) {
    return exact;
  }
  std::string prefix;
  double wanted = 0.0;
  if (!SplitScale(scale, prefix, wanted)) {
    return "";
  }
  const std::string head = base + "_" + prefix;
  std::string best;
  double best_value = -1.0;
  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator(dir.empty() ? "." : dir, ec)) {
    const auto name = entry.path().filename().string();
    if (name.rfind(head, 0) != 0 || name.size() <= head.size() + 5 ||
        name.compare(name.size() - 5, 5, ".json") != 0) {
      continue;
    }
    std::string found_prefix;
    double value = 0.0;
    if (SplitScale(name.substr(base.size() + 1, name.size() - base.size() - 6), found_prefix, value) &&
        found_prefix == prefix && value < wanted && value > best_value) {
      best_value = value;
      best = InDir(dir, name);
    }
  }
  return best;
}

} // namespace

std::string AuxCostModelDefaultPath(const AuxCostModelLocation &location) {
  return InDir(location.dir ? location.dir : "",
               std::string(location.stem ? location.stem : "") + ".json");
}

std::string ResolveAuxCostModelPath(const AuxCostModelLocation &location,
                                    const AuxCostModelLookup &lookup) {
  const std::string dir = location.dir ? location.dir : "";
  const std::string stem = location.stem ? location.stem : "";
  if (stem.empty()) {
    return "";
  }
  // Most specific first: a name with the scale falls back to the nearest
  // smaller calibrated scale before the next, less specific name.
  struct Name {
    std::string base;
    bool scaled;
  };
  std::vector<Name> names;
  if (!lookup.engine.empty()) {
    if (!lookup.scale.empty()) {
      names.push_back({stem + "_" + lookup.engine, true});
    }
    names.push_back({stem + "_" + lookup.engine, false});
  }
  if (!lookup.scale.empty()) {
    names.push_back({stem, true});
  }
  names.push_back({stem, false});

  std::vector<std::string> dirs;
  for (const auto &before : lookup.dirs_before) {
    if (!before.empty()) {
      dirs.push_back(before);
    }
  }
  dirs.push_back(dir);
  for (const auto &after : lookup.dirs_after) {
    if (!after.empty()) {
      dirs.push_back(after);
    }
  }
  for (const auto &in : dirs) {
    for (const auto &name : names) {
      const auto path = name.scaled ? ScaledModelPath(in, name.base, lookup.scale)
                                    : InDir(in, name.base + ".json");
      if (!path.empty() && FileReadable(path)) {
        return path;
      }
    }
  }
  return lookup.default_when_missing ? AuxCostModelDefaultPath(location) : "";
}

} // namespace accelerator_codegen::aux_state::core
