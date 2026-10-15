// GENERATED BACKEND — S1 capture for {{engine_name}} (see capture.hpp.tpl).
#include "backends/{{engine_name}}/engine/capture.hpp"

#include <stdexcept>

namespace accelerator_codegen::aux_state::engine::{{engine_name}} {

runtime::ColumnarBatch To{{EngineName}}ColumnarBatch(const void *, core::TableId) {
  throw std::logic_error("S1 capture");
}

} // namespace accelerator_codegen::aux_state::engine::{{engine_name}}
