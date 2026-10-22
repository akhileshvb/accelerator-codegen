#pragma once

#include <array>
#include <cstdint>

namespace accelerator_codegen::test_support {

struct Q18ManifestEntry {
	const char *label;
	const char *sql_path;
	int64_t quantity_threshold;
};

inline constexpr std::array<Q18ManifestEntry, 20> kQ38Manifest = {{
    {"seed_1_q18", ACCEL_TPCH_INSTANCES_DIR "/seed_1/18.sql", 312},
    {"seed_2_q18", ACCEL_TPCH_INSTANCES_DIR "/seed_2/18.sql", 312},
    {"seed_3_q18", ACCEL_TPCH_INSTANCES_DIR "/seed_3/18.sql", 312},
    {"seed_4_q18", ACCEL_TPCH_INSTANCES_DIR "/seed_4/18.sql", 313},
    {"seed_5_q18", ACCEL_TPCH_INSTANCES_DIR "/seed_5/18.sql", 312},
    {"seed_6_q18", ACCEL_TPCH_INSTANCES_DIR "/seed_6/18.sql", 312},
    {"seed_7_q18", ACCEL_TPCH_INSTANCES_DIR "/seed_7/18.sql", 312},
    {"seed_8_q18", ACCEL_TPCH_INSTANCES_DIR "/seed_8/18.sql", 315},
    {"seed_9_q18", ACCEL_TPCH_INSTANCES_DIR "/seed_9/18.sql", 313},
    {"seed_10_q18", ACCEL_TPCH_INSTANCES_DIR "/seed_10/18.sql", 314},
    {"seed_11_q18", ACCEL_TPCH_INSTANCES_DIR "/seed_11/18.sql", 315},
    {"seed_12_q18", ACCEL_TPCH_INSTANCES_DIR "/seed_12/18.sql", 315},
    {"seed_13_q18", ACCEL_TPCH_INSTANCES_DIR "/seed_13/18.sql", 312},
    {"seed_14_q18", ACCEL_TPCH_INSTANCES_DIR "/seed_14/18.sql", 315},
    {"seed_15_q18", ACCEL_TPCH_INSTANCES_DIR "/seed_15/18.sql", 314},
    {"seed_16_q18", ACCEL_TPCH_INSTANCES_DIR "/seed_16/18.sql", 314},
    {"seed_17_q18", ACCEL_TPCH_INSTANCES_DIR "/seed_17/18.sql", 313},
    {"seed_18_q18", ACCEL_TPCH_INSTANCES_DIR "/seed_18/18.sql", 315},
    {"seed_19_q18", ACCEL_TPCH_INSTANCES_DIR "/seed_19/18.sql", 315},
    {"seed_20_q18", ACCEL_TPCH_INSTANCES_DIR "/seed_20/18.sql", 315},
}};

inline constexpr const auto &kQ18Manifest = kQ38Manifest;

} // namespace accelerator_codegen::test_support
