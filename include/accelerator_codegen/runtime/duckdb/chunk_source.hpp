// GENERATED BACKEND
#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {
class DataChunk;
}

namespace accelerator_codegen {

class ChunkSource {
public:
    virtual ~ChunkSource() = default;
    virtual bool Next() = 0;
    virtual duckdb::DataChunk &Chunk() = 0;
};

} // namespace accelerator_codegen
