#pragma once

#include "accelerator_codegen/sec_edgar_q1_maintenance.hpp"

namespace accelerator_codegen {

void MaybeAppendSecEdgarQ1DecisionCsv(const SecEdgarQ1MaintenanceDecisionRecord &record);

} // namespace accelerator_codegen
