#pragma once

#include <vector>

#include "duckdb/accelerator/mutation_recorder.hpp"

namespace accelerator_codegen::aux_state {

struct PostgresMutationLogRow {
	int kind = 0;
	uint64_t table_oid = 0;
	std::string table_name;
	std::vector<int> updated_column_attnums;
	std::vector<int> col_attnums;
	std::vector<std::string> col_old;
	std::vector<std::string> col_new;
};

std::vector<duckdb::TableMutationEvent> DecodePostgresMutationLog(
    const std::vector<PostgresMutationLogRow> &rows, const std::vector<TableDependency> &dependencies);

} // namespace accelerator_codegen::aux_state
