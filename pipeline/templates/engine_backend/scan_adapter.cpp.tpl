// GENERATED BACKEND — engine-local scan helper .cpp stub
#include "accelerator_codegen/runtime/{{engine_name}}/scan_adapter.hpp"

namespace accelerator_codegen::aux_state::engine::{{engine_name}} {

void ScanNativeBaseTableForRebuild(const {{EngineName}}ScanSpec &scan_spec,
                                   const {{EngineName}}ScanRowConsumer &consumer) {
  // TODO: resolve table via native storage / table-AM APIs; project/filter into
  // engine-local row/slot batches; invoke consumer per row or batch.
  // No client-SQL round-trip. No DataChunk portable IR.
  (void)scan_spec;
  (void)consumer;
}

} // namespace accelerator_codegen::aux_state::engine::{{engine_name}}
