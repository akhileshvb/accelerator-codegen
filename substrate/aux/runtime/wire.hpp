// SCAFFOLDING
// Compact binary encoding of a ColumnarBatch, query arguments and result rows,
// for shipping them across a process boundary. Engine-neutral. Little-endian
// hosts only. Every Encode* appends to `out`; every Decode* consumes its bytes
// from the front of `in` and throws std::runtime_error on malformed input.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "substrate/aux/core/aux_types.hpp"
#include "substrate/aux/runtime/columnar_batch.hpp"

namespace accelerator_codegen::aux_state::runtime {

// Layout (all integers little-endian, all lengths 64-bit):
//   u8 version | u64 table | u8 kind | u64 row_count
//   ColumnSet old_rows | ColumnSet new_rows | u64 n, i32 updated_attnums[n]
// ColumnSet: u64 n | i32 attnums[n] | Column[n]
// Column:    u8 type | i32 scale | u8 has_nulls | u8 null_flags[row_count] (if has_nulls)
//            | values: the raw array of row_count elements (Boolean 1 byte, Int64,
//              UInt64, ScaledDecimal, Double 8, Date 4), or for String
//              u64 offsets[row_count + 1] | u64 byte_count | bytes
void EncodeBatch(const ColumnarBatch &batch, std::string &out);
ColumnarBatch DecodeBatch(std::string_view &in);

// u64 count | AuxValue[count]
// AuxValue: u8 type | u8 is_null | payload (absent when null; Boolean 1 byte,
//           Int64/UInt64/Double 8, ScaledDecimal i64 + i32, Date 4,
//           String u64 length + bytes)
void EncodeArguments(const core::QueryArguments &args, std::string &out);
core::QueryArguments DecodeArguments(std::string_view &in);

// u64 row_count | (u64 cell_count | AuxValue[cell_count])[row_count]
void EncodeRows(const std::vector<core::AuxRow> &rows, std::string &out);
std::vector<core::AuxRow> DecodeRows(std::string_view &in);

} // namespace accelerator_codegen::aux_state::runtime
