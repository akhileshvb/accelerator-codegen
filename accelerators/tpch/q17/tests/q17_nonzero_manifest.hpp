#pragma once

#include "tpch/q17/tests/q17_manifest.hpp"

namespace accelerator_codegen::test_support {

//! SF1 cases with dbgen-matching brand/container pairs (nonzero qualifying parts).
//! Selected from part table pairs with >=5 parts; verified nonzero Q17 avg_yearly on tpch_sf1.duckdb.
inline constexpr std::array<Q17ManifestEntry, 10> kQ37NonzeroManifest = {{
    {"nonzero_42_lg_pack",
     "/home/akhilvb/accelerator-codegen/benchmarks/tpch/tmp/q17_sf1_nonzero_42_lg_pack.sql", "Brand#42", "LG PACK"},
    {"nonzero_52_lg_jar",
     "/home/akhilvb/accelerator-codegen/benchmarks/tpch/tmp/q17_sf1_nonzero_52_lg_jar.sql", "Brand#52", "LG JAR"},
    {"nonzero_54_med_pack",
     "/home/akhilvb/accelerator-codegen/benchmarks/tpch/tmp/q17_sf1_nonzero_54_med_pack.sql", "Brand#54", "MED PACK"},
    {"nonzero_51_lg_jar",
     "/home/akhilvb/accelerator-codegen/benchmarks/tpch/tmp/q17_sf1_nonzero_51_lg_jar.sql", "Brand#51", "LG JAR"},
    {"nonzero_55_jumbo_drum",
     "/home/akhilvb/accelerator-codegen/benchmarks/tpch/tmp/q17_sf1_nonzero_55_jumbo_drum.sql", "Brand#55", "JUMBO DRUM"},
    {"nonzero_35_jumbo_jar",
     "/home/akhilvb/accelerator-codegen/benchmarks/tpch/tmp/q17_sf1_nonzero_35_jumbo_jar.sql", "Brand#35", "JUMBO JAR"},
    {"nonzero_12_lg_pkg",
     "/home/akhilvb/accelerator-codegen/benchmarks/tpch/tmp/q17_sf1_nonzero_12_lg_pkg.sql", "Brand#12", "LG PKG"},
    {"nonzero_43_lg_jar",
     "/home/akhilvb/accelerator-codegen/benchmarks/tpch/tmp/q17_sf1_nonzero_43_lg_jar.sql", "Brand#43", "LG JAR"},
    {"nonzero_41_med_bag",
     "/home/akhilvb/accelerator-codegen/benchmarks/tpch/tmp/q17_sf1_nonzero_41_med_bag.sql", "Brand#41", "MED BAG"},
    {"nonzero_11_lg_box",
     "/home/akhilvb/accelerator-codegen/benchmarks/tpch/tmp/q17_sf1_nonzero_11_lg_box.sql", "Brand#11", "LG BOX"},
}};

} // namespace accelerator_codegen::test_support
