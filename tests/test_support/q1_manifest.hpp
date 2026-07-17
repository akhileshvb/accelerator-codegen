#pragma once

#include <string>
#include <vector>

namespace accelerator_codegen::test_support {

struct Q1ManifestEntry {
    const char *label;
    const char *sql_path;
    int day_offset;
};

inline const std::vector<Q1ManifestEntry> kQ1Manifest = {
    {"seed_1_q1", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_1/01.sql", 103},
    {"seed_2_q1", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_2/01.sql", 105},
    {"seed_3_q1", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_3/01.sql", 90},
    {"seed_4_q1", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_4/01.sql", 98},
    {"seed_5_q1", "/home/akhilvb/tpch-utils/generated/sf1_instances/seed_5/01.sql", 104},
};

} // namespace accelerator_codegen::test_support
