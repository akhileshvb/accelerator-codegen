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
	mkdir -p "${build}"
	cmake -S "${root}" -B "${build}"
	cmake --build "${build}" --target "${target}" -j"$(nproc)"
	[[ -x "${build}/${target}" ]]
}

# Create + cmake-configure a (possibly missing) build directory.
ensure_build_tree() {
	local root="$1"
	local build="$2"
	mkdir -p "${build}"
	if [[ ! -f "${build}/CMakeCache.txt" ]]; then
		cmake -S "${root}" -B "${build}"
	fi
	[[ -d "${build}" && -f "${build}/CMakeCache.txt" ]]
}

# Rebuild even when the binary already exists (agent may have changed sources).
build_cmake_target() {
	local root="$1"
	local build="$2"
	local target="$3"
	mkdir -p "${build}"
	cmake -S "${root}" -B "${build}"
	cmake --build "${build}" --target "${target}" -j"$(nproc)"
	[[ -x "${build}/${target}" ]]
}

ensure_base_smoke_test() {
	build_cmake_target "$1" "$2" "$3"
}

ensure_aux_smoke_test() {
	local root="$1"
	local build="$2"
	local query="$3"
	local benchmark="${4:-tpch}"
	if [[ "${benchmark}" == "tpch" ]]; then
		ensure_cmake_target "${root}" "${build}" "test_tpch_q${query}_aux"
	else
		ensure_cmake_target "${root}" "${build}" "test_${benchmark}_q${query}_aux"
	fi
}

ensure_aux_unit_tests() {
	local root="$1"
	local build="$2"
	local query="$3"
	local benchmark="${4:-tpch}"
	local prefix="q${query}"
	if [[ "${benchmark}" != "tpch" ]]; then
		prefix="${benchmark}_q${query}"
	fi
	mkdir -p "${build}"
	cmake -S "${root}" -B "${build}"
	cmake --build "${build}" --target \
		"test_${prefix}_incremental" \
		"test_${prefix}_aux_partition_state" \
		"test_${prefix}_write_policies" \
		"test_${prefix}_hybrid_policy" \
		"test_${prefix}_cost_model" \
		"test_${prefix}_staged_publication" \
		"test_${prefix}_multi_partition_delta" \
		"test_${prefix}_action_failure" \
		-j"$(nproc)" 2>/dev/null || true
}

ensure_cross_accel_tests() {
	build_cmake_target "$1" "$2" test_cross_accel_correctness
	build_cmake_target "$1" "$2" test_cross_accel_isolation
}

ensure_multi_connection_stress_test() {
	build_cmake_target "$1" "$2" test_aux_multi_connection_stress
}
