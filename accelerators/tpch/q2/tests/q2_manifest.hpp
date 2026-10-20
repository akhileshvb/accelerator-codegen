#pragma once

#include <array>
#include <cstdint>

namespace accelerator_codegen::test_support {

struct Q2ManifestEntry {
	const char *label;
	const char *sql_path;
	int32_t part_size;
	const char *part_type_pattern;
	const char *region_name;
};

inline constexpr std::array<Q2ManifestEntry, 20> kQ2Manifest = {{
    {"seed_1_q2", ACCEL_TPCH_INSTANCES_DIR "/seed_1/02.sql", 10, "BRASS", "AFRICA"},
    {"seed_2_q2", ACCEL_TPCH_INSTANCES_DIR "/seed_2/02.sql", 12, "COPPER", "MIDDLE EAST"},
    {"seed_3_q2", ACCEL_TPCH_INSTANCES_DIR "/seed_3/02.sql", 42, "BRASS", "ASIA"},
    {"seed_4_q2", ACCEL_TPCH_INSTANCES_DIR "/seed_4/02.sql", 38, "NICKEL", "EUROPE"},
    {"seed_5_q2", ACCEL_TPCH_INSTANCES_DIR "/seed_5/02.sql", 36, "STEEL", "EUROPE"},
    {"seed_6_q2", ACCEL_TPCH_INSTANCES_DIR "/seed_6/02.sql", 49, "NICKEL", "EUROPE"},
    {"seed_7_q2", ACCEL_TPCH_INSTANCES_DIR "/seed_7/02.sql", 16, "BRASS", "AMERICA"},
    {"seed_8_q2", ACCEL_TPCH_INSTANCES_DIR "/seed_8/02.sql", 17, "BRASS", "MIDDLE EAST"},
    {"seed_9_q2", ACCEL_TPCH_INSTANCES_DIR "/seed_9/02.sql", 47, "TIN", "AMERICA"},
    {"seed_10_q2", ACCEL_TPCH_INSTANCES_DIR "/seed_10/02.sql", 36, "NICKEL", "MIDDLE EAST"},
    {"seed_11_q2", ACCEL_TPCH_INSTANCES_DIR "/seed_11/02.sql", 6, "TIN", "AFRICA"},
    {"seed_12_q2", ACCEL_TPCH_INSTANCES_DIR "/seed_12/02.sql", 49, "COPPER", "AMERICA"},
    {"seed_13_q2", ACCEL_TPCH_INSTANCES_DIR "/seed_13/02.sql", 30, "COPPER", "AFRICA"},
    {"seed_14_q2", ACCEL_TPCH_INSTANCES_DIR "/seed_14/02.sql", 6, "TIN", "MIDDLE EAST"},
    {"seed_15_q2", ACCEL_TPCH_INSTANCES_DIR "/seed_15/02.sql", 46, "NICKEL", "AMERICA"},
    {"seed_16_q2", ACCEL_TPCH_INSTANCES_DIR "/seed_16/02.sql", 30, "TIN", "AFRICA"},
    {"seed_17_q2", ACCEL_TPCH_INSTANCES_DIR "/seed_17/02.sql", 10, "COPPER", "EUROPE"},
    {"seed_18_q2", ACCEL_TPCH_INSTANCES_DIR "/seed_18/02.sql", 20, "TIN", "ASIA"},
    {"seed_19_q2", ACCEL_TPCH_INSTANCES_DIR "/seed_19/02.sql", 8, "NICKEL", "EUROPE"},
    {"seed_20_q2", ACCEL_TPCH_INSTANCES_DIR "/seed_20/02.sql", 27, "STEEL", "AMERICA"},
}};

} // namespace accelerator_codegen::test_support
