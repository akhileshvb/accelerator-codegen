#pragma once

#include <array>

namespace accelerator_codegen::test_support {

struct Q21ManifestEntry {
	const char *label;
	const char *sql_path;
	const char *nation_name;
};

inline constexpr std::array<Q21ManifestEntry, 20> kQ21Manifest = {{
    {"seed_1_q21", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_1/21.sql", "ALGERIA"},
    {"seed_2_q21", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_2/21.sql", "CHINA"},
    {"seed_3_q21", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_3/21.sql", "MOZAMBIQUE"},
    {"seed_4_q21", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_4/21.sql", "IRAN"},
    {"seed_5_q21", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_5/21.sql", "PERU"},
    {"seed_6_q21", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_6/21.sql", "KENYA"},
    {"seed_7_q21", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_7/21.sql", "GERMANY"},
    {"seed_8_q21", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_8/21.sql", "CANADA"},
    {"seed_9_q21", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_9/21.sql", "ALGERIA"},
    {"seed_10_q21", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_10/21.sql", "FRANCE"},
    {"seed_11_q21", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_11/21.sql", "VIETNAM"},
    {"seed_12_q21", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_12/21.sql", "PERU"},
    {"seed_13_q21", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_13/21.sql", "INDONESIA"},
    {"seed_14_q21", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_14/21.sql", "SAUDI ARABIA"},
    {"seed_15_q21", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_15/21.sql", "GERMANY"},
    {"seed_16_q21", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_16/21.sql", "CHINA"},
    {"seed_17_q21", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_17/21.sql", "PERU"},
    {"seed_18_q21", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_18/21.sql", "MOZAMBIQUE"},
    {"seed_19_q21", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_19/21.sql", "GERMANY"},
    {"seed_20_q21", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_20/21.sql", "IRAN"},
}};

} // namespace accelerator_codegen::test_support
