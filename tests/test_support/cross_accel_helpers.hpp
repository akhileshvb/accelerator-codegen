#pragma once

#include <string>
#include <vector>

#include "duckdb.hpp"

namespace accelerator_codegen::test_support {

enum class CrossAccelBenchmark { Tpch, SecEdgar };

CrossAccelBenchmark DetectCrossAccelBenchmark();
void EnableCrossAccelHybridMaintenance();
std::string CrossAccelDatabasePath();
std::string CrossAccelMutableDatabasePath();

std::string InvalidatingWriterSqlForQuery(int query_num, int bulk_row_limit = 1, int write_seq = 1);
void ApplyInvalidatingWriterSql(duckdb::Connection &connection, int query_num);
void ApplyMultiConnectionStressWriter(duckdb::Connection &connection, int query_num);
void BuildCrossAccelAux(duckdb::Connection &connection, int query_num);
std::string CrossAccelAuxSql(int query_num);
std::string CrossAccelReferenceSql(int query_num);
std::vector<int> CrossAccelAllQueries();
std::vector<int> CrossAccelTestQueries();
std::vector<int> CrossAccelQueriesIncludingTarget(int target_query);
void RequireCrossAccelQueryCorrectness(duckdb::Connection &connection, int query_num, const std::string &label);
void RequireCrossAccelCorrectness(duckdb::Connection &connection, const std::vector<int> &queries);

} // namespace accelerator_codegen::test_support
