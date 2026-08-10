#pragma once

#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace accelerator_codegen::aux_state::engine::postgres {

int32_t ResolveNationKey(const std::string &nation_name);
void FillRegionNations(const std::string &region_name, std::array<bool, 25> &region_nations);
void FillRegionNationsAndNames(const std::string &region_name, std::array<bool, 25> &region_nations,
                               std::unordered_map<int32_t, std::string> &nation_names);
void ResolvePartKeys(const std::string &part_type, std::vector<int64_t> &part_keys);

} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
