#include "accelerator_codegen/query_runtime.hpp"

#include <stdexcept>
#include <utility>

#include "duckdb/catalog/dependency_list.hpp"
#include "duckdb/execution/execution_context.hpp"

namespace accelerator_codegen {
namespace {

struct RuntimePackageInfo : public duckdb::TableFunctionInfo {
    explicit RuntimePackageInfo(QueryPackage package_p) : package(std::move(package_p)) {
    }

    QueryPackage package;
};

struct BoundQueryData : public duckdb::FunctionData {
    BoundQueryData(QueryPackage package_p,
                   std::unique_ptr<QueryParameters> query_parameters_p,
                   std::unique_ptr<QueryKernelFactoryData> kernel_factory_data_p,
                   duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> bound_inputs_p,
                   OutputSchema output_schema_p)
        : package(std::move(package_p)),
          query_parameters(std::move(query_parameters_p)),
          kernel_factory_data(std::move(kernel_factory_data_p)),
          bound_inputs(std::move(bound_inputs_p)),
          output_schema(std::move(output_schema_p)) {
    }

    QueryPackage package;
    std::unique_ptr<QueryParameters> query_parameters;
    std::unique_ptr<QueryKernelFactoryData> kernel_factory_data;
    duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> bound_inputs;
    OutputSchema output_schema;

    duckdb::unique_ptr<duckdb::FunctionData> Copy() const override {
        duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> copied_inputs;
        copied_inputs.reserve(bound_inputs.size());
        for (const auto &input : bound_inputs) {
            copied_inputs.push_back(input->Copy());
        }
        return duckdb::make_uniq<BoundQueryData>(
            package, query_parameters ? query_parameters->Copy() : nullptr,
            kernel_factory_data ? kernel_factory_data->Copy() : nullptr, std::move(copied_inputs), output_schema);
    }

    bool Equals(const duckdb::FunctionData &other_p) const override {
        auto &other = other_p.Cast<BoundQueryData>();
        if (package.function_name != other.package.function_name || package.bind_query != other.package.bind_query ||
            package.create_kernel != other.package.create_kernel ||
            !OverloadsEqual(package.overloads, other.package.overloads)) {
            return false;
        }
        if (!OptionalEquals(query_parameters.get(), other.query_parameters.get())) {
            return false;
        }
        if (!OptionalEquals(kernel_factory_data.get(), other.kernel_factory_data.get())) {
            return false;
        }
        if (bound_inputs.size() != other.bound_inputs.size()) {
            return false;
        }
        for (duckdb::idx_t i = 0; i < bound_inputs.size(); i++) {
            if (!bound_inputs[i]->Equals(*other.bound_inputs[i])) {
                return false;
            }
        }
        return output_schema.types == other.output_schema.types && output_schema.names == other.output_schema.names;
    }

private:
    template <class T>
    static bool OptionalEquals(const T *lhs, const T *rhs) {
        if (!lhs || !rhs) {
            return lhs == rhs;
        }
        return lhs->Equals(*rhs);
    }

    static bool OverloadsEqual(const duckdb::vector<QueryFunctionOverload> &lhs,
                               const duckdb::vector<QueryFunctionOverload> &rhs) {
        if (lhs.size() != rhs.size()) {
            return false;
        }
        for (duckdb::idx_t i = 0; i < lhs.size(); i++) {
            if (lhs[i].sql_argument_types != rhs[i].sql_argument_types) {
                return false;
            }
        }
        return true;
    }
};

struct RuntimeQueryGlobalState : public duckdb::GlobalTableFunctionState {
    duckdb::idx_t MaxThreads() const override {
        return 1;
    }
};

struct RuntimeQueryLocalState : public duckdb::LocalTableFunctionState {
    duckdb::vector<duckdb::unique_ptr<ChunkSource>> inputs;
    duckdb::unique_ptr<KernelInstance> kernel;
    bool inputs_consumed = false;
    bool finalized = false;
    bool output_exhausted = false;
};

const RuntimePackageInfo &GetRuntimePackageInfo(const duckdb::TableFunctionBindInput &input) {
    if (!input.info) {
        throw std::runtime_error("runtime-backed table function is missing package info");
    }
    return input.info->Cast<RuntimePackageInfo>();
}

const BoundQueryData &GetBoundQueryData(const duckdb::FunctionData &bind_data) {
    return bind_data.Cast<BoundQueryData>();
}

duckdb::unique_ptr<duckdb::FunctionData> RuntimeBind(duckdb::ClientContext &context,
                                                     duckdb::TableFunctionBindInput &input,
                                                     duckdb::vector<duckdb::LogicalType> &return_types,
                                                     duckdb::vector<duckdb::string> &names) {
    const auto &package = GetRuntimePackageInfo(input).package;
    if (!package.bind_query) {
        throw std::runtime_error("runtime-backed table function is missing bind callback");
    }

    auto bind_result = package.bind_query(context, input);
    if (!bind_result.query_parameters) {
        throw std::runtime_error("query package bind returned null query parameters");
    }
    if (bind_result.output_schema.types.size() != bind_result.output_schema.names.size()) {
        throw std::runtime_error("query package bind returned mismatched output schema");
    }

    duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> bound_inputs;
    bound_inputs.reserve(bind_result.input_scans.size());
    for (const auto &scan_spec : bind_result.input_scans) {
        bound_inputs.push_back(BindNativeScan(context, scan_spec));
    }

    return_types = bind_result.output_schema.types;
    names = bind_result.output_schema.names;

    return duckdb::make_uniq<BoundQueryData>(package, std::move(bind_result.query_parameters),
                                             std::move(bind_result.kernel_factory_data), std::move(bound_inputs),
                                             std::move(bind_result.output_schema));
}

duckdb::unique_ptr<duckdb::GlobalTableFunctionState> RuntimeInit(duckdb::ClientContext &,
                                                                 duckdb::TableFunctionInitInput &) {
    return duckdb::make_uniq<RuntimeQueryGlobalState>();
}

duckdb::unique_ptr<duckdb::LocalTableFunctionState> RuntimeLocalInit(duckdb::ExecutionContext &context,
                                                                     duckdb::TableFunctionInitInput &input,
                                                                     duckdb::GlobalTableFunctionState *) {
    auto &bound_query = GetBoundQueryData(*input.bind_data);
    auto local_state = duckdb::make_uniq<RuntimeQueryLocalState>();
    local_state->inputs.reserve(bound_query.bound_inputs.size());
    for (const auto &bound_input : bound_query.bound_inputs) {
        local_state->inputs.push_back(CreateNativeChunkSource(context.client, *bound_input));
    }
    if (!bound_query.package.create_kernel) {
        throw std::runtime_error("runtime-backed table function is missing kernel factory");
    }
    local_state->kernel =
        bound_query.package.create_kernel(*bound_query.query_parameters, bound_query.kernel_factory_data.get());
    if (!local_state->kernel) {
        throw std::runtime_error("query package returned null kernel instance");
    }
    return local_state;
}

void RuntimeDependency(duckdb::LogicalDependencyList &dependencies, const duckdb::FunctionData *bind_data) {
    auto &bound_query = GetBoundQueryData(*bind_data);
    for (const auto &bound_input : bound_query.bound_inputs) {
        AddNativeTableDependencies(dependencies, bound_input.get());
    }
}

void RuntimeExec(duckdb::ClientContext &context,
                 duckdb::TableFunctionInput &input,
                 duckdb::DataChunk &output) {
    auto &bound_query = GetBoundQueryData(*input.bind_data);
    auto &local_state = input.local_state->Cast<RuntimeQueryLocalState>();

    if (!local_state.inputs_consumed) {
        for (duckdb::idx_t input_id = 0; input_id < local_state.inputs.size(); input_id++) {
            auto &source = local_state.inputs[input_id];
            while (source->Next()) {
                local_state.kernel->ConsumeInput(input_id, source->Chunk());
            }
        }
        local_state.inputs_consumed = true;
    }

    if (!local_state.finalized) {
        local_state.kernel->Finalize();
        local_state.finalized = true;
    }

    if (local_state.output_exhausted) {
        output.SetCardinality(0);
        return;
    }

    while (true) {
        if (output.ColumnCount() != bound_query.output_schema.types.size()) {
            output.Destroy();
            output.Initialize(context, bound_query.output_schema.types);
        }
        output.Reset();
        if (!local_state.kernel->ProduceOutput(output)) {
            local_state.output_exhausted = true;
            output.SetCardinality(0);
            return;
        }
        if (output.size() != 0) {
            return;
        }
    }
}

duckdb::TableFunction CreateRuntimeFunctionOverload(const QueryPackage &package,
                                                    const QueryFunctionOverload &overload) {
    duckdb::TableFunction function(package.function_name, overload.sql_argument_types, RuntimeExec, RuntimeBind, RuntimeInit,
                                   RuntimeLocalInit);
    function.dependency = RuntimeDependency;
    function.function_info = duckdb::make_shared_ptr<RuntimePackageInfo>(package);
    function.projection_pushdown = false;
    return function;
}

} // namespace

duckdb::TableFunctionSet CreateRuntimeBackedFunctionSet(const QueryPackage &package) {
    duckdb::TableFunctionSet function_set(package.function_name);
    for (const auto &overload : package.overloads) {
        function_set.AddFunction(CreateRuntimeFunctionOverload(package, overload));
    }
    return function_set;
}

} // namespace accelerator_codegen
