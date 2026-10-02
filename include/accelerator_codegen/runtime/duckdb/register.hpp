// GENERATED BACKEND
#pragma once

namespace duckdb {
class Connection;
class TableFunctionSet;
} // namespace duckdb

namespace accelerator_codegen {

// Registers every accelerator table function on the connection's system
// catalog: the base kernels' function sets (registered below by each query's
// directory) and the aux-state table functions.
void RegisterAcceleratorFunctions(duckdb::Connection &connection);

// A base kernel's table function set.  Each query's directory registers its
// factory with a static registrar, e.g. in
// src/generated/<bench>/q<N>/duckdb/base/single-threaded/register.cpp:
//
//   duckdb::TableFunctionSet CreateTpchQ6FunctionSet();
//   static const BaseFunctionSetRegistrar kRegistrar{&CreateTpchQ6FunctionSet};
using TableFunctionSetFactory = duckdb::TableFunctionSet (*)();
void RegisterBaseFunctionSet(TableFunctionSetFactory factory);
struct BaseFunctionSetRegistrar {
    explicit BaseFunctionSetRegistrar(TableFunctionSetFactory factory) { RegisterBaseFunctionSet(factory); }
};

} // namespace accelerator_codegen
