#!/usr/bin/env bash
# Shared helper: configure CMake if needed and build a target.
# Usage: source this file, then call ensure_cmake_target ROOT BUILD TARGET

ensure_cmake_target() {
	local root="$1"
	local build="$2"
	local target="$3"
	if [[ -x "${build}/${target}" ]]; then
		return 0
	fi
	if [[ ! -d "${build}" ]]; then
		return 1
	fi
	cmake -S "${root}" -B "${build}"
	cmake --build "${build}" --target "${target}" -j"$(nproc)"
	[[ -x "${build}/${target}" ]]
}

ensure_aux_smoke_test() {
	ensure_cmake_target "$1" "$2" "test_tpch_q${3}_aux"
}

ensure_aux_unit_tests() {
	local root="$1"
	local build="$2"
	local query="$3"
	if [[ ! -d "${build}" ]]; then
		return 1
	fi
	cmake -S "${root}" -B "${build}"
	cmake --build "${build}" --target \
		"test_q${query}_incremental" \
		"test_q${query}_aux_partition_state" \
		"test_q${query}_write_policies" \
		"test_q${query}_hybrid_policy" \
		"test_q${query}_cost_model" \
		"test_q${query}_staged_publication" \
		"test_q${query}_multi_partition_delta" \
		"test_q${query}_action_failure" \
		-j"$(nproc)"
}

ensure_cross_accel_tests() {
	ensure_cmake_target "$1" "$2" test_cross_accel_correctness
	ensure_cmake_target "$1" "$2" test_cross_accel_isolation
}

ensure_multi_connection_stress_test() {
	ensure_cmake_target "$1" "$2" test_aux_multi_connection_stress
}
