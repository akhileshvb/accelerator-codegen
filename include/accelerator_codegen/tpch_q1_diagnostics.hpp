#pragma once

#include "accelerator_codegen/tpch_q1_maintenance.hpp"

namespace accelerator_codegen {

void MaybeAppendQ1DecisionCsv(const Q1MaintenanceDecisionRecord &record);

} // namespace accelerator_codegen
