#pragma once

#include <array>
#include <cstdint>

namespace accelerator_codegen::test_support {

struct Q1ManifestEntry {
	const char *label;
	const char *sql_path;
	int32_t day_offset;
};

inline constexpr std::array<Q1ManifestEntry, 5> kQ1Manifest = {{
    {"seed_1_q1", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_1/01.sql", 103},
    {"seed_2_q1", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_2/01.sql", 105},
    {"seed_3_q1", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_3/01.sql", 90},
    {"seed_4_q1", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_4/01.sql", 98},
    {"seed_5_q1", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_5/01.sql", 104},
}};

// Kept as a compatibility alias for the original full-query Q1 test.

} // namespace accelerator_codegen::test_support
