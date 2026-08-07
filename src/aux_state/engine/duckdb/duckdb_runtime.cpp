#include "accelerator_codegen/aux_state/engine/duckdb/duckdb_runtime.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

#include "accelerator_codegen/aux_state/query/sec_edgar/sec_edgar_q1_common.hpp"
#include "accelerator_codegen/aux_state/query/sec_edgar/sec_edgar_q2_common.hpp"
#include "accelerator_codegen/aux_state/query/sec_edgar/sec_edgar_q3_common.hpp"
#include "accelerator_codegen/aux_state/query/sec_edgar/sec_edgar_q4_common.hpp"
#include "accelerator_codegen/aux_state/query/sec_edgar/sec_edgar_q6_common.hpp"
#include "accelerator_codegen/aux_state/query/sec_edgar/sec_edgar_q24_common.hpp"
#include "accelerator_codegen/aux_state/query/tpch/tpch_q1_common.hpp"
#include "accelerator_codegen/aux_state/query/tpch/tpch_q3_common.hpp"
#include "accelerator_codegen/aux_state/query/tpch/tpch_q6_common.hpp"
#include "accelerator_codegen/aux_state/query/tpch/tpch_q10_common.hpp"
#include "accelerator_codegen/aux_state/query/tpch/tpch_q12_common.hpp"
#include "accelerator_codegen/aux_state/query/tpch/tpch_q14_common.hpp"
#include "accelerator_codegen/aux_state/core/aux_calibration.hpp"
#include "accelerator_codegen/aux_state/harness/cost_calibration.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace duckdb_backend = accelerator_codegen::aux_state::engine::duckdb;
namespace query = accelerator_codegen::aux_state::query;
using namespace query;

void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

int64_t DecimalRaw(const ::duckdb::Value &value) {
  return value.IsNull()
             ? 0
             : static_cast<int64_t>(std::llround(std::stod(value.ToString()) *
                                                  100.0));
}

int64_t SafeInt64(const ::duckdb::Value &value) {
  if (value.IsNull()) {
    return 0;
  }
  return static_cast<int64_t>(std::llround(std::stod(value.ToString())));
}

int32_t SafeInt32(const ::duckdb::Value &value) {
  return static_cast<int32_t>(SafeInt64(value));
}

int32_t DateDays(const ::duckdb::Value &value) {
  if (value.IsNull()) {
    return 0;
  }
  if (value.type().id() == ::duckdb::LogicalTypeId::DATE) {
    return ::duckdb::Date::EpochDays(value.GetValue<::duckdb::date_t>());
  }
  return SafeInt32(value);
}

template <typename Callback>
void ScanQuery(::duckdb::Connection &connection, const std::string &sql,
               Callback &&callback) {
  auto result = connection.Query(sql);
  Require(result != nullptr && !result->HasError(),
          result ? result->GetError() : "DuckDB scan returned null");
  for (::duckdb::idx_t row_index = 0; row_index < result->RowCount();
       ++row_index) {
    callback(*result, row_index);
  }
}

struct MaterializeSink final : core::AuxRowSink {
  void Begin(const core::OutputSchema &) override {}
  void WriteRow(const core::AuxRow &row) override { rows.push_back(row); }
  void End() override {}
  std::vector<core::AuxRow> rows;
};

struct Builder final : core::StateBuilder {
  void Write(core::StateImage image) override {
    images.push_back(std::move(image));
  }
  void Finish() override { finished = true; }
  std::vector<core::StateImage> images;
  bool finished = false;
};

template <typename QueryModule>
struct ModuleBinding : DuckdbRuntime::Binding {
  QueryModule module;

  template <typename... Args>
  ModuleBinding(Args &&...args) : module(std::forward<Args>(args)...) {}

  core::AuxQueryModule &Module() override { return module; }
  void Build(DuckDBAuxBackend &backend,
             core::StateBuilder &builder) override {
    module.Build(backend, builder);
  }
  void Execute(DuckDBAuxBackend &backend,
               const core::QueryArguments &arguments,
               core::AuxRowSink &sink) override {
    module.Execute(backend, arguments, sink);
  }
};

struct TpchQ1Scan : TpchQ1InitialScan {
  explicit TpchQ1Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanLineitems(
      const std::function<void(const TpchQ1LineitemInput &)> &consumer) override {
    ScanQuery(connection,
              "SELECT l_shipdate, l_quantity, l_extendedprice, l_discount, "
              "l_tax, l_returnflag, l_linestatus FROM lineitem",
              [&](auto &result, ::duckdb::idx_t row) {
                const auto date = result.GetValue(0, row);
                consumer({DateDays(result.GetValue(0, row)),
                          DecimalRaw(result.GetValue(1, row)), 2,
                          DecimalRaw(result.GetValue(2, row)), 2,
                          DecimalRaw(result.GetValue(3, row)), 2,
                          DecimalRaw(result.GetValue(4, row)), 2,
                          result.GetValue(5, row).ToString()[0],
                          result.GetValue(6, row).ToString()[0]});
              });
  }
  ::duckdb::Connection &connection;
};

struct TpchQ3Scan : TpchQ3InitialScan {
  explicit TpchQ3Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanCustomers(
      const std::function<void(const TpchQ3CustomerInput &)> &consumer) override {
    ScanQuery(connection, "SELECT c_custkey, c_mktsegment FROM customer",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({std::stoll(result.GetValue(0, row).ToString()),
                          result.GetValue(1, row).ToString()});
              });
  }
  void ScanOrders(
      const std::function<void(const TpchQ3OrderInput &)> &consumer) override {
    ScanQuery(connection,
              "SELECT o_orderkey, o_orderdate, o_custkey, o_shippriority "
              "FROM orders",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({std::stoll(result.GetValue(0, row).ToString()),
                          DateDays(result.GetValue(1, row)),
                          std::stoll(result.GetValue(2, row).ToString()),
                          static_cast<int32_t>(SafeInt32(result.GetValue(3, row)))});
              });
  }
  void ScanLineitems(
      const std::function<void(const TpchQ3LineitemInput &)> &consumer) override {
    ScanQuery(connection,
              "SELECT l_orderkey, l_extendedprice, l_discount, l_shipdate "
              "FROM lineitem",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({std::stoll(result.GetValue(0, row).ToString()),
                          DecimalRaw(result.GetValue(1, row)), 2,
                          DecimalRaw(result.GetValue(2, row)), 2,
                          DateDays(result.GetValue(3, row))});
              });
  }
  ::duckdb::Connection &connection;
};

struct TpchQ6Scan : TpchQ6InitialScan {
  explicit TpchQ6Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanLineitems(
      const std::function<void(const TpchQ6LineitemInput &)> &consumer) override {
    ScanQuery(connection,
              "SELECT l_shipdate, l_discount, l_quantity, l_extendedprice "
              "FROM lineitem",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({DateDays(result.GetValue(0, row)),
                          DecimalRaw(result.GetValue(1, row)), 2,
                          DecimalRaw(result.GetValue(2, row)), 2,
                          DecimalRaw(result.GetValue(3, row)), 2});
              });
  }
  ::duckdb::Connection &connection;
};

struct TpchQ10Scan : TpchQ10InitialScan {
  explicit TpchQ10Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanCustomers(
      const std::function<void(const TpchQ10CustomerInput &)> &consumer) override {
    ScanQuery(connection,
              "SELECT c_custkey, c_name, c_acctbal, c_nationkey, c_address, "
              "c_phone, c_comment FROM customer",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({std::stoll(result.GetValue(0, row).ToString()),
                          result.GetValue(1, row).ToString(),
                          DecimalRaw(result.GetValue(2, row)),
                          static_cast<int32_t>(SafeInt32(result.GetValue(3, row))),
                          result.GetValue(4, row).ToString(),
                          result.GetValue(5, row).ToString(),
                          result.GetValue(6, row).ToString()});
              });
  }
  void ScanNations(
      const std::function<void(const TpchQ10NationInput &)> &consumer) override {
    ScanQuery(connection, "SELECT n_nationkey, n_name FROM nation",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({                          static_cast<int32_t>(SafeInt32(result.GetValue(0, row))),
                          result.GetValue(1, row).ToString()});
              });
  }
  void ScanOrders(
      const std::function<void(const TpchQ10OrderInput &)> &consumer) override {
    ScanQuery(connection, "SELECT o_orderkey, o_custkey, o_orderdate FROM orders",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({std::stoll(result.GetValue(0, row).ToString()),
                          std::stoll(result.GetValue(1, row).ToString()),
                                                    DateDays(result.GetValue(2, row))});
              });
  }
  void ScanLineitems(
      const std::function<void(const TpchQ10LineitemInput &)> &consumer) override {
    ScanQuery(connection,
              "SELECT l_orderkey, l_extendedprice, l_discount, l_returnflag "
              "FROM lineitem",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({std::stoll(result.GetValue(0, row).ToString()),
                          DecimalRaw(result.GetValue(1, row)), 2,
                          DecimalRaw(result.GetValue(2, row)), 2,
                          result.GetValue(3, row).ToString() == "R"});
              });
  }
  ::duckdb::Connection &connection;
};

struct TpchQ12Scan : TpchQ12InitialScan {
  explicit TpchQ12Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanOrders(
      const std::function<void(const TpchQ12OrderInput &)> &consumer) override {
    ScanQuery(connection, "SELECT o_orderkey, o_orderpriority FROM orders",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({std::stoll(result.GetValue(0, row).ToString()),
                          result.GetValue(1, row).ToString()});
              });
  }
  void ScanLineitems(
      const std::function<void(const TpchQ12LineitemInput &)> &consumer) override {
    ScanQuery(connection,
              "SELECT l_orderkey, l_shipmode, l_commitdate, l_receiptdate, "
              "l_shipdate FROM lineitem",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({std::stoll(result.GetValue(0, row).ToString()),
                          result.GetValue(1, row).ToString(),
                                                    DateDays(result.GetValue(2, row)),
                          DateDays(result.GetValue(3, row)),
                          DateDays(result.GetValue(4, row))});
              });
  }
  ::duckdb::Connection &connection;
};

struct TpchQ14Scan : TpchQ14InitialScan {
  explicit TpchQ14Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanParts(
      const std::function<void(const TpchQ14PartInput &)> &consumer) override {
    ScanQuery(connection, "SELECT p_partkey, p_type FROM part",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({std::stoll(result.GetValue(0, row).ToString()),
                          result.GetValue(1, row).ToString()});
              });
  }
  void ScanLineitems(
      const std::function<void(const TpchQ14LineitemInput &)> &consumer) override {
    ScanQuery(connection,
              "SELECT l_partkey, l_extendedprice, l_discount, l_shipdate "
              "FROM lineitem",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({std::stoll(result.GetValue(0, row).ToString()),
                          DecimalRaw(result.GetValue(1, row)), 2,
                          DecimalRaw(result.GetValue(2, row)), 2,
                          DateDays(result.GetValue(3, row))});
              });
  }
  ::duckdb::Connection &connection;
};

struct SecEdgarQ1Scan : SecEdgarQ1InitialScan {
  explicit SecEdgarQ1Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanPre(
      const std::function<void(const SecEdgarQ1PreInput &)> &consumer) const override {
    ScanQuery(connection, "SELECT stmt, rfile, adsh, line FROM pre",
              [&](auto &result, ::duckdb::idx_t row) {
                const auto stmt = result.GetValue(0, row);
                const auto rfile = result.GetValue(1, row);
                consumer({stmt.IsNull() ? std::string() : stmt.ToString(),
                          rfile.IsNull() ? std::string() : rfile.ToString(),
                          rfile.IsNull(), stmt.IsNull(),
                          result.GetValue(2, row).ToString(),
                          static_cast<int32_t>(SafeInt32(result.GetValue(3, row)))});
              });
  }
  ::duckdb::Connection &connection;
};

struct SecEdgarQ2Scan : SecEdgarQ2InitialScan {
  explicit SecEdgarQ2Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanNum(
      const std::function<void(const SecEdgarQ2NumInput &)> &consumer) override {
    ScanQuery(connection, "SELECT adsh, tag, uom, value FROM num",
              [&](auto &result, ::duckdb::idx_t row) {
                const auto value = result.GetValue(3, row);
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          result.GetValue(2, row).ToString(),
                          value.IsNull() ? 0.0 : std::stod(value.ToString()),
                          value.IsNull()});
              });
  }
  void ScanSub(
      const std::function<void(const SecEdgarQ2SubInput &)> &consumer) override {
    ScanQuery(connection, "SELECT adsh, name, fy FROM sub",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          static_cast<int32_t>(SafeInt32(result.GetValue(2, row)))});
              });
  }
  ::duckdb::Connection &connection;
};

struct SecEdgarQ3Scan : SecEdgarQ3InitialScan {
  explicit SecEdgarQ3Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanNum(
      const std::function<void(const SecEdgarQ3NumInput &)> &consumer) override {
    ScanQuery(connection, "SELECT adsh, uom, value FROM num",
              [&](auto &result, ::duckdb::idx_t row) {
                const auto value = result.GetValue(2, row);
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          value.IsNull() ? 0.0 : std::stod(value.ToString()),
                          value.IsNull()});
              });
  }
  void ScanSub(
      const std::function<void(const SecEdgarQ3SubInput &)> &consumer) override {
    ScanQuery(connection, "SELECT adsh, name, cik, fy FROM sub",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          static_cast<int32_t>(SafeInt32(result.GetValue(2, row))),
                          static_cast<int32_t>(SafeInt32(result.GetValue(3, row)))});
              });
  }
  ::duckdb::Connection &connection;
};

struct SecEdgarQ4Scan : SecEdgarQ4InitialScan {
  explicit SecEdgarQ4Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanSub(
      const std::function<void(const SecEdgarQ4SubInput &)> &consumer) const override {
    ScanQuery(connection, "SELECT adsh, cik, sic FROM sub",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({result.GetValue(0, row).ToString(),
                          static_cast<int32_t>(SafeInt32(result.GetValue(1, row))),
                          static_cast<int32_t>(SafeInt32(result.GetValue(2, row)))});
              });
  }
  void ScanTag(
      const std::function<void(const SecEdgarQ4TagInput &)> &consumer) const override {
    ScanQuery(connection, "SELECT tag, version, abstract, tlabel FROM tag",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          static_cast<int32_t>(SafeInt32(result.GetValue(2, row))),
                          result.GetValue(3, row).ToString()});
              });
  }
  void ScanPre(
      const std::function<void(const SecEdgarQ4PreInput &)> &consumer) const override {
    ScanQuery(connection, "SELECT adsh, tag, version, stmt FROM pre",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          result.GetValue(2, row).ToString(),
                          result.GetValue(3, row).ToString()});
              });
  }
  void ScanNum(
      const std::function<void(const SecEdgarQ4NumInput &)> &consumer) const override {
    ScanQuery(connection, "SELECT adsh, tag, version, uom, value FROM num",
              [&](auto &result, ::duckdb::idx_t row) {
                const auto value = result.GetValue(4, row);
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          result.GetValue(2, row).ToString(),
                          result.GetValue(3, row).ToString(),
                          value.IsNull() ? 0.0 : std::stod(value.ToString()),
                          value.IsNull()});
              });
  }
  ::duckdb::Connection &connection;
};

struct SecEdgarQ6Scan : SecEdgarQ6InitialScan {
  explicit SecEdgarQ6Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanNum(
      const std::function<void(const SecEdgarQ6NumInput &)> &consumer) override {
    ScanQuery(connection, "SELECT adsh, tag, version, uom, value FROM num",
              [&](auto &result, ::duckdb::idx_t row) {
                const auto value = result.GetValue(4, row);
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          result.GetValue(2, row).ToString(),
                          result.GetValue(3, row).ToString(),
                          value.IsNull() ? 0.0 : std::stod(value.ToString()),
                          value.IsNull()});
              });
  }
  void ScanSub(
      const std::function<void(const SecEdgarQ6SubInput &)> &consumer) override {
    ScanQuery(connection, "SELECT adsh, name, fy FROM sub",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          static_cast<int32_t>(SafeInt32(result.GetValue(2, row)))});
              });
  }
  void ScanPre(
      const std::function<void(const SecEdgarQ6PreInput &)> &consumer) override {
    ScanQuery(connection,
              "SELECT adsh, tag, version, stmt, plabel FROM pre",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          result.GetValue(2, row).ToString(),
                          result.GetValue(3, row).ToString(),
                          result.GetValue(4, row).ToString()});
              });
  }
  ::duckdb::Connection &connection;
};

struct SecEdgarQ24Scan : SecEdgarQ24InitialScan {
  explicit SecEdgarQ24Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanNum(
      const std::function<void(const SecEdgarQ24NumInput &)> &consumer) override {
    ScanQuery(connection, "SELECT adsh, tag, version, uom, value, ddate FROM num",
              [&](auto &result, ::duckdb::idx_t row) {
                const auto value = result.GetValue(4, row);
                const auto date = result.GetValue(5, row);
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          result.GetValue(2, row).ToString(),
                          result.GetValue(3, row).ToString(),
                          value.IsNull() ? 0.0 : std::stod(value.ToString()),
                          value.IsNull(),
                          static_cast<int32_t>(SafeInt32(date))});
              });
  }
  void ScanPre(
      const std::function<void(const SecEdgarQ24PreInput &)> &consumer) override {
    ScanQuery(connection, "SELECT adsh, tag, version FROM pre",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          result.GetValue(2, row).ToString()});
              });
  }
  ::duckdb::Connection &connection;
};

template <typename Scan, typename QueryModule>
struct QueryBound : Scan, ModuleBinding<QueryModule> {
  template <typename... Args>
  QueryBound(::duckdb::Connection &connection, Args &&...args)
      : Scan(connection),
        ModuleBinding<QueryModule>(std::forward<Args>(args)...,
                                   static_cast<Scan *>(this)) {}
};

template <typename Scan, typename QueryModule, typename... ConfigArgs>
std::unique_ptr<DuckdbRuntime::Binding>
MakeBinding(::duckdb::Connection &connection, ConfigArgs &&...config_args) {
  return std::unique_ptr<DuckdbRuntime::Binding>(
      new QueryBound<Scan, QueryModule>(connection,
                                        std::forward<ConfigArgs>(config_args)...));
}

std::unique_ptr<DuckdbRuntime::Binding>
CreateBinding(::duckdb::Connection &connection,
              const SharedQueryCatalogEntry &entry) {
  switch (entry.query_id) {
  case 1:
    if (entry.workload == SharedWorkloadKind::Tpch) {
      return MakeBinding<TpchQ1Scan, TpchQ1Module>(connection, TpchQ1Config{1});
    }
    return MakeBinding<SecEdgarQ1Scan, SecEdgarQ1Module>(connection,
                                                         SecEdgarQ1Config{1});
  case 2:
    return MakeBinding<SecEdgarQ2Scan, SecEdgarQ2Module>(
        connection, SecEdgarQ2Config{1, 2});
  case 3:
    if (entry.workload == SharedWorkloadKind::Tpch) {
      return MakeBinding<TpchQ3Scan, TpchQ3Module>(connection,
                                                  TpchQ3Config{1, 2, 3});
    }
    return MakeBinding<SecEdgarQ3Scan, SecEdgarQ3Module>(
        connection, SecEdgarQ3Config{1, 2});
  case 4:
    return MakeBinding<SecEdgarQ4Scan, SecEdgarQ4Module>(
        connection, SecEdgarQ4Config{1, 2, 3, 4});
  case 6:
    if (entry.workload == SharedWorkloadKind::Tpch) {
      return MakeBinding<TpchQ6Scan, TpchQ6Module>(connection, TpchQ6Config{1});
    }
    return MakeBinding<SecEdgarQ6Scan, SecEdgarQ6Module>(
        connection, SecEdgarQ6Config{1, 2, 3});
  case 10:
    return MakeBinding<TpchQ10Scan, TpchQ10Module>(
        connection, TpchQ10Config{1, 2, 3, 4});
  case 12:
    return MakeBinding<TpchQ12Scan, TpchQ12Module>(connection,
                                                    TpchQ12Config{1, 2});
  case 14:
    return MakeBinding<TpchQ14Scan, TpchQ14Module>(connection,
                                                   TpchQ14Config{1, 2});
  case 24:
    return MakeBinding<SecEdgarQ24Scan, SecEdgarQ24Module>(
        connection, SecEdgarQ24Config{101, 102});
  default:
    throw std::runtime_error("unsupported shared query binding");
  }
}

} // namespace

DuckdbRuntime::DuckdbRuntime(::duckdb::Connection &connection,
                             const SharedQueryCatalogEntry &entry,
                             double expected_queries_before_write)
    : connection_(connection), entry_(entry),
      state_store_(entry.accelerator_id, legacy_store_), coordinator_(),
      backend_(coordinator_, state_store_,
               [](const ::duckdb::TableMutationEvent &) {
                 return core::MutationBatch{};
               },
               [] { return core::VisibilityToken{}; },
               [entry] {
                 return core::LoadAuxCostModelJson(
                     harness::SharedCostModelConfigPath(entry.workload,
                                                        entry.query_id));
               },
               [expected_queries_before_write] {
                 return expected_queries_before_write;
               }),
      binding_(CreateBinding(connection, entry)) {
  coordinator_.Register(std::shared_ptr<core::AuxQueryModule>(
      &binding_->Module(), [](core::AuxQueryModule *) {}));
}

void DuckdbRuntime::Build() {
  Builder builder;
  binding_->Build(backend_, builder);
  Require(builder.finished, "shared DuckDB build did not finish");
  legacy_store_.InvalidateAll();
  for (const auto &image : builder.images) {
    legacy_store_.PublishPartition(image.partition, image.payload, 0);
  }
}

std::vector<core::AuxRow> DuckdbRuntime::ExecuteCollect() {
  return ExecuteCollect(ToCoreArguments(entry_.benchmark_arguments));
}

std::vector<core::AuxRow> DuckdbRuntime::ExecuteCollect(
    const core::QueryArguments &arguments) {
  MaterializeSink sink;
  binding_->Execute(backend_, arguments, sink);
  last_rows = sink.rows.size();
  return std::move(sink.rows);
}

void DuckdbRuntime::Execute() {
  auto rows = ExecuteCollect();
  last_rows = rows.size();
}

void DuckdbRuntime::Apply(SharedProbe &probe) {
  connection_.BeginTransaction();
  auto result = connection_.Query(probe.update_sql);
  Require(result != nullptr && !result->HasError(),
          result ? result->GetError() : "DuckDB write failed");
  backend_.CaptureMutation(BuildSharedMutation(entry_, probe));
  coordinator_.PreCommit();
  connection_.Commit();
  coordinator_.Commit();
  probe.current_value += probe.write_delta;
}

void DuckdbRuntime::ResetCalibrationState() {
  legacy_store_.InvalidateAll();
  coordinator_.Abort();
}

void DuckdbRuntime::InvalidateAll() { legacy_store_.InvalidateAll(); }

void DuckdbRuntime::ApplyMutationBatch(const core::MutationBatch &mutation) {
  backend_.CaptureMutation(mutation);
  coordinator_.PreCommit();
  coordinator_.Commit();
}

void DuckdbRuntime::ExecuteBaseQuery() {
  auto result = connection_.Query(entry_.base_sql);
  Require(result != nullptr && !result->HasError(),
          result ? result->GetError() : "DuckDB base query failed");
  for (::duckdb::idx_t row = 0; row < result->RowCount(); ++row) {
    for (::duckdb::idx_t column = 0; column < result->ColumnCount(); ++column) {
      (void)result->GetValue(column, row);
    }
  }
}

DuckdbRuntime::~DuckdbRuntime() = default;

} // namespace accelerator_codegen::aux_state::engine::duckdb
