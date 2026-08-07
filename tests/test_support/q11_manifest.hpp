#pragma once

#include <array>

namespace accelerator_codegen::test_support {

struct Q11ManifestEntry {
	const char *label;
	const char *sql_path;
	const char *nation_name;
	double fraction;
};

inline constexpr std::array<Q11ManifestEntry, 20> kQ31Manifest = {{
    {"seed_1_q11", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_1/11.sql", "ROMANIA", 0.0001},
    {"seed_2_q11", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_2/11.sql", "ARGENTINA", 0.0001},
    {"seed_3_q11", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_3/11.sql", "IRAQ", 0.0001},
    {"seed_4_q11", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_4/11.sql", "BRAZIL", 0.0001},
    {"seed_5_q11", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_5/11.sql", "RUSSIA", 0.0001},
    {"seed_6_q11", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_6/11.sql", "UNITED STATES", 0.0001},
    {"seed_7_q11", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_7/11.sql", "IRAN", 0.0001},
    {"seed_8_q11", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_8/11.sql", "UNITED KINGDOM", 0.0001},
    {"seed_9_q11", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_9/11.sql", "RUSSIA", 0.0001},
    {"seed_10_q11", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_10/11.sql", "SAUDI ARABIA", 0.0001},
    {"seed_11_q11", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_11/11.sql", "CANADA", 0.0001},
    {"seed_12_q11", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_12/11.sql", "CHINA", 0.0001},
    {"seed_13_q11", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_13/11.sql", "INDIA", 0.0001},
    {"seed_14_q11", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_14/11.sql", "VIETNAM", 0.0001},
    {"seed_15_q11", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_15/11.sql", "VIETNAM", 0.0001},
    {"seed_16_q11", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_16/11.sql", "ETHIOPIA", 0.0001},
    {"seed_17_q11", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_17/11.sql", "IRAQ", 0.0001},
    {"seed_18_q11", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_18/11.sql", "INDONESIA", 0.0001},
    {"seed_19_q11", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_19/11.sql", "VIETNAM", 0.0001},
    {"seed_20_q11", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_20/11.sql", "JAPAN", 0.0001},
}};

inline constexpr const auto &kQ11Manifest = kQ31Manifest;

} // namespace accelerator_codegen::test_support
