#pragma once

#include <array>
#include <cstdint>

namespace accelerator_codegen::test_support {

struct Q19ManifestEntry {
	const char *label;
	const char *sql_path;
	const char *brand1;
	int64_t qty_low1;
	const char *brand2;
	int64_t qty_low2;
	const char *brand3;
	int64_t qty_low3;
};

inline constexpr std::array<Q19ManifestEntry, 20> kQ39Manifest = {{
    {"seed_1_q19", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_1/19.sql", "Brand#31", 4, "Brand#12", 15, "Brand#51", 23},
    {"seed_2_q19", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_2/19.sql", "Brand#24", 5, "Brand#34", 16, "Brand#11", 24},
    {"seed_3_q19", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_3/19.sql", "Brand#24", 4, "Brand#35", 19, "Brand#45", 20},
    {"seed_4_q19", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_4/19.sql", "Brand#32", 9, "Brand#13", 20, "Brand#31", 27},
    {"seed_5_q19", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_5/19.sql", "Brand#53", 8, "Brand#11", 18, "Brand#52", 22},
    {"seed_6_q19", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_6/19.sql", "Brand#15", 8, "Brand#15", 20, "Brand#32", 24},
    {"seed_7_q19", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_7/19.sql", "Brand#43", 5, "Brand#53", 11, "Brand#14", 22},
    {"seed_8_q19", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_8/19.sql", "Brand#54", 5, "Brand#44", 18, "Brand#53", 27},
    {"seed_9_q19", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_9/19.sql", "Brand#31", 7, "Brand#11", 17, "Brand#43", 22},
    {"seed_10_q19", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_10/19.sql", "Brand#45", 5, "Brand#32", 15, "Brand#21", 22},
    {"seed_11_q19", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_11/19.sql", "Brand#52", 3, "Brand#22", 19, "Brand#14", 23},
    {"seed_12_q19", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_12/19.sql", "Brand#43", 6, "Brand#21", 13, "Brand#34", 27},
    {"seed_13_q19", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_13/19.sql", "Brand#22", 4, "Brand#42", 17, "Brand#11", 20},
    {"seed_14_q19", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_14/19.sql", "Brand#32", 7, "Brand#14", 16, "Brand#13", 25},
    {"seed_15_q19", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_15/19.sql", "Brand#25", 2, "Brand#23", 14, "Brand#41", 29},
    {"seed_16_q19", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_16/19.sql", "Brand#14", 8, "Brand#22", 10, "Brand#33", 27},
    {"seed_17_q19", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_17/19.sql", "Brand#23", 5, "Brand#42", 19, "Brand#34", 21},
    {"seed_18_q19", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_18/19.sql", "Brand#13", 8, "Brand#44", 14, "Brand#15", 29},
    {"seed_19_q19", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_19/19.sql", "Brand#34", 2, "Brand#21", 11, "Brand#15", 28},
    {"seed_20_q19", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_20/19.sql", "Brand#32", 5, "Brand#31", 10, "Brand#25", 29},
}};

inline constexpr const auto &kQ19Manifest = kQ39Manifest;

} // namespace accelerator_codegen::test_support
