// accel_tpch_q1_verus(day_offset): stateless TPC-H Q1 on DuckDB, aggregated by
// the Verus-verified kernel (src/verus/q1).
//
// TRUSTED GLUE.  The native scan (the same scan spec as accel_tpch_q1, with
// l_shipdate <= cutoff pushed down) delivers DataChunks; this file only
// decodes each chunk's seven columns into flat arrays (DECIMAL(15,2) as its
// int64 hundredths, DATE as days since 1970-01-01, VARCHAR flags as their one
// byte) and hands them to the kernel, then formats the kernel's exact sums as
// DECIMAL(38, s) and the AVG columns as DOUBLE (sum / 10^2 / count, the same
// formula as the C++ accel_tpch_q1).  NULLs, flags that are not exactly one
// byte, and kernel refusals raise an error (this stateless entry point has no
// SQL fallback).  The per-row NULL check is skipped when lineitem declares all
// seven columns NOT NULL (the engine enforces it).
#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "accelerator_codegen/verus/q1_verus.h"
#include "tpch/q1/duckdb/base/query.hpp"
#ifdef ACCEL_ENABLE_PARALLEL_TPCH
#include "accelerator_codegen/runtime/parallel/parallel_kernel.hpp"
#include "accelerator_codegen/runtime/parallel/parallel_runtime.hpp"
#endif

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

using verus_q1::Q1Batch;
using verus_q1::Q1Kernel;

duckdb::hugeint_t ToHugeint(__int128 v) {
    return duckdb::hugeint_t(static_cast<int64_t>(v >> 64), static_cast<uint64_t>(v));
}

double AvgScale2(__int128 sum, int64_t count) {
    double scaled = duckdb::Hugeint::Cast<double>(ToHugeint(sum));
    scaled /= 10.0;
    scaled /= 10.0;
    return scaled / static_cast<double>(count);
}

//! Whether the engine enforces NOT NULL on all seven Q1 columns (then the
//! per-row NULL check is skipped: the storage cannot hold a NULL there).
struct Q1VerusFactoryData final : public QueryKernelFactoryData {
    bool columns_not_null = false;
    std::unique_ptr<QueryKernelFactoryData> Copy() const override {
        return std::make_unique<Q1VerusFactoryData>(*this);
    }
    bool Equals(const QueryKernelFactoryData &other) const override {
        return columns_not_null == dynamic_cast<const Q1VerusFactoryData &>(other).columns_not_null;
    }
};

bool LineitemQ1ColumnsNotNull(duckdb::ClientContext &context) {
    auto &table = duckdb::Catalog::GetEntry<duckdb::TableCatalogEntry>(context, INVALID_CATALOG, DEFAULT_SCHEMA,
                                                                       "lineitem");
    const char *names[] = {"l_shipdate", "l_quantity", "l_extendedprice", "l_discount",
                           "l_tax",      "l_returnflag", "l_linestatus"};
    for (const char *name : names) {
        const auto logical = table.GetColumn(name).Logical();
        bool found = false;
        for (const auto &constraint : table.GetConstraints()) {
            if (constraint->type == duckdb::ConstraintType::NOT_NULL &&
                constraint->Cast<duckdb::NotNullConstraint>().index == logical) {
                found = true;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

//! Decode one chunk's seven columns into `batch`.  Returns an error message,
//! or nullptr.  Column at a time: each column gathered through its selection.
const char *DecodeChunk(duckdb::DataChunk &chunk, Q1Batch &batch, bool columns_not_null) {
    const duckdb::idx_t n = chunk.size();
    batch.n = 0;
    if (n == 0) {
        return nullptr;
    }
    if (batch.Capacity() < n) {
        batch.Reserve(n);
    }
    duckdb::UnifiedVectorFormat f[7];
    bool all_valid = true;
    for (duckdb::idx_t c = 0; c < 7; c++) {
        chunk.data[c].ToUnifiedFormat(n, f[c]);
        all_valid = all_valid && f[c].validity.AllValid();
    }
    if (!all_valid && !columns_not_null) {
        for (duckdb::idx_t c = 0; c < 7; c++) {
            for (duckdb::idx_t r = 0; r < n; r++) {
                if (!f[c].validity.RowIsValid(f[c].sel->get_index(r))) {
                    return "NULL in a Q1 column";
                }
            }
        }
    }
    auto gather_i64 = [&](duckdb::idx_t c, int64_t *out) {
        const auto *src = duckdb::UnifiedVectorFormat::GetData<int64_t>(f[c]);
        const auto &sel = *f[c].sel;
        for (duckdb::idx_t r = 0; r < n; r++) {
            out[r] = src[sel.get_index(r)];
        }
    };
    auto gather_flag = [&](duckdb::idx_t c, uint8_t *out) {
        const auto *src = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(f[c]);
        const auto &sel = *f[c].sel;
        bool one_byte = true;
        for (duckdb::idx_t r = 0; r < n; r++) {
            const auto &v = src[sel.get_index(r)];
            one_byte = one_byte && v.GetSize() == 1;
            out[r] = static_cast<uint8_t>(v.GetPrefix()[0]);
        }
        return one_byte;
    };
    {
        const auto *src = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(f[Q1LineitemChunkLayout::SHIPDATE_INDEX]);
        const auto &sel = *f[Q1LineitemChunkLayout::SHIPDATE_INDEX].sel;
        for (duckdb::idx_t r = 0; r < n; r++) {
            batch.shipdate[r] = src[sel.get_index(r)].days;
        }
    }
    gather_i64(Q1LineitemChunkLayout::QUANTITY_INDEX, batch.quantity.data());
    gather_i64(Q1LineitemChunkLayout::EXTENDEDPRICE_INDEX, batch.extendedprice.data());
    gather_i64(Q1LineitemChunkLayout::DISCOUNT_INDEX, batch.discount.data());
    gather_i64(Q1LineitemChunkLayout::TAX_INDEX, batch.tax.data());
    if (!gather_flag(Q1LineitemChunkLayout::RETURNFLAG_INDEX, batch.returnflag.data()) ||
        !gather_flag(Q1LineitemChunkLayout::LINESTATUS_INDEX, batch.linestatus.data())) {
        return "flag is not exactly one byte";
    }
    batch.n = n;
    return nullptr;
}

//! Write groups[next..] into `output`; false when all are written.
bool EmitGroups(const std::vector<q1v_group> &groups, size_t &next, duckdb::DataChunk &output) {
    if (next >= groups.size()) {
        return false;
    }
    duckdb::idx_t row = 0;
    for (; next < groups.size() && row < STANDARD_VECTOR_SIZE; next++) {
        const auto &g = groups[next];
        output.SetValue(0, row, duckdb::Value(std::string(1, static_cast<char>(g.returnflag))));
        output.SetValue(1, row, duckdb::Value(std::string(1, static_cast<char>(g.linestatus))));
        output.SetValue(2, row, duckdb::Value::DECIMAL(ToHugeint(g.sum_qty), 38, 2));
        output.SetValue(3, row, duckdb::Value::DECIMAL(ToHugeint(g.sum_base_price), 38, 2));
        output.SetValue(4, row, duckdb::Value::DECIMAL(ToHugeint(g.sum_disc_price), 38, 4));
        output.SetValue(5, row, duckdb::Value::DECIMAL(ToHugeint(g.sum_charge), 38, 6));
        output.SetValue(6, row, duckdb::Value::DOUBLE(AvgScale2(g.sum_qty, g.count)));
        output.SetValue(7, row, duckdb::Value::DOUBLE(AvgScale2(g.sum_base_price, g.count)));
        output.SetValue(8, row, duckdb::Value::DOUBLE(AvgScale2(g.sum_disc, g.count)));
        output.SetValue(9, row, duckdb::Value::BIGINT(g.count));
        row++;
    }
    output.SetCardinality(row);
    return true;
}

[[noreturn]] void Fail(const char *why) {
    throw duckdb::InvalidInputException(std::string("accel_tpch_q1_verus: ") + why);
}

class Q1VerusKernel final : public KernelInstance {
public:
    Q1VerusKernel(const Q1Parameters &params, bool simple, bool columns_not_null)
        : kernel(static_cast<int32_t>(params.cutoff_date.days), simple), batch(STANDARD_VECTOR_SIZE),
          columns_not_null(columns_not_null) {
    }

    void ConsumeInput(duckdb::idx_t, duckdb::DataChunk &chunk) override {
        if (const char *err = DecodeChunk(chunk, batch, columns_not_null)) {
            Fail(err);
        }
        if (!kernel.Consume(batch)) {
            Fail("row outside the verified kernel's domain");
        }
    }

    void Finalize() override {
        if (!kernel.Finish(groups)) {
            Fail("kernel refused");
        }
    }

    bool ProduceOutput(duckdb::DataChunk &output) override {
        return EmitGroups(groups, next, output);
    }

private:
    Q1Kernel kernel;
    Q1Batch batch;
    std::vector<q1v_group> groups;
    size_t next = 0;
    bool columns_not_null = false;
};

#ifdef ACCEL_ENABLE_PARALLEL_TPCH
//! One verified state per scan worker; the states are combined with the
//! verified merge (rows := rows ++ other.rows) in a pairwise tree.
class Q1VerusParallelKernel final : public ParallelKernelInstance {
public:
    Q1VerusParallelKernel(const Q1Parameters &params, bool columns_not_null)
        : cutoff(static_cast<int32_t>(params.cutoff_date.days)), columns_not_null(columns_not_null) {
    }

    void RunParallel(duckdb::ClientContext &context,
                     const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
        struct Worker {
            explicit Worker(int32_t cutoff) : kernel(cutoff), batch(STANDARD_VECTOR_SIZE) {
            }
            Q1Kernel kernel;
            Q1Batch batch;
            const char *error = nullptr;
        };
        const auto worker_count = ParallelWorkerCount(context);
        std::vector<std::unique_ptr<Worker>> workers(worker_count);
        RunParallelChunkScan(context, *bound_inputs[0], worker_count,
                             [&](duckdb::idx_t id, duckdb::DataChunk &chunk) {
                                 auto &w = workers[id];
                                 if (!w) {
                                     w = std::make_unique<Worker>(cutoff);
                                 }
                                 if (w->error) {
                                     return;
                                 }
                                 if (const char *err = DecodeChunk(chunk, w->batch, columns_not_null)) {
                                     w->error = err;
                                 } else if (!w->kernel.Consume(w->batch)) {
                                     w->error = "row outside the verified kernel's domain";
                                 }
                             });
        std::vector<Q1Kernel *> live;
        for (auto &w : workers) {
            if (w) {
                if (w->error) {
                    Fail(w->error);
                }
                live.push_back(&w->kernel);
            }
        }
        if (live.empty()) {
            workers.emplace_back(std::make_unique<Worker>(cutoff));
            live.push_back(&workers.back()->kernel);
        }
        while (live.size() > 1) {
            const size_t half = live.size() / 2;
            std::vector<std::thread> threads;
            std::vector<char> ok(half, 1);
            for (size_t i = 0; i < half; i++) {
                threads.emplace_back([&, i] { ok[i] = live[i]->Merge(*live[live.size() - 1 - i]); });
            }
            for (auto &t : threads) {
                t.join();
            }
            for (char o : ok) {
                if (!o) {
                    Fail("kernel refused a merge");
                }
            }
            live.resize(live.size() - half);
        }
        if (!live[0]->Finish(groups)) {
            Fail("kernel refused");
        }
    }

    void Finalize() override {
    }

    bool ProduceOutput(duckdb::DataChunk &output) override {
        return EmitGroups(groups, next, output);
    }

private:
    int32_t cutoff;
    bool columns_not_null;
    std::vector<q1v_group> groups;
    size_t next = 0;
};
#endif

QueryBindResult BindQ1VerusPackage(duckdb::ClientContext &context, duckdb::TableFunctionBindInput &input) {
    if (input.inputs.size() != 1) {
        throw duckdb::BinderException("accel_tpch_q1_verus expects exactly 1 argument");
    }
    auto parameters = MakeQ1Parameters(input.inputs[0].GetValue<int32_t>());
    QueryBindResult result;
    result.query_parameters = std::make_unique<Q1Parameters>(parameters);
    result.input_scans.push_back(CreateQ1ScanSpec(parameters));
    auto factory = std::make_unique<Q1VerusFactoryData>();
    factory->columns_not_null = LineitemQ1ColumnsNotNull(context);
    result.kernel_factory_data = std::move(factory);
    result.output_schema.names = {"l_returnflag", "l_linestatus", "sum_qty", "sum_base_price", "sum_disc_price",
                                  "sum_charge", "avg_qty", "avg_price", "avg_disc", "count_order"};
    result.output_schema.types = {
        duckdb::LogicalType::VARCHAR,          duckdb::LogicalType::VARCHAR,
        duckdb::LogicalType::DECIMAL(38, 2),   duckdb::LogicalType::DECIMAL(38, 2),
        duckdb::LogicalType::DECIMAL(38, 4),   duckdb::LogicalType::DECIMAL(38, 6),
        duckdb::LogicalType::DOUBLE,           duckdb::LogicalType::DOUBLE,
        duckdb::LogicalType::DOUBLE,           duckdb::LogicalType::BIGINT,
    };
    return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ1VerusKernelInstance(const QueryParameters &parameters,
                                                               const QueryKernelFactoryData *factory) {
    return duckdb::make_uniq<Q1VerusKernel>(dynamic_cast<const Q1Parameters &>(parameters), false,
                                            dynamic_cast<const Q1VerusFactoryData &>(*factory).columns_not_null);
}

duckdb::unique_ptr<KernelInstance> CreateQ1VerusSimpleKernelInstance(const QueryParameters &parameters,
                                                                     const QueryKernelFactoryData *factory) {
    return duckdb::make_uniq<Q1VerusKernel>(dynamic_cast<const Q1Parameters &>(parameters), true,
                                            dynamic_cast<const Q1VerusFactoryData &>(*factory).columns_not_null);
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ1VerusFunctionSet() {
    QueryPackage package;
    package.function_name = "accel_tpch_q1_verus";
    package.overloads = {
        {{duckdb::LogicalType::INTEGER}},
        {{duckdb::LogicalType::BIGINT}},
    };
    package.bind_query = BindQ1VerusPackage;
    package.create_kernel = CreateQ1VerusKernelInstance;
    return CreateRuntimeBackedFunctionSet(package);
}

#ifdef ACCEL_ENABLE_PARALLEL_TPCH
namespace {
duckdb::unique_ptr<KernelInstance> CreateQ1VerusParallelKernelInstance(const QueryParameters &parameters,
                                                                       const QueryKernelFactoryData *factory) {
    return duckdb::make_uniq<Q1VerusParallelKernel>(
        dynamic_cast<const Q1Parameters &>(parameters),
        dynamic_cast<const Q1VerusFactoryData &>(*factory).columns_not_null);
}
} // namespace

duckdb::TableFunctionSet CreateTpchQ1VerusParallelFunctionSet() {
    QueryPackage package;
    package.function_name = "accel_tpch_q1_verus_parallel";
    package.overloads = {
        {{duckdb::LogicalType::INTEGER}},
        {{duckdb::LogicalType::BIGINT}},
    };
    package.bind_query = BindQ1VerusPackage;
    package.create_kernel = CreateQ1VerusParallelKernelInstance;
    return CreateRuntimeBackedFunctionSet(package);
}
#endif

duckdb::TableFunctionSet CreateTpchQ1VerusSimpleFunctionSet() {
    QueryPackage package;
    package.function_name = "accel_tpch_q1_verus_simple";
    package.overloads = {
        {{duckdb::LogicalType::INTEGER}},
        {{duckdb::LogicalType::BIGINT}},
    };
    package.bind_query = BindQ1VerusPackage;
    package.create_kernel = CreateQ1VerusSimpleKernelInstance;
    return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
