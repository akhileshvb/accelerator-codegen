#include "accelerator_codegen/register.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace {

void Require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

int main() {
    try {
        duckdb::DuckDB database(nullptr);
        duckdb::Connection connection(database);

        accelerator_codegen::RegisterAcceleratorFunctions(connection);

        auto result = connection.Query("SELECT * FROM accelerator_hello()");
        Require(result != nullptr, "registration query returned null result");
        Require(!result->HasError(), result->GetError());
        Require(result->names.size() == 1, "expected one output column");
        Require(result->names[0] == "value", "expected output column named 'value'");
        Require(result->types.size() == 1, "expected one output type");
        Require(result->types[0] == duckdb::LogicalType::BIGINT, "expected BIGINT output type");

        auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
        Require(materialized.RowCount() == 1, "expected one output row");
        Require(materialized.GetValue(0, 0).GetValue<int64_t>() == 42, "expected accelerator_hello() to return 42");

        std::cout << "registration_smoke_test passed" << std::endl;
        return EXIT_SUCCESS;
    } catch (const std::exception &ex) {
        std::cerr << "registration_smoke_test failed: " << ex.what() << std::endl;
        return EXIT_FAILURE;
    }
}
