#pragma once

#include <array>

namespace accelerator_codegen::test_support {

struct Q7ManifestEntry {
	const char *label;
	const char *sql_path;
	const char *nation1;
	const char *nation2;
};

inline constexpr std::array<Q7ManifestEntry, 20> kQ7Manifest = {{
    {"seed_1_q7", ACCEL_TPCH_INSTANCES_DIR "/seed_1/07.sql", "UNITED KINGDOM", "BRAZIL"},
    {"seed_2_q7", ACCEL_TPCH_INSTANCES_DIR "/seed_2/07.sql", "KENYA", "ALGERIA"},
    {"seed_3_q7", ACCEL_TPCH_INSTANCES_DIR "/seed_3/07.sql", "ARGENTINA", "MOZAMBIQUE"},
    {"seed_4_q7", ACCEL_TPCH_INSTANCES_DIR "/seed_4/07.sql", "SAUDI ARABIA", "IRAQ"},
    {"seed_5_q7", ACCEL_TPCH_INSTANCES_DIR "/seed_5/07.sql", "IRAQ", "RUSSIA"},
    {"seed_6_q7", ACCEL_TPCH_INSTANCES_DIR "/seed_6/07.sql", "MOZAMBIQUE", "FRANCE"},
    {"seed_7_q7", ACCEL_TPCH_INSTANCES_DIR "/seed_7/07.sql", "CHINA", "MOZAMBIQUE"},
    {"seed_8_q7", ACCEL_TPCH_INSTANCES_DIR "/seed_8/07.sql", "ETHIOPIA", "UNITED KINGDOM"},
    {"seed_9_q7", ACCEL_TPCH_INSTANCES_DIR "/seed_9/07.sql", "BRAZIL", "KENYA"},
    {"seed_10_q7", ACCEL_TPCH_INSTANCES_DIR "/seed_10/07.sql", "JORDAN", "ALGERIA"},
    {"seed_11_q7", ACCEL_TPCH_INSTANCES_DIR "/seed_11/07.sql", "MOZAMBIQUE", "IRAN"},
    {"seed_12_q7", ACCEL_TPCH_INSTANCES_DIR "/seed_12/07.sql", "GERMANY", "ARGENTINA"},
    {"seed_13_q7", ACCEL_TPCH_INSTANCES_DIR "/seed_13/07.sql", "KENYA", "VIETNAM"},
    {"seed_14_q7", ACCEL_TPCH_INSTANCES_DIR "/seed_14/07.sql", "SAUDI ARABIA", "GERMANY"},
    {"seed_15_q7", ACCEL_TPCH_INSTANCES_DIR "/seed_15/07.sql", "INDIA", "MOROCCO"},
    {"seed_16_q7", ACCEL_TPCH_INSTANCES_DIR "/seed_16/07.sql", "PERU", "EGYPT"},
    {"seed_17_q7", ACCEL_TPCH_INSTANCES_DIR "/seed_17/07.sql", "RUSSIA", "MOZAMBIQUE"},
    {"seed_18_q7", ACCEL_TPCH_INSTANCES_DIR "/seed_18/07.sql", "KENYA", "UNITED STATES"},
    {"seed_19_q7", ACCEL_TPCH_INSTANCES_DIR "/seed_19/07.sql", "ROMANIA", "MOROCCO"},
    {"seed_20_q7", ACCEL_TPCH_INSTANCES_DIR "/seed_20/07.sql", "INDONESIA", "IRAQ"},
}};

} // namespace accelerator_codegen::test_support
