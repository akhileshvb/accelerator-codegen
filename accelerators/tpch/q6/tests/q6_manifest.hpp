#pragma once

#include <array>

namespace accelerator_codegen::test_support {

struct Q6ManifestEntry {
    const char *label;
    const char *sql_path;
    const char *start_date;
    const char *discount;
    const char *quantity;
};

inline constexpr std::array<Q6ManifestEntry, 20> kQ6Manifest = {{
    {"seed_1_q6", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_1/06.sql", "1995-01-01", "0.03", "25"},
    {"seed_2_q6", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_2/06.sql", "1995-01-01", "0.07", "24"},
    {"seed_3_q6", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_3/06.sql", "1994-01-01", "0.09", "25"},
    {"seed_4_q6", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_4/06.sql", "1995-01-01", "0.02", "25"},
    {"seed_5_q6", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_5/06.sql", "1994-01-01", "0.07", "24"},
    {"seed_6_q6", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_6/06.sql", "1996-01-01", "0.04", "24"},
    {"seed_7_q6", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_7/06.sql", "1997-01-01", "0.02", "25"},
    {"seed_8_q6", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_8/06.sql", "1996-01-01", "0.07", "24"},
    {"seed_9_q6", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_9/06.sql", "1997-01-01", "0.07", "24"},
    {"seed_10_q6", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_10/06.sql", "1994-01-01", "0.07", "25"},
    {"seed_11_q6", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_11/06.sql", "1993-01-01", "0.05", "25"},
    {"seed_12_q6", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_12/06.sql", "1993-01-01", "0.09", "25"},
    {"seed_13_q6", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_13/06.sql", "1994-01-01", "0.05", "24"},
    {"seed_14_q6", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_14/06.sql", "1993-01-01", "0.09", "24"},
    {"seed_15_q6", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_15/06.sql", "1995-01-01", "0.06", "25"},
    {"seed_16_q6", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_16/06.sql", "1997-01-01", "0.02", "25"},
    {"seed_17_q6", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_17/06.sql", "1996-01-01", "0.09", "24"},
    {"seed_18_q6", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_18/06.sql", "1994-01-01", "0.08", "25"},
    {"seed_19_q6", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_19/06.sql", "1996-01-01", "0.08", "24"},
    {"seed_20_q6", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_20/06.sql", "1994-01-01", "0.06", "24"},
}};

} // namespace accelerator_codegen::test_support
