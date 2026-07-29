#pragma once

#include "accelerator_codegen/tpch_q12_maintenance.hpp"

namespace accelerator_codegen {

void MaybeAppendQ12DecisionCsv(const Q12MaintenanceDecisionRecord &record);

} // namespace accelerator_codegen
