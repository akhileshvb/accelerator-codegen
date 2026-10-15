#pragma once

#include "tpch/q17/tests/q17_manifest.hpp"

#include <string>

namespace accelerator_codegen::test_support {

//! Q17 reference SQL for one brand/container pair: the generated SF1 instance
//! text (qgen's Q17 template) with the two parameters substituted.
inline std::string Q17ReferenceSql(const char *brand, const char *container) {
    return std::string("SELECT\n"
                       "  SUM(l_extendedprice) / 7.0 AS avg_yearly\n"
                       "FROM\n"
                       "  lineitem,\n"
                       "  part,\n"
                       "(\n"
                       "    SELECT\n"
                       "      l_partkey AS agg_partkey,\n"
                       "      0.2 * AVG(l_quantity) AS avg_quantity\n"
                       "    FROM\n"
                       "      lineitem\n"
                       "    GROUP BY\n"
                       "      l_partkey) part_agg\n"
                       "WHERE\n"
                       "  p_partkey = l_partkey\n"
                       "  AND agg_partkey = l_partkey\n"
                       "  AND p_brand = '") +
           brand + "'\n  AND p_container = '" + container + "'\n  AND l_quantity < avg_quantity;\n";
}

//! SF1 cases with dbgen-matching brand/container pairs (nonzero qualifying parts).
//! Selected from part table pairs with >=5 parts; verified nonzero Q17 avg_yearly on tpch_sf1.duckdb.
//! No SQL file: sql_path is null and the reference SQL is Q17ReferenceSql(brand, container).
inline constexpr std::array<Q17ManifestEntry, 10> kQ37NonzeroManifest = {{
    {"nonzero_42_lg_pack", nullptr, "Brand#42", "LG PACK"},
    {"nonzero_52_lg_jar", nullptr, "Brand#52", "LG JAR"},
    {"nonzero_54_med_pack", nullptr, "Brand#54", "MED PACK"},
    {"nonzero_51_lg_jar", nullptr, "Brand#51", "LG JAR"},
    {"nonzero_55_jumbo_drum", nullptr, "Brand#55", "JUMBO DRUM"},
    {"nonzero_35_jumbo_jar", nullptr, "Brand#35", "JUMBO JAR"},
    {"nonzero_12_lg_pkg", nullptr, "Brand#12", "LG PKG"},
    {"nonzero_43_lg_jar", nullptr, "Brand#43", "LG JAR"},
    {"nonzero_41_med_bag", nullptr, "Brand#41", "MED BAG"},
    {"nonzero_11_lg_box", nullptr, "Brand#11", "LG BOX"},
}};

} // namespace accelerator_codegen::test_support
