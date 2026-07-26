#pragma once

#include "accelerator_codegen/tpch_q14_maintenance.hpp"

namespace accelerator_codegen {

void MaybeAppendQ14DecisionCsv(const Q14MaintenanceDecisionRecord &record);

}
