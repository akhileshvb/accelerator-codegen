#pragma once

#include "accelerator_codegen/sec_edgar_q4_maintenance.hpp"

namespace accelerator_codegen {

void MaybeAppendSecEdgarQ4DecisionCsv(const SecEdgarQ4MaintenanceDecisionRecord &record);

} // namespace accelerator_codegen
