#pragma once

#include <array>
#include <cstdint>

namespace accelerator_codegen::test_support {

struct Q5ManifestEntry {
	const char *label;
	const char *sql_path;
	const char *region_name;
	const char *start_date;
};

inline constexpr std::array<Q5ManifestEntry, 20> kQ5Manifest = {{
    {"seed_1_q5", ACCEL_TPCH_INSTANCES_DIR "/seed_1/05.sql", "ASIA", "1996-01-01"},
    {"seed_2_q5", ACCEL_TPCH_INSTANCES_DIR "/seed_2/05.sql", "AFRICA", "1996-01-01"},
    {"seed_3_q5", ACCEL_TPCH_INSTANCES_DIR "/seed_3/05.sql", "AFRICA", "1994-01-01"},
    {"seed_4_q5", ACCEL_TPCH_INSTANCES_DIR "/seed_4/05.sql", "AFRICA", "1994-01-01"},
    {"seed_5_q5", ACCEL_TPCH_INSTANCES_DIR "/seed_5/05.sql", "ASIA", "1997-01-01"},
    {"seed_6_q5", ACCEL_TPCH_INSTANCES_DIR "/seed_6/05.sql", "EUROPE", "1994-01-01"},
    {"seed_7_q5", ACCEL_TPCH_INSTANCES_DIR "/seed_7/05.sql", "EUROPE", "1995-01-01"},
    {"seed_8_q5", ACCEL_TPCH_INSTANCES_DIR "/seed_8/05.sql", "ASIA", "1994-01-01"},
    {"seed_9_q5", ACCEL_TPCH_INSTANCES_DIR "/seed_9/05.sql", "MIDDLE EAST", "1996-01-01"},
    {"seed_10_q5", ACCEL_TPCH_INSTANCES_DIR "/seed_10/05.sql", "AMERICA", "1996-01-01"},
    {"seed_11_q5", ACCEL_TPCH_INSTANCES_DIR "/seed_11/05.sql", "AFRICA", "1994-01-01"},
    {"seed_12_q5", ACCEL_TPCH_INSTANCES_DIR "/seed_12/05.sql", "AMERICA", "1997-01-01"},
    {"seed_13_q5", ACCEL_TPCH_INSTANCES_DIR "/seed_13/05.sql", "MIDDLE EAST", "1995-01-01"},
    {"seed_14_q5", ACCEL_TPCH_INSTANCES_DIR "/seed_14/05.sql", "AFRICA", "1994-01-01"},
    {"seed_15_q5", ACCEL_TPCH_INSTANCES_DIR "/seed_15/05.sql", "EUROPE", "1994-01-01"},
    {"seed_16_q5", ACCEL_TPCH_INSTANCES_DIR "/seed_16/05.sql", "MIDDLE EAST", "1995-01-01"},
    {"seed_17_q5", ACCEL_TPCH_INSTANCES_DIR "/seed_17/05.sql", "AFRICA", "1994-01-01"},
    {"seed_18_q5", ACCEL_TPCH_INSTANCES_DIR "/seed_18/05.sql", "MIDDLE EAST", "1995-01-01"},
    {"seed_19_q5", ACCEL_TPCH_INSTANCES_DIR "/seed_19/05.sql", "AFRICA", "1995-01-01"},
    {"seed_20_q5", ACCEL_TPCH_INSTANCES_DIR "/seed_20/05.sql", "ASIA", "1993-01-01"},
}};

} // namespace accelerator_codegen::test_support
