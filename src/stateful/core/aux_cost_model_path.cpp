// SCAFFOLDING
#include "accelerator_codegen/stateful/core/aux_cost_model_path.hpp"

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
  std::vector<std::string> names;
  if (!lookup.engine.empty()) {
    if (!lookup.scale.empty()) {
      names.push_back(stem + "_" + lookup.engine + "_" + lookup.scale + ".json");
    }
    names.push_back(stem + "_" + lookup.engine + ".json");
  }
  if (!lookup.scale.empty()) {
    names.push_back(stem + "_" + lookup.scale + ".json");
  }
  names.push_back(stem + ".json");

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
      const auto path = InDir(in, name);
      if (FileReadable(path)) {
        return path;
      }
    }
  }
  return lookup.default_when_missing ? AuxCostModelDefaultPath(location) : "";
}

} // namespace accelerator_codegen::aux_state::core
