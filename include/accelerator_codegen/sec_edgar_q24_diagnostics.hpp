#pragma once

#include "accelerator_codegen/sec_edgar_q24_maintenance.hpp"

namespace accelerator_codegen {

void MaybeAppendSecEdgarQ24DecisionCsv(const SecEdgarQ24MaintenanceDecisionRecord &record);

} // namespace accelerator_codegen
