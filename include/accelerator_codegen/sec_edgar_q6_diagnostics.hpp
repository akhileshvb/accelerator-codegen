#pragma once

#include "accelerator_codegen/sec_edgar_q6_maintenance.hpp"

namespace accelerator_codegen {

void MaybeAppendSecEdgarQ6DecisionCsv(const SecEdgarQ6MaintenanceDecisionRecord &record);

} // namespace accelerator_codegen
