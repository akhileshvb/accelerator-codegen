// GENERATED BACKEND — engine scan adapter .cpp stub
#include "accelerator_codegen/runtime/{{engine_name}}/scan_adapter.hpp"

namespace accelerator_codegen::aux_state::engine::{{engine_name}} {

void ScanNativeBaseTable(const NativeScanSpec &scan_spec,
                         const NativeScanChunkConsumer &consumer) {
  // TODO: resolve table via native storage APIs; project/filter into DataChunks;
  // invoke consumer for each chunk. No client-SQL round-trip on the hot path.
  (void)scan_spec;
  (void)consumer;
}

} // namespace accelerator_codegen::aux_state::engine::{{engine_name}}
