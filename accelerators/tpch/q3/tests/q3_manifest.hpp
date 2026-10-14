#pragma once

#include <array>

namespace accelerator_codegen::test_support {

struct Q3ManifestEntry {
    const char *label;
    const char *sql_path;
    const char *market_segment;
    const char *cutoff_date;
};

inline constexpr std::array<Q3ManifestEntry, 20> kQ3Manifest = {{
    {"seed_1_q3", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_1/03.sql", "FURNITURE", "1995-03-10"},
    {"seed_2_q3", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_2/03.sql", "AUTOMOBILE", "1995-03-02"},
    {"seed_3_q3", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_3/03.sql", "MACHINERY", "1995-03-30"},
    {"seed_4_q3", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_4/03.sql", "FURNITURE", "1995-03-15"},
    {"seed_5_q3", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_5/03.sql", "FURNITURE", "1995-03-17"},
    {"seed_6_q3", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_6/03.sql", "BUILDING", "1995-03-27"},
    {"seed_7_q3", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_7/03.sql", "FURNITURE", "1995-03-24"},
    {"seed_8_q3", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_8/03.sql", "BUILDING", "1995-03-22"},
    {"seed_9_q3", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_9/03.sql", "AUTOMOBILE", "1995-03-18"},
    {"seed_10_q3", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_10/03.sql", "FURNITURE", "1995-03-20"},
    {"seed_11_q3", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_11/03.sql", "BUILDING", "1995-03-30"},
    {"seed_12_q3", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_12/03.sql", "MACHINERY", "1995-03-28"},
    {"seed_13_q3", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_13/03.sql", "AUTOMOBILE", "1995-03-20"},
    {"seed_14_q3", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_14/03.sql", "BUILDING", "1995-03-07"},
    {"seed_15_q3", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_15/03.sql", "FURNITURE", "1995-03-23"},
    {"seed_16_q3", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_16/03.sql", "AUTOMOBILE", "1995-03-15"},
    {"seed_17_q3", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_17/03.sql", "AUTOMOBILE", "1995-03-17"},
    {"seed_18_q3", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_18/03.sql", "BUILDING", "1995-03-20"},
    {"seed_19_q3", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_19/03.sql", "BUILDING", "1995-03-18"},
    {"seed_20_q3", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_20/03.sql", "AUTOMOBILE", "1995-03-03"},
}};

} // namespace accelerator_codegen::test_support
