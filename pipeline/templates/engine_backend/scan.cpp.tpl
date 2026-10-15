// GENERATED BACKEND — S5/S8 engine-local scans for {{engine_name}} (see scan.hpp.tpl).
#include "backends/{{engine_name}}/engine/scan.hpp"

#include <stdexcept>

namespace accelerator_codegen::aux_state::engine::{{engine_name}} {

void Scan{{EngineName}}Table(const {{EngineName}}ScanSpec &, std::size_t, const {{EngineName}}ChunkConsumer &) {
  throw std::logic_error("S8 scan");
}

} // namespace accelerator_codegen::aux_state::engine::{{engine_name}}
