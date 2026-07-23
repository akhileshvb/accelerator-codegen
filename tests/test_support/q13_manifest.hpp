#pragma once

#include <array>

namespace accelerator_codegen::test_support {

struct Q13ManifestEntry {
	const char *label;
	const char *sql_path;
	const char *comment_pattern;
};

inline constexpr std::array<Q13ManifestEntry, 20> kQ13Manifest = {{
    {"seed_1_q13", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_1/13.sql", "%unusual%accounts%"},
    {"seed_2_q13", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_2/13.sql", "%unusual%requests%"},
    {"seed_3_q13", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_3/13.sql", "%pending%packages%"},
    {"seed_4_q13", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_4/13.sql", "%express%packages%"},
    {"seed_5_q13", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_5/13.sql", "%special%requests%"},
    {"seed_6_q13", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_6/13.sql", "%unusual%requests%"},
    {"seed_7_q13", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_7/13.sql", "%unusual%deposits%"},
    {"seed_8_q13", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_8/13.sql", "%unusual%accounts%"},
    {"seed_9_q13", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_9/13.sql", "%special%requests%"},
    {"seed_10_q13", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_10/13.sql", "%unusual%deposits%"},
    {"seed_11_q13", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_11/13.sql", "%pending%accounts%"},
    {"seed_12_q13", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_12/13.sql", "%pending%deposits%"},
    {"seed_13_q13", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_13/13.sql", "%pending%deposits%"},
    {"seed_14_q13", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_14/13.sql", "%special%deposits%"},
    {"seed_15_q13", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_15/13.sql", "%pending%requests%"},
    {"seed_16_q13", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_16/13.sql", "%special%packages%"},
    {"seed_17_q13", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_17/13.sql", "%pending%requests%"},
    {"seed_18_q13", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_18/13.sql", "%pending%packages%"},
    {"seed_19_q13", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_19/13.sql", "%special%packages%"},
    {"seed_20_q13", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_20/13.sql", "%pending%accounts%"},
}};

} // namespace accelerator_codegen::test_support
