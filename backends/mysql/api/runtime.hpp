// MySQL engine backend: the public interface that per-query code plugs into.
//
// The backend (this header, runtime.cpp, mysql_plugin.cpp) names NO query.
// Everything query-specific lives under accelerators/<bench>/q<N>/mysql/ and
// reaches the backend through registries with static registrars:
//
//   * UDF registry (this header): accelerators/<bench>/q<N>/mysql/plugin/runtime.cpp
//     defines the query's scans, Runtime<> instance and entry point and registers
//     it with a file-scope MysqlUdfRegistrar.  The same dir's udf.cpp exports the
//     UDF symbols the MySQL server resolves by name (ACCEL_MYSQL_QUERY_UDF in
//     udf_shim.hpp), and functions.sql holds its CREATE FUNCTION statement
//     (scripts/mysql_register_functions.sh assembles the fragments).
//   * Query catalog (catalog/mysql/query_catalog.hpp): q<N>/mysql/aux/catalog.cpp
//     registers the query's catalog entry (MysqlQueryCatalogRegistrar).
//
// Registries are plain functions with function-local static storage in
// runtime.cpp / query_catalog.cpp; there are no header-only singletons. The
// state itself (capture, commits, builds, reads' epochs) is the runtime
// skeleton's, hosted by MysqlHost (mysql_host.hpp).
//
// Engine-level helpers (ScanTypedTuples, Typed*, ByteExactLiteral,
// KeyListPredicate, ColumnsCompareBytewise, Runtime<>) stay in the backend and
// are exported in namespace detail below for per-query code.
#pragma once

#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "substrate/aux/core/aux_calibration.hpp"
#include "substrate/aux/core/aux_cost_model_path.hpp"
#include "substrate/aux/core/aux_execute_stats.hpp"
#include "substrate/aux/core/aux_types.hpp"
#include "substrate/aux/core/parallel_slices.hpp"
#include "backends/mysql/api/mysql_host.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {

// ---- UDF registry ---------------------------------------------------------
struct MysqlUdfEntry {
  // SQL function name; the UDF symbols are <name>, <name>_init, <name>_deinit.
  std::string name;
  // One char per argument: 'i' int (int64), 'f' double (nearest double of the
  // argument's decimal text), 'n' exact decimal (ScaledDecimal: 0.065 is
  // {65, 3}, never a double), 'd' date, anything else string.  A NULL
  // argument arrives as an AuxValue with is_null set (of the declared type);
  // a malformed int/decimal/double/date argument is a UDF error (SQL NULL
  // result), never a default value.
  std::string arg_types;
  // Runs the query; returns '|'-separated rows, one per line.
  std::string (*execute)(const core::QueryArguments &) = nullptr;
  // A NULL first argument yields an empty result instead of running the query.
  bool null_first_arg_yields_empty = false;
};

// Registers (replacing any entry of the same name).  Entries are held stably.
void RegisterMysqlUdf(MysqlUdfEntry entry);
// nullptr if no query registered under `name`.
const MysqlUdfEntry *FindMysqlUdf(const std::string &name);
// Registered UDF names, sorted.
std::vector<std::string> RegisteredMysqlUdfNames();
// Runs the registered UDF `name` (throws std::runtime_error if unknown).
std::string ExecuteMysqlUdf(const std::string &name, const core::QueryArguments &arguments);

struct MysqlUdfRegistrar {
  explicit MysqlUdfRegistrar(MysqlUdfEntry entry) { RegisterMysqlUdf(std::move(entry)); }
};

namespace detail {

namespace core = accelerator_codegen::aux_state::core;

// Table scans this thread ran (a query that scans answered from SQL).
extern thread_local uint64_t t_table_scans;

// The query's cost model: `env_name` when set, else the first of
// <stem>_mysql.json and <stem>.json found in the query's directory
// (relative to the working directory), then in the source checkout mounted
// in Docker (/accel-src/<dir>); "" when none.
std::string ResolveCostModelPath(const char *env_name, const core::AuxCostModelLocation &location);

// The database the scan connections use: ACCEL_MYSQL_DB / MYSQL_DATABASE from
// the server's environment, or this override (mysql_accel_set_scan_database).
// Aux states already built are not affected: invalidate them to rebuild.
void SetScanDatabase(const std::string &db);
// "cli": scan through the mysql CLI (no snapshot builds); "client": in-process
// C API when available; anything else: decide from ACCEL_MYSQL_SCAN / availability.
void SetScanMode(const std::string &mode);
// Strict 'YYYY-MM-DD' -> Unix-epoch days; throws std::runtime_error if malformed.
int32_t ParseUnixDays(const std::string &text);
// `mysql` CLI command line (connection options only) for the server's scan
// connection settings; append further options and the statement.
std::string MysqlCliBase();
std::string MysqlCliPrefix();
std::string ShellEscape(const std::string &s);
std::vector<std::string> SplitTsv(const std::string &line);

// Streams the result of `sql` and calls `callback` once per row with the row's
// columns as text.  In-process over the MySQL C API (mysql_use_result, rows
// are not buffered) when the plugin is built with the client library
// (ACCEL_MYSQL_HAVE_CLIENT), else through `mysql --quick`.  The vector passed
// to the callback is reused for the next row (copy what you keep).  Inside a
// SnapshotScanSession on this thread every scan runs on the session's
// connection (one consistent read view); otherwise each call uses its own
// connection (autocommit, sees committed data at the time of the call).
void ScanTypedTuples(const std::string &sql,
                     const std::function<void(const std::vector<std::string> &)> &callback);

// A consistent read view for a set of scans: opens a connection with
// START TRANSACTION WITH CONSISTENT SNAPSHOT (REPEATABLE READ, read only) and
// routes this thread's ScanTypedTuples through it until destroyed (the
// connection is closed, which ends the view; no COMMIT/ROLLBACK statement is
// issued because the audit plugin reads those as transaction events).
// Throws std::runtime_error if the connection or snapshot fails and
// std::logic_error if !SnapshotScansSupported() or one is already open.
class SnapshotScanSession {
public:
  // `lanes` connections, each holding the same view, for ScanTypedTuplesPartitioned
  // (the thread's own scans use the first). The host opens them while no
  // writer can commit, which is what makes the views equal.
  explicit SnapshotScanSession(std::size_t lanes = 1);
  ~SnapshotScanSession();
  SnapshotScanSession(const SnapshotScanSession &) = delete;
  SnapshotScanSession &operator=(const SnapshotScanSession &) = delete;
  void Close();
};
// False when built without the client library (CLI scans cannot hold a view).
bool SnapshotScansSupported();
// The connections of this thread's SnapshotScanSession (none outside one), and
// a scan on one of them (from any thread, one at a time per lane).
struct SnapshotLane;
std::vector<SnapshotLane *> SnapshotLanes();
void ScanOnSnapshotLane(SnapshotLane &lane, const std::string &sql,
                        const std::function<void(const std::vector<std::string> &)> &callback);

// A table cut into partitions by the value of an integer column.
struct SplitTable {
  std::string select;       // column list
  std::string table;
  std::string where;        // extra filter; empty for none
  std::string split_column; // integer column the partitions are ranges of
  std::string group_by;     // GROUP BY list; empty for none. Each partition then
                            // groups its own range: a group may come back once per
                            // partition, and the caller sums the partial groups.
  std::string bounds_from;  // FROM of the MIN/MAX of split_column; empty: `table`.
                            // For a join, name the table the column lives in.
};
// `SELECT <select> FROM <table> [WHERE (<where>)] [GROUP BY <group_by>]`: the
// whole table, as the serial scan reads it.
std::string SelectSql(const SplitTable &table);
using PartitionedRowSink = std::function<void(std::size_t, const std::vector<std::string> &)>;
// The partitioned scan of a build: sink(partition, row) for every row of
// `table`, with the same row (and Typed* accessors) as ScanTypedTuples gives.
//  - the rows of a partition are a fixed set (a range of the split column,
//    from MIN/MAX in the build's view), delivered in one statement's order;
//  - sinks of one partition never overlap; partitions run concurrently, one
//    per snapshot lane, on pool threads, so the sink must not touch the engine;
//  - one partition is ScanTypedTuples.
// Without a session of this thread's own the partitions run one after the
// other in a view of their own.
void ScanTypedTuplesPartitioned(const core::AuxEngineServices &services, const SplitTable &table,
                                std::size_t partitions, const PartitionedRowSink &sink);

const std::string &Col(const std::vector<std::string> &cols, int attribute);
// Whether the column of the row being delivered by ScanTypedTuples is SQL NULL.
bool TypedIsNull(const std::vector<std::string> &cols, int attribute);

// Makes `row` (with `nulls`: which cells are SQL NULL) the row the Typed*
// accessors read on this thread, until destroyed.
class RowScope {
public:
  RowScope(const std::vector<std::string> *row, const std::vector<char> *nulls);
  ~RowScope();
  RowScope(const RowScope &) = delete;
  RowScope &operator=(const RowScope &) = delete;

private:
  const std::vector<std::string> *prev_row;
  const std::vector<char> *prev_nulls;
};

// The services of the module call running on this thread; scans decode under
// their thread limit.
class ScanServicesScope {
public:
  explicit ScanServicesScope(const core::AuxEngineServices &services);
  ~ScanServicesScope();
  ScanServicesScope(const ScanServicesScope &) = delete;
  ScanServicesScope &operator=(const ScanServicesScope &) = delete;

private:
  const core::AuxEngineServices *saved;
};
const core::AuxEngineServices *CurrentScanServices();

// ScanTypedTuples that decodes rows in parallel: decode(cols) -> Input runs on
// pool threads over a batch of rows, consume(Input &&) on this thread in row
// order, so the module sees what the serial scan gives it. The slices never
// touch the connection. With one worker it is ScanTypedTuples.
template <typename Decode, typename Consume>
void ScanTypedTuplesDecoded(const std::string &sql, Decode &&decode, Consume &&consume) {
  using Input = decltype(decode(std::declval<const std::vector<std::string> &>()));
  const auto *services = CurrentScanServices();
  if (!services || services->WorkerCount() <= 1) {
    ScanTypedTuples(sql, [&](const std::vector<std::string> &cols) { consume(decode(cols)); });
    return;
  }
  constexpr std::size_t kBatchRows = 16384;
  constexpr std::size_t kSliceRows = 1024;
  struct Row {
    std::vector<std::string> cols;
    std::vector<char> nulls;
  };
  std::vector<Row> rows;
  std::size_t filled = 0;
  const auto flush = [&] {
    const std::size_t slices = core::SliceCount(*services, filled, kSliceRows);
    std::vector<std::vector<Input>> decoded(slices);
    core::RunSlices(*services, slices, filled, [&](std::size_t slice, std::size_t begin, std::size_t end) {
      decoded[slice].reserve(end - begin);
      for (std::size_t i = begin; i < end; ++i) {
        const RowScope scope(&rows[i].cols, &rows[i].nulls);
        decoded[slice].push_back(decode(rows[i].cols));
      }
    });
    for (auto &slice : decoded) {
      for (auto &input : slice) {
        consume(std::move(input));
      }
    }
    filled = 0;
  };
  ScanTypedTuples(sql, [&](const std::vector<std::string> &cols) {
    if (filled == rows.size()) {
      rows.emplace_back();
    }
    Row &row = rows[filled++];
    row.cols = cols;
    row.nulls.resize(cols.size());
    for (std::size_t i = 0; i < cols.size(); ++i) {
      row.nulls[i] = TypedIsNull(cols, static_cast<int>(i));
    }
    if (filled == kBatchRows) {
      flush();
    }
  });
  flush();
}

// The typed accessors parse the text exactly and throw std::runtime_error on
// text they cannot represent (malformed, out of range, needs rounding); a NULL
// cell reads as 0 / "" (see TypedIsNull).
int64_t TypedInt(const std::vector<std::string> &cols, int attribute);
int32_t TypedDate(const std::vector<std::string> &cols, int attribute);
std::string TypedText(const std::vector<std::string> &cols, int attribute);
// The decimal exactly as stored, at its own scale ("0.065" -> {65, 3}).
core::ScaledDecimal TypedScaledDecimal(const std::vector<std::string> &cols, int attribute);
// The decimal as an integer at `scale` (value * 10^scale); throws unless that
// is exact (no digit is ever rounded away).
int64_t TypedDecimal(const std::vector<std::string> &cols, int attribute, int scale);
// TypedDecimal(cols, attribute, 2): the raw value at scale 2 (the contract
// existing callers rely on).
int64_t TypedDecimalRaw(const std::vector<std::string> &cols, int attribute);
char TypedChar(const std::vector<std::string> &cols, int attribute);
// A text value as an SQL literal that reaches the server byte for byte.
std::string ByteExactLiteral(const std::string &text);
// `column IN (k1, k2, ...)`, or FALSE for no keys.
std::string KeyListPredicate(const char *column, const std::vector<int64_t> &keys);
// Whether every (table, column) compares text byte by byte (utf8mb4_0900_bin).
bool ColumnsCompareBytewise(const std::vector<std::pair<std::string, std::string>> &columns);

class TsvSink final : public core::AuxRowSink {
public:
  explicit TsvSink(std::string &out_p) : out(out_p) {}

  void Begin(const core::OutputSchema &) override {}

  void WriteRow(const core::AuxRow &row) override {
    for (std::size_t column = 0; column < row.values.size(); ++column) {
      if (column > 0) {
        out.push_back('|');
      }
      // NULL as the mysql client prints it (an all-NULL row stays a row).
      out += row.values[column].is_null ? std::string("NULL") : CellText(row.values[column]);
    }
    out.push_back('\n');
  }

  void End() override {}

private:
  static std::string DecimalText(const core::ScaledDecimal &decimal) {
    return core::ScaledDecimal128ToString(decimal.value, decimal.scale);
  }

  static std::string FormatUnixDate(int32_t days) {
    int z = days + 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = static_cast<int>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", y, m, d);
    return buf;
  }

  static std::string CellText(const core::AuxValue &value) {
    if (std::holds_alternative<int64_t>(value.payload)) {
      return std::to_string(std::get<int64_t>(value.payload));
    }
    if (std::holds_alternative<uint64_t>(value.payload)) {
      return std::to_string(std::get<uint64_t>(value.payload));
    }
    if (std::holds_alternative<double>(value.payload)) {
      std::ostringstream ss;
      ss.setf(std::ios::fixed);
      ss.precision(6);
      ss << std::get<double>(value.payload);
      return ss.str();
    }
    if (std::holds_alternative<core::ScaledDecimal>(value.payload)) {
      return DecimalText(std::get<core::ScaledDecimal>(value.payload));
    }
    if (const auto *wide = std::get_if<core::ScaledDecimal128>(&value.payload)) {
      return core::ScaledDecimal128ToString(wide->value, wide->scale);
    }
    if (std::holds_alternative<core::AuxDate>(value.payload)) {
      return FormatUnixDate(std::get<core::AuxDate>(value.payload).days_since_epoch);
    }
    if (std::holds_alternative<std::string>(value.payload)) {
      return std::get<std::string>(value.payload);
    }
    if (std::holds_alternative<bool>(value.payload)) {
      return std::get<bool>(value.payload) ? "t" : "f";
    }
    // Every payload alternative is handled above; fail loudly if one is added.
    throw std::logic_error("unprintable AuxValue payload");
  }

  std::string &out;
};

// How long a reader waits for an in-flight commit or a build before it
// answers from SQL (ACCEL_AUX_READ_WAIT_US).
std::chrono::microseconds ReadWait();

// One query's accelerator as a client of the server's MysqlHost: the host
// builds and maintains its state; this runs the query against it.
template <typename Module, typename Config, typename ScanT>
class Runtime final {
public:
  Runtime(const char *cost_model_env, core::AuxCostModelLocation cost_model,
          core::AcceleratorId accelerator_id, Config config)
      : scan(), module(config, &scan),
        accelerator(MysqlHost::Get().Register(
            std::shared_ptr<Module>(&module, [](Module *) {}),
            core::LoadAuxCostModelJson(ResolveCostModelPath(cost_model_env, cost_model)))) {
    if (module.Id() != accelerator_id) {
      throw std::logic_error("accelerator id mismatch: " + accelerator_id);
    }
  }

  void Execute(const core::QueryArguments &arguments, std::string &out) {
    MysqlHost::Get().NoteRead(accelerator);
    TsvSink sink(out);
    runtime::ReadOptions options;
    options.max_wait_us = static_cast<uint64_t>(ReadWait().count());
    auto guard = accelerator.BeginRead(options);
    const ScanServicesScope scans(accelerator);
    t_table_scans = 0;
    if (guard.Servable()) {
      module.Execute(accelerator, arguments, sink);
    } else {
      module.ExecuteFallback(accelerator, arguments, sink);
    }
    core::RecordAuxExecute(!guard.Servable() || t_table_scans > 0 ? core::AuxExecutePath::Fallback
                                                                  : core::AuxExecutePath::Hit);
  }

private:
  ScanT scan;
  Module module;
  runtime::AcceleratorRuntime &accelerator;
};

} // namespace detail
} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
