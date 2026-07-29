#pragma once

#include "accelerator_codegen/tpch_q3_maintenance.hpp"

namespace accelerator_codegen {

void MaybeAppendQ3DecisionCsv(const Q3MaintenanceDecisionRecord &record);

} // namespace accelerator_codegen
