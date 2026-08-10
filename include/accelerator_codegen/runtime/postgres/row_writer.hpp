#pragma once

#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include <functional>
#include <string>
#include <vector>

namespace accelerator_codegen::aux_state::engine::postgres {

using PostgresRowWriter =
    std::function<void(const std::vector<std::vector<std::string>> &rows)>;

} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
