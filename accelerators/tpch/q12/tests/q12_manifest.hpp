#pragma once

#include <array>

namespace accelerator_codegen::test_support {

struct Q12ManifestEntry {
	const char *label;
	const char *sql_path;
	const char *shipmode1;
	const char *shipmode2;
	const char *start_date;
};

inline constexpr std::array<Q12ManifestEntry, 20> kQ12Manifest = {{
    {"seed_1_q12", ACCEL_TPCH_INSTANCES_DIR "/seed_1/12.sql", "REG AIR", "SHIP", "1996-01-01"},
    {"seed_2_q12", ACCEL_TPCH_INSTANCES_DIR "/seed_2/12.sql", "REG AIR", "SHIP", "1996-01-01"},
    {"seed_3_q12", ACCEL_TPCH_INSTANCES_DIR "/seed_3/12.sql", "REG AIR", "RAIL", "1993-01-01"},
    {"seed_4_q12", ACCEL_TPCH_INSTANCES_DIR "/seed_4/12.sql", "RAIL", "SHIP", "1993-01-01"},
    {"seed_5_q12", ACCEL_TPCH_INSTANCES_DIR "/seed_5/12.sql", "MAIL", "FOB", "1997-01-01"},
    {"seed_6_q12", ACCEL_TPCH_INSTANCES_DIR "/seed_6/12.sql", "RAIL", "REG AIR", "1997-01-01"},
    {"seed_7_q12", ACCEL_TPCH_INSTANCES_DIR "/seed_7/12.sql", "MAIL", "TRUCK", "1995-01-01"},
    {"seed_8_q12", ACCEL_TPCH_INSTANCES_DIR "/seed_8/12.sql", "AIR", "SHIP", "1993-01-01"},
    {"seed_9_q12", ACCEL_TPCH_INSTANCES_DIR "/seed_9/12.sql", "FOB", "REG AIR", "1996-01-01"},
    {"seed_10_q12", ACCEL_TPCH_INSTANCES_DIR "/seed_10/12.sql", "MAIL", "SHIP", "1996-01-01"},
    {"seed_11_q12", ACCEL_TPCH_INSTANCES_DIR "/seed_11/12.sql", "SHIP", "RAIL", "1993-01-01"},
    {"seed_12_q12", ACCEL_TPCH_INSTANCES_DIR "/seed_12/12.sql", "REG AIR", "MAIL", "1994-01-01"},
    {"seed_13_q12", ACCEL_TPCH_INSTANCES_DIR "/seed_13/12.sql", "MAIL", "AIR", "1996-01-01"},
    {"seed_14_q12", ACCEL_TPCH_INSTANCES_DIR "/seed_14/12.sql", "AIR", "RAIL", "1994-01-01"},
    {"seed_15_q12", ACCEL_TPCH_INSTANCES_DIR "/seed_15/12.sql", "FOB", "MAIL", "1996-01-01"},
    {"seed_16_q12", ACCEL_TPCH_INSTANCES_DIR "/seed_16/12.sql", "RAIL", "REG AIR", "1997-01-01"},
    {"seed_17_q12", ACCEL_TPCH_INSTANCES_DIR "/seed_17/12.sql", "SHIP", "REG AIR", "1993-01-01"},
    {"seed_18_q12", ACCEL_TPCH_INSTANCES_DIR "/seed_18/12.sql", "SHIP", "TRUCK", "1997-01-01"},
    {"seed_19_q12", ACCEL_TPCH_INSTANCES_DIR "/seed_19/12.sql", "AIR", "RAIL", "1995-01-01"},
    {"seed_20_q12", ACCEL_TPCH_INSTANCES_DIR "/seed_20/12.sql", "SHIP", "MAIL", "1997-01-01"},
}};

} // namespace accelerator_codegen::test_support
