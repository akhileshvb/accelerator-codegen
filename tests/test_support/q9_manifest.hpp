#pragma once

#include <array>

namespace accelerator_codegen::test_support {

struct Q9ManifestEntry {
	const char *label;
	const char *sql_path;
	const char *color;
};

inline constexpr std::array<Q9ManifestEntry, 20> kQ9Manifest = {{
    {"seed_1_q9", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_1/09.sql", "powder"},
    {"seed_2_q9", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_2/09.sql", "medium"},
    {"seed_3_q9", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_3/09.sql", "cornsilk"},
    {"seed_4_q9", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_4/09.sql", "slate"},
    {"seed_5_q9", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_5/09.sql", "pink"},
    {"seed_6_q9", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_6/09.sql", "blanched"},
    {"seed_7_q9", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_7/09.sql", "burnished"},
    {"seed_8_q9", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_8/09.sql", "cyan"},
    {"seed_9_q9", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_9/09.sql", "cyan"},
    {"seed_10_q9", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_10/09.sql", "aquamarine"},
    {"seed_11_q9", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_11/09.sql", "frosted"},
    {"seed_12_q9", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_12/09.sql", "dim"},
    {"seed_13_q9", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_13/09.sql", "salmon"},
    {"seed_14_q9", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_14/09.sql", "mint"},
    {"seed_15_q9", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_15/09.sql", "floral"},
    {"seed_16_q9", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_16/09.sql", "rose"},
    {"seed_17_q9", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_17/09.sql", "brown"},
    {"seed_18_q9", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_18/09.sql", "wheat"},
    {"seed_19_q9", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_19/09.sql", "lemon"},
    {"seed_20_q9", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_20/09.sql", "ghost"},
}};

} // namespace accelerator_codegen::test_support
