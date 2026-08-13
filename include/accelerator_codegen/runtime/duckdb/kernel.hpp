// GENERATED BACKEND
#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {
class DataChunk;
}

namespace accelerator_codegen {

class KernelInstance {
public:
    virtual ~KernelInstance() = default;
    virtual void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) = 0;
    virtual void Finalize() = 0;
    virtual bool ProduceOutput(duckdb::DataChunk &output) = 0;
};

} // namespace accelerator_codegen
