#pragma once

#include <array>
#include <cstdint>

namespace accelerator_codegen::test_support {

struct Q1ManifestEntry {
	const char *label;
	const char *sql_path;
	int32_t day_offset;
};

inline constexpr std::array<Q1ManifestEntry, 5> kQ3Manifest = {{
    {"seed_1_q1", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_1/01.sql", 103},
    {"seed_2_q1", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_2/01.sql", 103},
    {"seed_3_q1", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_3/01.sql", 103},
    {"seed_4_q1", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_4/01.sql", 103},
    {"seed_5_q1", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_5/01.sql", 103},
}};

} // namespace accelerator_codegen::test_support
