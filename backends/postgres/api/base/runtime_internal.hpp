// Internal declarations shared by the base PostgreSQL runtime files.
#pragma once

#include "backends/postgres/api/base/pg_base.hpp"

#include <deque>
#include <string_view>
#include <utility>

namespace accelerator_codegen::pg_base {

std::deque<KernelEntry> &KernelRegistry();
const KernelEntry *FindKernel(int query_id, bool parallel);
TaskFn FindTask(const char *name);

//! Wire form of a batch between a parallel worker and the leader
//! (ScanTableParallel, ScanTablePartitioned): row count, then per column its values (Int32/Date:
//! int32 each, Int64/Decimal/Double: 8 bytes each, Text: a uint32 length per row,
//! then all the bytes), each nullable column followed by its NULL mask (a byte
//! per row).
std::string EncodeBatch(const ScanSpec &spec, const Batch &batch);

} // namespace accelerator_codegen::pg_base
