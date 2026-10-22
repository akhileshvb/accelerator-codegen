#pragma once

#include <array>

namespace accelerator_codegen::test_support {

struct Q4ManifestEntry {
	const char *label;
	const char *sql_path;
	const char *start_date;
};

inline constexpr std::array<Q4ManifestEntry, 20> kQ4Manifest = {{
    {"seed_1_q4", ACCEL_TPCH_INSTANCES_DIR "/seed_1/04.sql", "1996-11-01"},
    {"seed_2_q4", ACCEL_TPCH_INSTANCES_DIR "/seed_2/04.sql", "1995-01-01"},
    {"seed_3_q4", ACCEL_TPCH_INSTANCES_DIR "/seed_3/04.sql", "1997-04-01"},
    {"seed_4_q4", ACCEL_TPCH_INSTANCES_DIR "/seed_4/04.sql", "1995-04-01"},
    {"seed_5_q4", ACCEL_TPCH_INSTANCES_DIR "/seed_5/04.sql", "1997-02-01"},
    {"seed_6_q4", ACCEL_TPCH_INSTANCES_DIR "/seed_6/04.sql", "1993-12-01"},
    {"seed_7_q4", ACCEL_TPCH_INSTANCES_DIR "/seed_7/04.sql", "1995-02-01"},
    {"seed_8_q4", ACCEL_TPCH_INSTANCES_DIR "/seed_8/04.sql", "1996-08-01"},
    {"seed_9_q4", ACCEL_TPCH_INSTANCES_DIR "/seed_9/04.sql", "1995-09-01"},
    {"seed_10_q4", ACCEL_TPCH_INSTANCES_DIR "/seed_10/04.sql", "1995-01-01"},
    {"seed_11_q4", ACCEL_TPCH_INSTANCES_DIR "/seed_11/04.sql", "1993-05-01"},
    {"seed_12_q4", ACCEL_TPCH_INSTANCES_DIR "/seed_12/04.sql", "1993-06-01"},
    {"seed_13_q4", ACCEL_TPCH_INSTANCES_DIR "/seed_13/04.sql", "1994-09-01"},
    {"seed_14_q4", ACCEL_TPCH_INSTANCES_DIR "/seed_14/04.sql", "1994-12-01"},
    {"seed_15_q4", ACCEL_TPCH_INSTANCES_DIR "/seed_15/04.sql", "1993-01-01"},
    {"seed_16_q4", ACCEL_TPCH_INSTANCES_DIR "/seed_16/04.sql", "1994-02-01"},
    {"seed_17_q4", ACCEL_TPCH_INSTANCES_DIR "/seed_17/04.sql", "1997-10-01"},
    {"seed_18_q4", ACCEL_TPCH_INSTANCES_DIR "/seed_18/04.sql", "1997-03-01"},
    {"seed_19_q4", ACCEL_TPCH_INSTANCES_DIR "/seed_19/04.sql", "1997-05-01"},
    {"seed_20_q4", ACCEL_TPCH_INSTANCES_DIR "/seed_20/04.sql", "1997-08-01"},
}};

} // namespace accelerator_codegen::test_support
