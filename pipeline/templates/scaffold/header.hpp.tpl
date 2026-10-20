#pragma once

#include <cstdint>
#include <memory>

// {{benchmark_name}} Q{{query_number}} on {{engine_name}}. The engine's native types and
// registrars are the ones backends/{{engine_name}}/api/README.md lists; include what you use.
namespace accelerator_codegen {

struct {{type_prefix}}Parameters {
	uint64_t row_estimate = 0;
};

struct {{type_prefix}}ProfileCounters {
	uint64_t pre_rows = 0;
	uint64_t qualifying_rows = 0;
	uint64_t generic_fallback_rows = 0;
	uint64_t flat_chunks = 0;
	uint64_t dict_chunks = 0;
	uint64_t generic_chunks = 0;
	uint64_t distinct_inserts = 0;
	uint64_t active_groups = 0;
	int64_t pre_scan_us = 0;
	int64_t pre_consume_us = 0;
	int64_t finalize_us = 0;
};

} // namespace accelerator_codegen
