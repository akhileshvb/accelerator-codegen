#pragma once

#include "accelerator_codegen/sec_edgar_q2_maintenance.hpp"

namespace accelerator_codegen {

void MaybeAppendSecEdgarQ2DecisionCsv(const SecEdgarQ2MaintenanceDecisionRecord &record);

} // namespace accelerator_codegen
