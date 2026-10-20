#pragma once

#include <array>
#include <cstdint>

namespace accelerator_codegen::test_support {

struct Q16ManifestEntry {
	const char *label;
	const char *sql_path;
	const char *brand;
	const char *type_pattern;
	std::array<int32_t, 8> sizes;
};

inline constexpr std::array<Q16ManifestEntry, 20> kQ36Manifest = {{
    {"seed_1_q16", ACCEL_TPCH_INSTANCES_DIR "/seed_1/16.sql", "Brand#13", "SMALL BRUSHED%", {26, 24, 27, 29, 7, 20, 22, 33}},
    {"seed_2_q16", ACCEL_TPCH_INSTANCES_DIR "/seed_2/16.sql", "Brand#14", "STANDARD ANODIZED%", {43, 32, 45, 36, 8, 42, 29, 46}},
    {"seed_3_q16", ACCEL_TPCH_INSTANCES_DIR "/seed_3/16.sql", "Brand#41", "MEDIUM PLATED%", {4, 17, 14, 24, 25, 40, 20, 12}},
    {"seed_4_q16", ACCEL_TPCH_INSTANCES_DIR "/seed_4/16.sql", "Brand#22", "MEDIUM BRUSHED%", {1, 6, 18, 7, 28, 43, 2, 15}},
    {"seed_5_q16", ACCEL_TPCH_INSTANCES_DIR "/seed_5/16.sql", "Brand#24", "SMALL POLISHED%", {17, 14, 7, 34, 39, 44, 28, 9}},
    {"seed_6_q16", ACCEL_TPCH_INSTANCES_DIR "/seed_6/16.sql", "Brand#33", "STANDARD POLISHED%", {35, 1, 40, 22, 29, 32, 8, 36}},
    {"seed_7_q16", ACCEL_TPCH_INSTANCES_DIR "/seed_7/16.sql", "Brand#33", "PROMO POLISHED%", {8, 45, 7, 17, 38, 29, 28, 34}},
    {"seed_8_q16", ACCEL_TPCH_INSTANCES_DIR "/seed_8/16.sql", "Brand#55", "STANDARD BURNISHED%", {44, 20, 4, 34, 24, 25, 10, 21}},
    {"seed_9_q16", ACCEL_TPCH_INSTANCES_DIR "/seed_9/16.sql", "Brand#22", "STANDARD BRUSHED%", {21, 37, 11, 34, 50, 40, 33, 43}},
    {"seed_10_q16", ACCEL_TPCH_INSTANCES_DIR "/seed_10/16.sql", "Brand#55", "LARGE BURNISHED%", {6, 3, 30, 42, 40, 21, 8, 28}},
    {"seed_11_q16", ACCEL_TPCH_INSTANCES_DIR "/seed_11/16.sql", "Brand#42", "PROMO PLATED%", {9, 32, 2, 7, 43, 3, 12, 49}},
    {"seed_12_q16", ACCEL_TPCH_INSTANCES_DIR "/seed_12/16.sql", "Brand#22", "MEDIUM POLISHED%", {19, 11, 34, 48, 38, 20, 9, 29}},
    {"seed_13_q16", ACCEL_TPCH_INSTANCES_DIR "/seed_13/16.sql", "Brand#12", "ECONOMY ANODIZED%", {30, 12, 36, 21, 8, 39, 7, 43}},
    {"seed_14_q16", ACCEL_TPCH_INSTANCES_DIR "/seed_14/16.sql", "Brand#31", "MEDIUM BRUSHED%", {34, 6, 37, 29, 33, 47, 30, 24}},
    {"seed_15_q16", ACCEL_TPCH_INSTANCES_DIR "/seed_15/16.sql", "Brand#35", "MEDIUM POLISHED%", {25, 36, 17, 32, 45, 38, 48, 47}},
    {"seed_16_q16", ACCEL_TPCH_INSTANCES_DIR "/seed_16/16.sql", "Brand#41", "PROMO BURNISHED%", {29, 49, 41, 7, 26, 18, 22, 44}},
    {"seed_17_q16", ACCEL_TPCH_INSTANCES_DIR "/seed_17/16.sql", "Brand#15", "MEDIUM PLATED%", {10, 34, 31, 2, 3, 15, 40, 28}},
    {"seed_18_q16", ACCEL_TPCH_INSTANCES_DIR "/seed_18/16.sql", "Brand#32", "SMALL BRUSHED%", {40, 3, 33, 36, 4, 13, 35, 9}},
    {"seed_19_q16", ACCEL_TPCH_INSTANCES_DIR "/seed_19/16.sql", "Brand#53", "PROMO BRUSHED%", {45, 2, 17, 49, 40, 23, 8, 43}},
    {"seed_20_q16", ACCEL_TPCH_INSTANCES_DIR "/seed_20/16.sql", "Brand#54", "LARGE ANODIZED%", {14, 17, 48, 19, 23, 6, 25, 9}},
}};

inline constexpr const auto &kQ16Manifest = kQ36Manifest;

} // namespace accelerator_codegen::test_support
