#pragma once

#include <array>
#include <cstdint>

namespace accelerator_codegen::test_support {

struct Q10ManifestEntry {
	const char *label;
	const char *sql_path;
	const char *start_date;
};

inline constexpr std::array<Q10ManifestEntry, 20> kQ10Manifest = {{
    {"seed_1_q10", ACCEL_TPCH_INSTANCES_DIR "/seed_1/10.sql", "1993-06-01"},
    {"seed_2_q10", ACCEL_TPCH_INSTANCES_DIR "/seed_2/10.sql", "1993-10-01"},
    {"seed_3_q10", ACCEL_TPCH_INSTANCES_DIR "/seed_3/10.sql", "1994-08-01"},
    {"seed_4_q10", ACCEL_TPCH_INSTANCES_DIR "/seed_4/10.sql", "1993-11-01"},
    {"seed_5_q10", ACCEL_TPCH_INSTANCES_DIR "/seed_5/10.sql", "1993-10-01"},
    {"seed_6_q10", ACCEL_TPCH_INSTANCES_DIR "/seed_6/10.sql", "1993-06-01"},
    {"seed_7_q10", ACCEL_TPCH_INSTANCES_DIR "/seed_7/10.sql", "1997-01-01"},
    {"seed_8_q10", ACCEL_TPCH_INSTANCES_DIR "/seed_8/10.sql", "1993-11-01"},
    {"seed_9_q10", ACCEL_TPCH_INSTANCES_DIR "/seed_9/10.sql", "1994-08-01"},
    {"seed_10_q10", ACCEL_TPCH_INSTANCES_DIR "/seed_10/10.sql", "1996-10-01"},
    {"seed_11_q10", ACCEL_TPCH_INSTANCES_DIR "/seed_11/10.sql", "1996-12-01"},
    {"seed_12_q10", ACCEL_TPCH_INSTANCES_DIR "/seed_12/10.sql", "1997-01-01"},
    {"seed_13_q10", ACCEL_TPCH_INSTANCES_DIR "/seed_13/10.sql", "1996-06-01"},
    {"seed_14_q10", ACCEL_TPCH_INSTANCES_DIR "/seed_14/10.sql", "1995-05-01"},
    {"seed_15_q10", ACCEL_TPCH_INSTANCES_DIR "/seed_15/10.sql", "1996-02-01"},
    {"seed_16_q10", ACCEL_TPCH_INSTANCES_DIR "/seed_16/10.sql", "1997-01-01"},
    {"seed_17_q10", ACCEL_TPCH_INSTANCES_DIR "/seed_17/10.sql", "1993-09-01"},
    {"seed_18_q10", ACCEL_TPCH_INSTANCES_DIR "/seed_18/10.sql", "1996-02-01"},
    {"seed_19_q10", ACCEL_TPCH_INSTANCES_DIR "/seed_19/10.sql", "1993-11-01"},
    {"seed_20_q10", ACCEL_TPCH_INSTANCES_DIR "/seed_20/10.sql", "1993-05-01"},
}};

} // namespace accelerator_codegen::test_support
