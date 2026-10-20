#pragma once

#include <array>
#include <cstdint>

namespace accelerator_codegen::test_support {

struct Q22ManifestEntry {
	const char *label;
	const char *sql_path;
	std::array<const char *, 7> country_codes;
};

inline constexpr std::array<Q22ManifestEntry, 20> kQ22Manifest = {{
    {"seed_1_q22", ACCEL_TPCH_INSTANCES_DIR "/seed_1/22.sql", {"30", "19", "14", "28", "11", "22", "32"}},
    {"seed_2_q22", ACCEL_TPCH_INSTANCES_DIR "/seed_2/22.sql", {"21", "16", "17", "24", "15", "31", "26"}},
    {"seed_3_q22", ACCEL_TPCH_INSTANCES_DIR "/seed_3/22.sql", {"14", "10", "27", "30", "15", "16", "17"}},
    {"seed_4_q22", ACCEL_TPCH_INSTANCES_DIR "/seed_4/22.sql", {"12", "30", "13", "16", "20", "32", "11"}},
    {"seed_5_q22", ACCEL_TPCH_INSTANCES_DIR "/seed_5/22.sql", {"21", "12", "17", "14", "23", "16", "29"}},
    {"seed_6_q22", ACCEL_TPCH_INSTANCES_DIR "/seed_6/22.sql", {"25", "30", "29", "13", "16", "18", "28"}},
    {"seed_7_q22", ACCEL_TPCH_INSTANCES_DIR "/seed_7/22.sql", {"21", "25", "30", "29", "31", "26", "15"}},
    {"seed_8_q22", ACCEL_TPCH_INSTANCES_DIR "/seed_8/22.sql", {"27", "21", "30", "14", "20", "26", "34"}},
    {"seed_9_q22", ACCEL_TPCH_INSTANCES_DIR "/seed_9/22.sql", {"24", "28", "13", "12", "27", "22", "16"}},
    {"seed_10_q22", ACCEL_TPCH_INSTANCES_DIR "/seed_10/22.sql", {"12", "20", "33", "16", "22", "10", "26"}},
    {"seed_11_q22", ACCEL_TPCH_INSTANCES_DIR "/seed_11/22.sql", {"17", "33", "18", "29", "19", "30", "32"}},
    {"seed_12_q22", ACCEL_TPCH_INSTANCES_DIR "/seed_12/22.sql", {"23", "18", "33", "29", "28", "20", "34"}},
    {"seed_13_q22", ACCEL_TPCH_INSTANCES_DIR "/seed_13/22.sql", {"21", "12", "34", "11", "28", "17", "27"}},
    {"seed_14_q22", ACCEL_TPCH_INSTANCES_DIR "/seed_14/22.sql", {"33", "19", "34", "21", "32", "18", "27"}},
    {"seed_15_q22", ACCEL_TPCH_INSTANCES_DIR "/seed_15/22.sql", {"21", "20", "31", "23", "27", "25", "14"}},
    {"seed_16_q22", ACCEL_TPCH_INSTANCES_DIR "/seed_16/22.sql", {"19", "30", "18", "23", "10", "25", "24"}},
    {"seed_17_q22", ACCEL_TPCH_INSTANCES_DIR "/seed_17/22.sql", {"20", "26", "33", "14", "13", "16", "29"}},
    {"seed_18_q22", ACCEL_TPCH_INSTANCES_DIR "/seed_18/22.sql", {"14", "28", "29", "23", "13", "27", "21"}},
    {"seed_19_q22", ACCEL_TPCH_INSTANCES_DIR "/seed_19/22.sql", {"32", "20", "12", "28", "22", "26", "18"}},
    {"seed_20_q22", ACCEL_TPCH_INSTANCES_DIR "/seed_20/22.sql", {"16", "32", "18", "33", "17", "24", "27"}},
}};

} // namespace accelerator_codegen::test_support
