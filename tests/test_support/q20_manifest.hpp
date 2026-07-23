#pragma once

#include <array>

namespace accelerator_codegen::test_support {

struct Q20ManifestEntry {
	const char *label;
	const char *sql_path;
	const char *start_date;
	const char *part_name_pattern;
	const char *nation_name;
};

inline constexpr std::array<Q20ManifestEntry, 20> kQ20Manifest = {{
    {"seed_1_q20", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_1/20.sql", "1997-01-01", "gainsboro%", "RUSSIA"},
    {"seed_2_q20", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_2/20.sql", "1995-01-01", "violet%", "ARGENTINA"},
    {"seed_3_q20", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_3/20.sql", "1994-01-01", "navajo%", "VIETNAM"},
    {"seed_4_q20", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_4/20.sql", "1997-01-01", "burlywood%", "CHINA"},
    {"seed_5_q20", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_5/20.sql", "1993-01-01", "navajo%", "UNITED KINGDOM"},
    {"seed_6_q20", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_6/20.sql", "1994-01-01", "papaya%", "CANADA"},
    {"seed_7_q20", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_7/20.sql", "1997-01-01", "chiffon%", "CANADA"},
    {"seed_8_q20", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_8/20.sql", "1994-01-01", "dark%", "MOROCCO"},
    {"seed_9_q20", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_9/20.sql", "1994-01-01", "rosy%", "INDIA"},
    {"seed_10_q20", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_10/20.sql", "1995-01-01", "blanched%", "INDIA"},
    {"seed_11_q20", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_11/20.sql", "1994-01-01", "smoke%", "PERU"},
    {"seed_12_q20", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_12/20.sql", "1994-01-01", "ghost%", "KENYA"},
    {"seed_13_q20", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_13/20.sql", "1997-01-01", "pale%", "JAPAN"},
    {"seed_14_q20", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_14/20.sql", "1995-01-01", "lawn%", "PERU"},
    {"seed_15_q20", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_15/20.sql", "1995-01-01", "lime%", "JORDAN"},
    {"seed_16_q20", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_16/20.sql", "1997-01-01", "sandy%", "VIETNAM"},
    {"seed_17_q20", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_17/20.sql", "1994-01-01", "brown%", "CHINA"},
    {"seed_18_q20", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_18/20.sql", "1993-01-01", "violet%", "ROMANIA"},
    {"seed_19_q20", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_19/20.sql", "1997-01-01", "rosy%", "VIETNAM"},
    {"seed_20_q20", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_20/20.sql", "1996-01-01", "violet%", "ROMANIA"},
}};

} // namespace accelerator_codegen::test_support
