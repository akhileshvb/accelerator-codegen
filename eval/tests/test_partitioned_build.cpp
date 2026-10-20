// Parallel build framework: core::PartitionedBuild over the DuckDB
// partitioned typed scan (backends/duckdb/api/build_scan.hpp).
//
//  1. Partition bounds (DuckDB row groups, Postgres heap blocks) are
//     deterministic, contiguous and cover every row.
//  2. A toy module (counts, integer and decimal sums, an exactly summable
//     double, a text hash, a map) built at P = 1, 4, 16 has identical state,
//     equal to SQL's answer; an inexact double sum repeats bit for bit at a
//     fixed P.
//  3. Builds read the build snapshot, and fallback scans the reader's: rows
//     committed or deleted by another connection after it are not seen at any P.
//
// With a database path argument it also times a lineitem build (7 columns)
// at P = 1, 8, 16 (read-only use of the database).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "backends/duckdb/api/build_scan.hpp"
#include "backends/postgres/api/base/pg_base.hpp"
#include "eval/tests/test_support/toy_build_state.hpp"
#include "substrate/aux/core/partitioned_build.hpp"
#include "substrate/aux/runtime/thread_budget.hpp"

using namespace accelerator_codegen::aux_state;
namespace engine = accelerator_codegen::aux_state::engine;
namespace scan = accelerator_codegen::aux_state::engine::duckdb::scan;
using accelerator_codegen::aux_state::engine::duckdb::DuckDBAuxBackend;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " +     \
                               #cond);                                                             \
    }                                                                                              \
  } while (0)

namespace {

// The build phase of a thread pool: WorkerCount() partitions, run on the pool.
class BuildServices final : public core::AuxEngineServices {
public:
  explicit BuildServices(std::size_t workers_p) : workers(workers_p), pool(workers_p) {}
  core::VisibilityToken CurrentVisibility() const override { throw std::logic_error("unused"); }
  core::AuxStateStore &StateStore() override { throw std::logic_error("unused"); }
  std::size_t WorkerCount() const override { return workers; }
  void ParallelFor(std::size_t n, const std::function<void(std::size_t)> &fn) const override {
    pool.ParallelFor(n, fn, workers);
  }

private:
  std::size_t workers;
  mutable runtime::ThreadBudget pool;
};

using test::ToyState;

ToyState BuildToy(::duckdb::Connection &connection, std::size_t partitions, std::size_t threads = 0) {
  BuildServices services(threads ? threads : partitions);
  const std::vector<scan::BuildColumn> columns = {scan::IntColumn("k"), scan::DateColumn("d"),
                                                  scan::DecimalColumn("qty", 2), scan::TextColumn("flag"),
                                                  scan::TextColumn("name")};
  return core::PartitionedBuild<ToyState>(
      services, [] { return ToyState{}; },
      [&](std::size_t parts, auto &&sink) {
        scan::ScanPartitioned(services, connection, "toy", columns, parts, sink);
      },
      [](ToyState &state, const scan::BuildRow &row) { state.Add(row); },
      [](ToyState &into, ToyState &&from) { into.Merge(std::move(from)); }, partitions);
}

// ---- tests ----

void PartitionBoundsAreDeterministic() {
  for (const uint64_t rows : {0ull, 1ull, 122879ull, 122880ull, 122881ull, 5'000'000ull, 600'000'000ull}) {
    for (const std::size_t partitions : {1, 2, 3, 8, 16, 40, 1000}) {
      const auto bounds = scan::RowGroupBounds(rows, partitions);
      CHECK(bounds == scan::RowGroupBounds(rows, partitions));
      CHECK(bounds.size() == partitions + 1 && bounds.front() == 0);
      CHECK(std::is_sorted(bounds.begin(), bounds.end()));
      CHECK(bounds.back() >= rows && bounds.back() < rows + 122880);
      for (const auto bound : bounds) {
        CHECK(bound % 122880 == 0);
      }
    }
  }
}

void PostgresBlockRangesAreDeterministic() {
  using accelerator_codegen::pg_base::PartitionBlocks;
  for (const uint32_t blocks : {0u, 1u, 7u, 112592u, 4'000'000u, 4'294'967'295u}) {
    for (const std::size_t partitions : {1, 2, 3, 8, 16, 100}) {
      uint32_t next = 0;
      for (std::size_t p = 0; p < partitions; ++p) {
        const auto range = PartitionBlocks(blocks, partitions, p);
        CHECK(range.first == next && range.first == PartitionBlocks(blocks, partitions, p).first);
        next = range.first + range.count;
      }
      CHECK(next == blocks);
    }
  }
}

void MakeToyTable(::duckdb::Connection &connection, int64_t rows) {
  const auto result = connection.Query(
      "CREATE TABLE toy AS SELECT i::BIGINT AS k, DATE '1992-01-01' + (i % 2500)::INTEGER AS d, "
      "CAST(CAST((i * 7919) % 1000003 AS VARCHAR) || '.' || lpad(CAST(i % 100 AS VARCHAR), 2, '0') AS "
      "DECIMAL(15,2)) AS qty, CASE i % 3 WHEN 0 THEN 'A' WHEN 1 THEN 'N' ELSE 'R' END AS flag, "
      "'name#' || CAST(i AS VARCHAR) AS name FROM range(" + std::to_string(rows) + ") t(i)");
  CHECK(!result->HasError());
}

ToyState SqlAnswer(::duckdb::Connection &connection) {
  const auto result = connection.Query(
      "SELECT count(*), sum(k)::BIGINT, (sum(qty) * 100)::BIGINT, max(d) - DATE '1970-01-01' FROM toy");
  CHECK(!result->HasError());
  ToyState p;
  p.rows = result->GetValue(0, 0).GetValue<int64_t>();
  p.key_sum = result->GetValue(1, 0).GetValue<int64_t>();
  p.qty_raw = result->GetValue(2, 0).GetValue<int64_t>();
  p.date_max = result->GetValue(3, 0).GetValue<int64_t>();
  return p;
}

void ToyModuleIsIdenticalAtAnyPartitionCount() {
  ::duckdb::DuckDB db(nullptr);
  ::duckdb::Connection connection(db);
  MakeToyTable(connection, 3'000'000);
  const ToyState truth = SqlAnswer(connection);
  const ToyState serial = BuildToy(connection, 1);
  CHECK(serial.rows == truth.rows && serial.key_sum == truth.key_sum && serial.qty_raw == truth.qty_raw &&
        serial.date_max == truth.date_max);
  CHECK(serial.exact_sum == static_cast<double>(truth.qty_raw) * 0.25);
  for (const std::size_t partitions : {2, 4, 16, 40}) {
    CHECK(BuildToy(connection, partitions) == serial);
  }
  // More partitions than row groups leaves some empty.
  CHECK(BuildToy(connection, 200) == serial);
  // A fixed partition count is reproducible bit for bit, at any thread count.
  const ToyState a = BuildToy(connection, 16, 2), b = BuildToy(connection, 16, 16);
  CHECK(a == b && std::memcmp(&a.inexact_sum, &b.inexact_sum, sizeof(double)) == 0);
}

// A fallback scans under the reader's ScanRedirect: every partition reads the
// reader's snapshot, not rows another connection committed after it.
void FallbackScansReadTheReadersSnapshot() {
  ::duckdb::DuckDB db(nullptr);
  ::duckdb::Connection reader(db), writer(db);
  MakeToyTable(writer, 1'000'000);
  const ToyState before = BuildToy(writer, 1);
  reader.BeginTransaction();
  CHECK(!reader.Query("SELECT count(*) FROM toy")->HasError());
  const auto snapshot = static_cast<::duckdb::transaction_t>(DuckDBAuxBackend::SnapshotEpoch(*reader.context));
  CHECK(!writer.Query("DELETE FROM toy WHERE k % 7 = 0")->HasError());
  {
    std::atomic<uint64_t> opened{0};
    engine::duckdb::ScanRedirect redirect(*db.instance, opened, snapshot);
    for (const std::size_t partitions : {1, 4, 16}) {
      CHECK(BuildToy(writer, partitions) == before);
    }
  }
  reader.Rollback();
}

// A nullable double column: the NULLs are counted exactly, and the inexact
// double sum is bit-identical at any thread count with the fixed partition count.
struct NullableDoubleState {
  int64_t rows = 0, nulls = 0;
  double sum = 0.0;
  void Merge(NullableDoubleState &&from) {
    rows += from.rows;
    nulls += from.nulls;
    sum += from.sum;
  }
};

NullableDoubleState BuildNullableDoubles(::duckdb::Connection &connection, std::size_t threads) {
  BuildServices services(threads);
  return core::PartitionedBuild<NullableDoubleState>(
      services, [] { return NullableDoubleState{}; },
      [&](std::size_t parts, auto &&sink) {
        scan::ScanPartitioned(services, connection, "doubles",
                              {scan::Nullable(scan::DoubleColumn("x"))},
                              parts, sink);
      },
      [](NullableDoubleState &state, const scan::BuildRow &row) {
        state.rows++;
        if (row.IsNull(0)) {
          state.nulls++;
        } else {
          state.sum += row.Double(0);
        }
      },
      [](NullableDoubleState &into, NullableDoubleState &&from) { into.Merge(std::move(from)); },
      core::kFloatBuildPartitions);
}

void NullableDoubleSumIsBitIdenticalAtAnyThreadCount() {
  ::duckdb::DuckDB db(nullptr);
  ::duckdb::Connection connection(db);
  CHECK(!connection
             .Query("CREATE TABLE doubles AS SELECT CASE WHEN i % 7 = 0 THEN NULL ELSE sin(i) * 1e6 / (1 + i % 13) END "
                    "AS x, CASE WHEN i % 5 = 0 THEN NULL ELSE 's' || i::VARCHAR END AS s "
                    "FROM range(3000000) t(i)")
             ->HasError());
  const auto nulls = connection.Query("SELECT count(*) FROM doubles WHERE x IS NULL")->GetValue(0, 0).GetValue<int64_t>();
  const NullableDoubleState one = BuildNullableDoubles(connection, 1);
  CHECK(one.rows == 3'000'000 && one.nulls == nulls);
  for (const std::size_t threads : {4, 16}) {
    const NullableDoubleState other = BuildNullableDoubles(connection, threads);
    CHECK(other.nulls == one.nulls && std::memcmp(&other.sum, &one.sum, sizeof(double)) == 0);
  }
}

void BuildsReadTheirSnapshot() {
  ::duckdb::DuckDB db(nullptr);
  ::duckdb::Connection build(db), writer(db);
  MakeToyTable(build, 1'000'000);
  build.BeginTransaction();
  const auto epoch = DuckDBAuxBackend::SnapshotEpoch(*build.context);
  CHECK(epoch != 0);
  CHECK(!writer.Query("INSERT INTO toy SELECT * FROM toy WHERE k < 300000")->HasError());
  CHECK(!writer.Query("DELETE FROM toy WHERE k >= 900000")->HasError());
  CHECK(DuckDBAuxBackend::SnapshotEpoch(*build.context) == epoch);
  const ToyState serial = BuildToy(build, 1);
  CHECK(serial.rows == 1'000'000);
  for (const std::size_t partitions : {4, 16}) {
    CHECK(BuildToy(build, partitions) == serial);
  }
  build.Rollback();
  CHECK(BuildToy(build, 1).rows == 1'000'000 + 300'000 - 100'000);
  CHECK(BuildToy(build, 16) == BuildToy(build, 1));
}

// ---- measurement on lineitem ----

struct LineitemState {
  int64_t rows = 0, qty_raw = 0, price_raw = 0;
  std::vector<int64_t> price_by_part = std::vector<int64_t>(1 << 20);
  std::map<char, int64_t> flags;
  std::unordered_map<int32_t, int64_t> rows_by_date;

  void Add(int64_t part, int64_t qty, int64_t price, int32_t day, char flag) {
    rows++;
    qty_raw += qty;
    price_raw += price;
    price_by_part[part & ((1 << 20) - 1)] += price;
    flags[flag]++;
    rows_by_date[day]++;
  }
  void Merge(LineitemState &&from) {
    rows += from.rows;
    qty_raw += from.qty_raw;
    price_raw += from.price_raw;
    for (std::size_t i = 0; i < price_by_part.size(); ++i) {
      price_by_part[i] += from.price_by_part[i];
    }
    for (const auto &[flag, count] : from.flags) {
      flags[flag] += count;
    }
    for (const auto &[day, count] : from.rows_by_date) {
      rows_by_date[day] += count;
    }
  }
  bool operator==(const LineitemState &o) const {
    return rows == o.rows && qty_raw == o.qty_raw && price_raw == o.price_raw &&
           price_by_part == o.price_by_part && flags == o.flags && rows_by_date == o.rows_by_date;
  }
};

constexpr const char *kLineitemSql =
    "SELECT l_partkey, l_quantity, l_extendedprice, l_shipdate, l_returnflag FROM lineitem";

// Today's build: parallel decode of boxed values, one serial consumer.
LineitemState BuildLineitemBoxed(::duckdb::Connection &connection, std::size_t workers) {
  BuildServices services(workers);
  engine::duckdb::scan::ScanServicesScope scope(services);
  LineitemState state;
  struct Input {
    int64_t part, qty, price;
    int32_t day;
    char flag;
  };
  engine::duckdb::scan::ScanQueryDecoded(
      connection, kLineitemSql,
      [](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
        using namespace engine::duckdb::scan;
        return Input{SafeInt64(ChunkValue(chunk, 0, row)), DecimalRaw(ChunkValue(chunk, 1, row)),
                     DecimalRaw(ChunkValue(chunk, 2, row)), DateDays(ChunkValue(chunk, 3, row)),
                     FirstChar(ChunkValue(chunk, 4, row))};
      },
      [&](const Input &in) { state.Add(in.part, in.qty, in.price, in.day, in.flag); });
  return state;
}

LineitemState BuildLineitemPartitioned(::duckdb::Connection &connection, std::size_t partitions) {
  BuildServices services(partitions);
  const std::vector<scan::BuildColumn> columns = {scan::IntColumn("l_partkey"), scan::DecimalColumn("l_quantity", 2),
                                                  scan::DecimalColumn("l_extendedprice", 2),
                                                  scan::DateColumn("l_shipdate"), scan::TextColumn("l_returnflag")};
  return core::PartitionedBuild<LineitemState>(
      services, [] { return LineitemState{}; },
      [&](std::size_t parts, auto &&sink) {
        scan::ScanPartitioned(services, connection, "lineitem", columns, parts, sink);
      },
      [](LineitemState &state, const scan::BuildRow &row) {
        state.Add(row.Int(0), row.Decimal(1), row.Decimal(2), row.Date(3), row.Char(4));
      },
      [](LineitemState &into, LineitemState &&from) { into.Merge(std::move(from)); });
}

void MeasureLineitem(const std::string &path) {
  ::duckdb::DBConfig config;
  config.options.access_mode = ::duckdb::AccessMode::READ_ONLY;
  ::duckdb::DuckDB db(path, &config);
  ::duckdb::Connection connection(db);
  connection.BeginTransaction();
  LineitemState reference;
  const auto run = [&](const std::string &name, const std::function<LineitemState()> &build) {
    const auto start = std::chrono::steady_clock::now();
    LineitemState state = build();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (reference.rows == 0) {
      reference = state;
    }
    CHECK(state == reference);
    std::cout << "lineitem " << name << ": " << state.rows << " rows in " << seconds << " s = "
              << static_cast<double>(state.rows) / seconds / 1e6 << " Mrows/s\n";
  };
  run("boxed decode, serial consumer, 8 workers", [&] { return BuildLineitemBoxed(connection, 8); });
  for (const std::size_t partitions : {1, 8, 16, 1, 8, 16}) {
    run("partitioned P=" + std::to_string(partitions), [&] { return BuildLineitemPartitioned(connection, partitions); });
  }
  connection.Rollback();
}

} // namespace

int main(int argc, char **argv) {
  try {
    PartitionBoundsAreDeterministic();
    PostgresBlockRangesAreDeterministic();
    ToyModuleIsIdenticalAtAnyPartitionCount();
    NullableDoubleSumIsBitIdenticalAtAnyThreadCount();
    BuildsReadTheirSnapshot();
    FallbackScansReadTheReadersSnapshot();
    if (argc > 1) {
      MeasureLineitem(argv[1]);
    }
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << "\n";
    return 1;
  }
  std::cout << "partitioned build tests passed\n";
  return 0;
}
