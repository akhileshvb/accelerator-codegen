#pragma once

#include "accelerator_codegen/tpch_q6_maintenance.hpp"

namespace accelerator_codegen {

void MaybeAppendQ6DecisionCsv(const Q6MaintenanceDecisionRecord &record);

} // namespace accelerator_codegen
