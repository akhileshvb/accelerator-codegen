#include "{{benchmark_name}}/q{{query_number}}/{{engine_name}}/base/query.hpp"
{{api_includes}}
// Query package of {{benchmark_name}} Q{{query_number}} ({{function_name}}) for {{engine_name}}.
// Output columns: {{output_schema_names}}
// Binds the parameters, the input scan and the kernel through the engine's API:
{{api_manifest}}

namespace accelerator_codegen {

// TODO({{engine_name}}): {{create_fn}} builds the package from the symbols above.

} // namespace accelerator_codegen
