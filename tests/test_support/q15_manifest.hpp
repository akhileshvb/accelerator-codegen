#pragma once

#include <array>

namespace accelerator_codegen::test_support {

struct Q15ManifestEntry {
	const char *label;
	const char *sql_path;
	const char *start_date;
};

inline constexpr std::array<Q15ManifestEntry, 20> kQ15Manifest = {{
    {"seed_1_q15", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_1/15.sql", "1994-11-01"},
    {"seed_2_q15", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_2/15.sql", "1994-06-01"},
    {"seed_3_q15", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_3/15.sql", "1994-02-01"},
    {"seed_4_q15", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_4/15.sql", "1995-07-01"},
    {"seed_5_q15", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_5/15.sql", "1997-10-01"},
    {"seed_6_q15", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_6/15.sql", "1994-09-01"},
    {"seed_7_q15", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_7/15.sql", "1996-06-01"},
    {"seed_8_q15", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_8/15.sql", "1997-10-01"},
    {"seed_9_q15", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_9/15.sql", "1994-10-01"},
    {"seed_10_q15", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_10/15.sql", "1994-11-01"},
    {"seed_11_q15", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_11/15.sql", "1996-03-01"},
    {"seed_12_q15", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_12/15.sql", "1996-07-01"},
    {"seed_13_q15", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_13/15.sql", "1997-10-01"},
    {"seed_14_q15", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_14/15.sql", "1996-01-01"},
    {"seed_15_q15", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_15/15.sql", "1994-05-01"},
    {"seed_16_q15", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_16/15.sql", "1997-10-01"},
    {"seed_17_q15", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_17/15.sql", "1993-02-01"},
    {"seed_18_q15", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_18/15.sql", "1993-08-01"},
    {"seed_19_q15", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_19/15.sql", "1997-10-01"},
    {"seed_20_q15", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_20/15.sql", "1994-10-01"},
}};

} // namespace accelerator_codegen::test_support
