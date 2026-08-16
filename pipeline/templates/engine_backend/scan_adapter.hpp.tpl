// GENERATED BACKEND — engine scan adapter template
#pragma once

#include "accelerator_codegen/runtime/native_scan.hpp"

namespace accelerator_codegen::aux_state::engine::{{engine_name}} {

// Stream projected base-table rows via native storage into DataChunks.
void ScanNativeBaseTable(const NativeScanSpec &scan_spec,
                         const NativeScanChunkConsumer &consumer);

} // namespace accelerator_codegen::aux_state::engine::{{engine_name}}
