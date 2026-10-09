#pragma once

// DuckDB string_t adaptors over the engine-neutral accession-key kernels.
#include "sec_edgar/q1/common/adsh_key.hpp"

#include "duckdb/common/types/string_type.hpp"

namespace accelerator_codegen::aux_state::query::q1 {

namespace detail {
inline void StringTAt(const void *ctx, std::size_t index, const char *&data, std::size_t &len) {
	const auto &value = static_cast<const duckdb::string_t *>(ctx)[index];
	data = value.GetData();
	len = value.GetSize();
}
} // namespace detail

// out_keys[i] = kInvalidAdshKey when values[sel ? sel[i] : i] is not canonical.
inline void ParseAdshKeyBatch(const duckdb::string_t *values, const uint32_t *sel, std::size_t count,
                              uint64_t *out_keys) {
	ParseAdshKeyBatchFn(values, detail::StringTAt, sel, count, out_keys);
}
inline void ParseAdshKeyBatchScalar(const duckdb::string_t *values, const uint32_t *sel, std::size_t count,
                                    uint64_t *out_keys) {
	ParseAdshKeyBatchScalarFn(values, detail::StringTAt, sel, count, out_keys);
}

} // namespace accelerator_codegen::aux_state::query::q1
