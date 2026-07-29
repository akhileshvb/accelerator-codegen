#pragma once

#include "accelerator_codegen/tpch_q10_maintenance.hpp"

namespace accelerator_codegen {

void MaybeAppendQ10DecisionCsv(const Q10MaintenanceDecisionRecord &record);

} // namespace accelerator_codegen
