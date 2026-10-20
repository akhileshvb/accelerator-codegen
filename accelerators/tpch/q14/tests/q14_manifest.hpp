#pragma once

#include <array>

namespace accelerator_codegen::test_support {

struct Q14ManifestEntry {
	const char *label;
	const char *sql_path;
	const char *start_date;
};

inline constexpr std::array<Q14ManifestEntry, 20> kQ14Manifest = {{
    {"seed_1_q14", ACCEL_TPCH_INSTANCES_DIR "/seed_1/14.sql", "1995-06-01"},
    {"seed_2_q14", ACCEL_TPCH_INSTANCES_DIR "/seed_2/14.sql", "1994-10-01"},
    {"seed_3_q14", ACCEL_TPCH_INSTANCES_DIR "/seed_3/14.sql", "1994-05-01"},
    {"seed_4_q14", ACCEL_TPCH_INSTANCES_DIR "/seed_4/14.sql", "1994-11-01"},
    {"seed_5_q14", ACCEL_TPCH_INSTANCES_DIR "/seed_5/14.sql", "1995-12-01"},
    {"seed_6_q14", ACCEL_TPCH_INSTANCES_DIR "/seed_6/14.sql", "1993-04-01"},
    {"seed_7_q14", ACCEL_TPCH_INSTANCES_DIR "/seed_7/14.sql", "1996-08-01"},
    {"seed_8_q14", ACCEL_TPCH_INSTANCES_DIR "/seed_8/14.sql", "1993-05-01"},
    {"seed_9_q14", ACCEL_TPCH_INSTANCES_DIR "/seed_9/14.sql", "1997-01-01"},
    {"seed_10_q14", ACCEL_TPCH_INSTANCES_DIR "/seed_10/14.sql", "1995-10-01"},
    {"seed_11_q14", ACCEL_TPCH_INSTANCES_DIR "/seed_11/14.sql", "1995-06-01"},
    {"seed_12_q14", ACCEL_TPCH_INSTANCES_DIR "/seed_12/14.sql", "1995-10-01"},
    {"seed_13_q14", ACCEL_TPCH_INSTANCES_DIR "/seed_13/14.sql", "1993-05-01"},
    {"seed_14_q14", ACCEL_TPCH_INSTANCES_DIR "/seed_14/14.sql", "1994-06-01"},
    {"seed_15_q14", ACCEL_TPCH_INSTANCES_DIR "/seed_15/14.sql", "1997-01-01"},
    {"seed_16_q14", ACCEL_TPCH_INSTANCES_DIR "/seed_16/14.sql", "1993-11-01"},
    {"seed_17_q14", ACCEL_TPCH_INSTANCES_DIR "/seed_17/14.sql", "1995-03-01"},
    {"seed_18_q14", ACCEL_TPCH_INSTANCES_DIR "/seed_18/14.sql", "1995-09-01"},
    {"seed_19_q14", ACCEL_TPCH_INSTANCES_DIR "/seed_19/14.sql", "1994-10-01"},
    {"seed_20_q14", ACCEL_TPCH_INSTANCES_DIR "/seed_20/14.sql", "1997-01-01"},
}};

} // namespace accelerator_codegen::test_support
