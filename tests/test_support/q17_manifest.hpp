#pragma once

#include <array>

namespace accelerator_codegen::test_support {

struct Q17ManifestEntry {
	const char *label;
	const char *sql_path;
	const char *brand;
	const char *container;
};

inline constexpr std::array<Q17ManifestEntry, 20> kQ37Manifest = {{
    {"seed_1_q17", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_1/17.sql", "Brand#34", "LARGE BRUSHED"},
    {"seed_2_q17", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_2/17.sql", "Brand#24", "LARGE POLISHED"},
    {"seed_3_q17", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_3/17.sql", "Brand#41", "MEDIUM ANODIZED"},
    {"seed_4_q17", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_4/17.sql", "Brand#44", "ECONOMY ANODIZED"},
    {"seed_5_q17", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_5/17.sql", "Brand#15", "SMALL BRUSHED"},
    {"seed_6_q17", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_6/17.sql", "Brand#23", "LARGE PLATED"},
    {"seed_7_q17", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_7/17.sql", "Brand#32", "MEDIUM POLISHED"},
    {"seed_8_q17", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_8/17.sql", "Brand#25", "PROMO BURNISHED"},
    {"seed_9_q17", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_9/17.sql", "Brand#14", "PROMO POLISHED"},
    {"seed_10_q17", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_10/17.sql", "Brand#55", "SMALL POLISHED"},
    {"seed_11_q17", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_11/17.sql", "Brand#22", "PROMO POLISHED"},
    {"seed_12_q17", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_12/17.sql", "Brand#54", "STANDARD PLATED"},
    {"seed_13_q17", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_13/17.sql", "Brand#41", "MEDIUM BURNISHED"},
    {"seed_14_q17", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_14/17.sql", "Brand#51", "PROMO BRUSHED"},
    {"seed_15_q17", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_15/17.sql", "Brand#12", "LARGE ANODIZED"},
    {"seed_16_q17", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_16/17.sql", "Brand#31", "SMALL BRUSHED"},
    {"seed_17_q17", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_17/17.sql", "Brand#51", "MEDIUM POLISHED"},
    {"seed_18_q17", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_18/17.sql", "Brand#54", "LARGE BURNISHED"},
    {"seed_19_q17", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_19/17.sql", "Brand#44", "PROMO PLATED"},
    {"seed_20_q17", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_20/17.sql", "Brand#33", "SMALL POLISHED"},
}};

} // namespace accelerator_codegen::test_support
