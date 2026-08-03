#pragma once

#include "accelerator_codegen/sec_edgar_q3_maintenance.hpp"

namespace accelerator_codegen {

void MaybeAppendSecEdgarQ3DecisionCsv(const SecEdgarQ3MaintenanceDecisionRecord &record);

} // namespace accelerator_codegen
