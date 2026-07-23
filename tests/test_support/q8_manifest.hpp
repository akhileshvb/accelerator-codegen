#pragma once

#include <array>

namespace accelerator_codegen::test_support {

struct Q8ManifestEntry {
	const char *label;
	const char *sql_path;
	const char *nation_name;
	const char *region_name;
	const char *part_type;
};

inline constexpr std::array<Q8ManifestEntry, 20> kQ8Manifest = {{
    {"seed_1_q8", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_1/08.sql", "INDIA", "ASIA", "BRASS"},
    {"seed_2_q8", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_2/08.sql", "UNITED KINGDOM", "EUROPE", "NICKEL"},
    {"seed_3_q8", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_3/08.sql", "JAPAN", "ASIA", "BRASS"},
    {"seed_4_q8", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_4/08.sql", "PERU", "AMERICA", "COPPER"},
    {"seed_5_q8", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_5/08.sql", "MOROCCO", "AFRICA", "STEEL"},
    {"seed_6_q8", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_6/08.sql", "MOZAMBIQUE", "AFRICA", "BRASS"},
    {"seed_7_q8", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_7/08.sql", "GERMANY", "EUROPE", "TIN"},
    {"seed_8_q8", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_8/08.sql", "ETHIOPIA", "AFRICA", "COPPER"},
    {"seed_9_q8", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_9/08.sql", "VIETNAM", "ASIA", "STEEL"},
    {"seed_10_q8", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_10/08.sql", "ALGERIA", "AFRICA", "BRASS"},
    {"seed_11_q8", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_11/08.sql", "KENYA", "AFRICA", "STEEL"},
    {"seed_12_q8", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_12/08.sql", "INDIA", "ASIA", "STEEL"},
    {"seed_13_q8", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_13/08.sql", "KENYA", "AFRICA", "TIN"},
    {"seed_14_q8", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_14/08.sql", "ALGERIA", "AFRICA", "COPPER"},
    {"seed_15_q8", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_15/08.sql", "GERMANY", "EUROPE", "NICKEL"},
    {"seed_16_q8", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_16/08.sql", "CANADA", "AMERICA", "COPPER"},
    {"seed_17_q8", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_17/08.sql", "INDONESIA", "ASIA", "TIN"},
    {"seed_18_q8", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_18/08.sql", "GERMANY", "EUROPE", "NICKEL"},
    {"seed_19_q8", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_19/08.sql", "JAPAN", "ASIA", "STEEL"},
    {"seed_20_q8", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_20/08.sql", "JAPAN", "ASIA", "BRASS"},
}};

} // namespace accelerator_codegen::test_support
