#pragma once

#include <array>
#include <cstdint>

namespace accelerator_codegen::test_support {

struct Q18ManifestEntry {
	const char *label;
	const char *sql_path;
	int64_t quantity_threshold;
};

inline constexpr std::array<Q18ManifestEntry, 20> kQ38Manifest = {{
    {"seed_1_q18", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_1/18.sql", 312},
    {"seed_2_q18", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_2/18.sql", 312},
    {"seed_3_q18", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_3/18.sql", 312},
    {"seed_4_q18", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_4/18.sql", 313},
    {"seed_5_q18", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_5/18.sql", 312},
    {"seed_6_q18", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_6/18.sql", 312},
    {"seed_7_q18", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_7/18.sql", 312},
    {"seed_8_q18", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_8/18.sql", 315},
    {"seed_9_q18", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_9/18.sql", 313},
    {"seed_10_q18", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_10/18.sql", 314},
    {"seed_11_q18", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_11/18.sql", 315},
    {"seed_12_q18", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_12/18.sql", 315},
    {"seed_13_q18", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_13/18.sql", 312},
    {"seed_14_q18", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_14/18.sql", 315},
    {"seed_15_q18", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_15/18.sql", 314},
    {"seed_16_q18", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_16/18.sql", 314},
    {"seed_17_q18", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_17/18.sql", 313},
    {"seed_18_q18", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_18/18.sql", 315},
    {"seed_19_q18", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_19/18.sql", 315},
    {"seed_20_q18", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_20/18.sql", 315},
}};

} // namespace accelerator_codegen::test_support
