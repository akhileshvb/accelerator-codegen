// GENERATED BACKEND
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "accelerator_codegen/runtime/postgres/scan_adapter.hpp"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>

#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/vector.hpp"

extern "C" {
#include "postgres.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
}

#ifdef ERROR
#undef ERROR
#endif
#ifdef Max
#undef Max
#endif
#ifdef Min
#undef Min
#endif

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

constexpr int32_t kPostgresDateEpochOffset = 10957;
constexpr ::duckdb::idx_t kScanBatchSize = 2048;

struct BoundColumn {
	AttrNumber attnum = 0;
	Oid type_oid = InvalidOid;
	NativeColumnSpec spec;
};

struct BoundFilter {
	AttrNumber attnum = 0;
	Oid type_oid = InvalidOid;
	NativeFilterSpec spec;
};

std::string DatumText(Datum datum, Oid type_oid) {
	Oid output_function = InvalidOid;
	bool is_varlena = false;
	getTypeOutputInfo(type_oid, &output_function, &is_varlena);
	char *text = OidOutputFunctionCall(output_function, datum);
	std::string result(text ? text : "");
	if (text) {
		pfree(text);
	}
	return result;
}

Form_pg_attribute LookupAttribute(TupleDesc tuple_desc, const std::string &name) {
	for (int attribute = 0; attribute < tuple_desc->natts; ++attribute) {
		Form_pg_attribute attr = TupleDescAttr(tuple_desc, attribute);
		if (attr->attisdropped) {
			continue;
		}
		if (name == NameStr(attr->attname)) {
			return attr;
		}
	}
	throw std::runtime_error("postgres native scan: column \"" + name +
	                         "\" not found");
}

std::vector<BoundColumn> BindProjectedColumns(TupleDesc tuple_desc,
                                              const NativeScanSpec &scan_spec) {
	if (scan_spec.projected_columns.empty()) {
		throw std::runtime_error("postgres native scan requires projected columns");
	}
	std::vector<BoundColumn> columns;
	columns.reserve(scan_spec.projected_columns.size());
	for (const auto &column : scan_spec.projected_columns) {
		Form_pg_attribute attr = LookupAttribute(tuple_desc, column.source_name);
		BoundColumn bound;
		bound.spec = column;
		bound.attnum = attr->attnum;
		bound.type_oid = attr->atttypid;
		columns.push_back(std::move(bound));
	}
	return columns;
}

std::vector<BoundFilter> BindFilters(TupleDesc tuple_desc,
                                     const NativeScanSpec &scan_spec) {
	std::vector<BoundFilter> filters;
	filters.reserve(scan_spec.filters.size());
	for (const auto &filter : scan_spec.filters) {
		Form_pg_attribute attr = LookupAttribute(tuple_desc, filter.source_name);
		BoundFilter bound;
		bound.spec = filter;
		bound.attnum = attr->attnum;
		bound.type_oid = attr->atttypid;
		filters.push_back(std::move(bound));
	}
	return filters;
}

int64_t DatumAsInt64(Datum datum, Oid type_oid) {
	if (type_oid == INT8OID) {
		return DatumGetInt64(datum);
	}
	if (type_oid == INT4OID) {
		return DatumGetInt32(datum);
	}
	if (type_oid == INT2OID) {
		return DatumGetInt16(datum);
	}
	if (type_oid == NUMERICOID) {
		const double numeric =
		    DatumGetFloat8(DirectFunctionCall1(numeric_float8, datum));
		return static_cast<int64_t>(std::llround(numeric * 100.0));
	}
	if (type_oid == DATEOID) {
		return static_cast<int64_t>(DatumGetDateADT(datum) + kPostgresDateEpochOffset);
	}
	return std::stoll(DatumText(datum, type_oid));
}

double DatumAsDouble(Datum datum, Oid type_oid) {
	if (type_oid == FLOAT8OID) {
		return DatumGetFloat8(datum);
	}
	if (type_oid == FLOAT4OID) {
		return DatumGetFloat4(datum);
	}
	if (type_oid == NUMERICOID) {
		return DatumGetFloat8(DirectFunctionCall1(numeric_float8, datum));
	}
	return std::stod(DatumText(datum, type_oid));
}

int32_t DatumAsUnixDays(Datum datum, Oid type_oid) {
	if (type_oid == DATEOID) {
		return DatumGetDateADT(datum) + kPostgresDateEpochOffset;
	}
	return static_cast<int32_t>(DatumAsInt64(datum, type_oid));
}

template <typename T>
bool CompareOrdered(T left, ::duckdb::ExpressionType comparison, T right) {
	switch (comparison) {
	case ::duckdb::ExpressionType::COMPARE_EQUAL:
		return left == right;
	case ::duckdb::ExpressionType::COMPARE_NOTEQUAL:
		return left != right;
	case ::duckdb::ExpressionType::COMPARE_LESSTHAN:
		return left < right;
	case ::duckdb::ExpressionType::COMPARE_GREATERTHAN:
		return left > right;
	case ::duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return left <= right;
	case ::duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return left >= right;
	default:
		throw std::runtime_error("unsupported native scan filter comparison");
	}
}

bool PassesFilter(const BoundFilter &filter, Datum datum, bool is_null) {
	if (filter.spec.comparison_type ==
	    ::duckdb::ExpressionType::OPERATOR_IS_NOT_NULL) {
		return !is_null;
	}
	if (is_null) {
		return false;
	}

	const auto &constant = filter.spec.constant;
	switch (constant.type().id()) {
	case ::duckdb::LogicalTypeId::DATE: {
		const auto expected = constant.GetValue<::duckdb::date_t>().days;
		return CompareOrdered(DatumAsUnixDays(datum, filter.type_oid),
		                      filter.spec.comparison_type, expected);
	}
	case ::duckdb::LogicalTypeId::INTEGER:
	case ::duckdb::LogicalTypeId::BIGINT:
	case ::duckdb::LogicalTypeId::SMALLINT:
	case ::duckdb::LogicalTypeId::TINYINT:
	case ::duckdb::LogicalTypeId::DECIMAL:
	case ::duckdb::LogicalTypeId::HUGEINT:
	case ::duckdb::LogicalTypeId::UBIGINT:
	case ::duckdb::LogicalTypeId::UINTEGER: {
		int64_t expected = 0;
		if (constant.type().id() == ::duckdb::LogicalTypeId::DECIMAL) {
			expected = static_cast<int64_t>(
			    std::llround(std::stod(constant.ToString()) * 100.0));
		} else {
			expected = static_cast<int64_t>(std::llround(std::stod(constant.ToString())));
		}
		return CompareOrdered(DatumAsInt64(datum, filter.type_oid),
		                      filter.spec.comparison_type, expected);
	}
	case ::duckdb::LogicalTypeId::FLOAT:
	case ::duckdb::LogicalTypeId::DOUBLE: {
		return CompareOrdered(DatumAsDouble(datum, filter.type_oid),
		                      filter.spec.comparison_type,
		                      constant.GetValue<double>());
	}
	case ::duckdb::LogicalTypeId::VARCHAR:
	case ::duckdb::LogicalTypeId::CHAR: {
		const std::string left = DatumText(datum, filter.type_oid);
		const std::string right = constant.ToString();
		switch (filter.spec.comparison_type) {
		case ::duckdb::ExpressionType::COMPARE_EQUAL:
			return left == right;
		case ::duckdb::ExpressionType::COMPARE_NOTEQUAL:
			return left != right;
		case ::duckdb::ExpressionType::COMPARE_LESSTHAN:
			return left < right;
		case ::duckdb::ExpressionType::COMPARE_GREATERTHAN:
			return left > right;
		case ::duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO:
			return left <= right;
		case ::duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			return left >= right;
		default:
			throw std::runtime_error("unsupported varchar filter comparison");
		}
	}
	default:
		throw std::runtime_error("unsupported native scan filter constant type");
	}
}

bool PassesFilters(TupleTableSlot *slot, const std::vector<BoundFilter> &filters) {
	for (const auto &filter : filters) {
		bool is_null = false;
		const Datum datum = slot_getattr(slot, filter.attnum, &is_null);
		if (!PassesFilter(filter, datum, is_null)) {
			return false;
		}
	}
	return true;
}

void AppendFlatValue(::duckdb::Vector &vector, ::duckdb::idx_t row_index,
                     Datum datum, bool is_null, Oid type_oid,
                     const NativeColumnSpec &column) {
	if (is_null) {
		::duckdb::FlatVector::SetNull(vector, row_index, true);
		return;
	}
	switch (column.expected_physical_type) {
	case ::duckdb::PhysicalType::INT32: {
		::duckdb::FlatVector::GetData<int32_t>(vector)[row_index] =
		    DatumAsUnixDays(datum, type_oid);
		return;
	}
	case ::duckdb::PhysicalType::INT64: {
		::duckdb::FlatVector::GetData<int64_t>(vector)[row_index] =
		    DatumAsInt64(datum, type_oid);
		return;
	}
	case ::duckdb::PhysicalType::DOUBLE: {
		::duckdb::FlatVector::GetData<double>(vector)[row_index] =
		    DatumAsDouble(datum, type_oid);
		return;
	}
	case ::duckdb::PhysicalType::VARCHAR: {
		const std::string text = DatumText(datum, type_oid);
		::duckdb::FlatVector::GetData<::duckdb::string_t>(vector)[row_index] =
		    ::duckdb::StringVector::AddString(vector, text);
		return;
	}
	default:
		throw std::runtime_error("unsupported postgres scan column physical type");
	}
}

void AppendProjectedRow(::duckdb::DataChunk &chunk, ::duckdb::idx_t row_index,
                        TupleTableSlot *slot,
                        const std::vector<BoundColumn> &columns) {
	for (::duckdb::idx_t column_index = 0; column_index < columns.size();
	     ++column_index) {
		const auto &column = columns[column_index];
		bool is_null = false;
		const Datum datum = slot_getattr(slot, column.attnum, &is_null);
		auto &vector = chunk.data[column_index];
		vector.SetVectorType(::duckdb::VectorType::FLAT_VECTOR);
		AppendFlatValue(vector, row_index, datum, is_null, column.type_oid,
		                column.spec);
	}
}

Relation OpenScanRelation(const NativeScanSpec &scan_spec) {
	if (scan_spec.table_name.empty()) {
		throw std::runtime_error("postgres native scan requires a table name");
	}
	RangeVar *range_var = makeRangeVar(
	    scan_spec.schema_name.empty() ? nullptr
	                                  : pstrdup(scan_spec.schema_name.c_str()),
	    pstrdup(scan_spec.table_name.c_str()), -1);
	Relation relation = table_openrv(range_var, AccessShareLock);
	if (range_var->schemaname) {
		pfree(range_var->schemaname);
	}
	pfree(range_var->relname);
	pfree(range_var);
	return relation;
}

} // namespace

namespace {
thread_local int xact_callback_depth = 0;
} // namespace

void EnterPostgresXactCallback() { ++xact_callback_depth; }
void LeavePostgresXactCallback() { --xact_callback_depth; }

void ScanNativeBaseTable(const NativeScanSpec &scan_spec,
                         const NativeScanChunkConsumer &consumer) {
	Relation relation = OpenScanRelation(scan_spec);
	TupleDesc tuple_desc = RelationGetDescr(relation);
	const auto columns = BindProjectedColumns(tuple_desc, scan_spec);
	const auto filters = BindFilters(tuple_desc, scan_spec);

	TupleTableSlot *slot = table_slot_create(relation, nullptr);
	TableScanDesc scan =
	    table_beginscan(relation, GetActiveSnapshot(), 0, nullptr);

	::duckdb::vector<::duckdb::LogicalType> types;
	types.reserve(columns.size());
	for (const auto &column : columns) {
		types.push_back(column.spec.expected_logical_type);
	}
	::duckdb::DataChunk chunk;
	chunk.Initialize(::duckdb::Allocator::DefaultAllocator(), types);

	::duckdb::idx_t batch_count = 0;
	auto flush = [&]() {
		if (batch_count == 0) {
			return;
		}
		chunk.SetCardinality(batch_count);
		consumer(chunk);
		chunk.Reset();
		batch_count = 0;
	};

	while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
		CHECK_FOR_INTERRUPTS();
		if (!PassesFilters(slot, filters)) {
			continue;
		}
		AppendProjectedRow(chunk, batch_count, slot, columns);
		++batch_count;
		if (batch_count >= kScanBatchSize) {
			flush();
		}
	}
	flush();

	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	table_close(relation, AccessShareLock);
}

void RunParallelNativeScan(
    const NativeScanSpec &scan_spec, ::duckdb::idx_t worker_count,
    const std::function<void(::duckdb::idx_t worker_id, ::duckdb::DataChunk &chunk)>
        &consume) {
	// std::thread inside a PostgreSQL xact callback SIGSEGVs (backend process
	// terminated by signal 11 during COMMIT of an invalidating UPDATE).
	if (xact_callback_depth > 0) {
		worker_count = 1;
	}
	if (worker_count <= 1) {
		ScanNativeBaseTable(scan_spec, [&](::duckdb::DataChunk &chunk) { consume(0, chunk); });
		return;
	}

	std::vector<std::unique_ptr<::duckdb::DataChunk>> batches;
	ScanNativeBaseTable(scan_spec, [&](::duckdb::DataChunk &chunk) {
		auto copy = std::make_unique<::duckdb::DataChunk>();
		copy->Initialize(::duckdb::Allocator::DefaultAllocator(), chunk.GetTypes());
		chunk.Copy(*copy);
		batches.push_back(std::move(copy));
	});

	std::atomic<::duckdb::idx_t> next{0};
	std::mutex error_mutex;
	std::exception_ptr error;
	auto run_worker = [&](::duckdb::idx_t worker_id) {
		try {
			while (true) {
				const auto index = next.fetch_add(1, std::memory_order_relaxed);
				if (index >= batches.size()) {
					return;
				}
				consume(worker_id, *batches[index]);
			}
		} catch (...) {
			std::lock_guard<std::mutex> lock(error_mutex);
			if (!error) {
				error = std::current_exception();
			}
		}
	};

	std::vector<std::thread> threads;
	threads.reserve(worker_count - 1);
	for (::duckdb::idx_t worker_id = 1; worker_id < worker_count; ++worker_id) {
		threads.emplace_back(run_worker, worker_id);
	}
	run_worker(0);
	for (auto &thread : threads) {
		thread.join();
	}
	if (error) {
		std::rethrow_exception(error);
	}
}

} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
