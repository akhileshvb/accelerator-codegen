// Internal declarations shared by the stateless PostgreSQL runtime files.
#pragma once

#include "backends/postgres/api/stateless/pg_stateless.hpp"

#include <deque>
#include <utility>

namespace accelerator_codegen::pg_stateless {

std::deque<KernelEntry> &KernelRegistry();
const KernelEntry *FindKernel(int query_id, bool parallel);
TaskFn FindTask(const char *name);

} // namespace accelerator_codegen::pg_stateless
