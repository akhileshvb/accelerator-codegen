// Base PostgreSQL accelerators: runtime API seen by the per-query kernels.
//
// This header is plain C++ (no PostgreSQL headers), so kernels can use the STL
// and SIMD freely.  The runtime (backends/postgres/api/base/) does every
// PostgreSQL call: heap scans under the query's active snapshot, decoding into
// columnar batches, parallel scans in PostgreSQL parallel worker processes,
// argument decoding and result emission.
//
// Nothing here survives a call: every scan, buffer and shared-memory segment
// is created per call and released before the function returns.  Kernels and
// tasks must not keep static / global / thread_local mutable state.
//
// Value encodings in batches (shared with the DuckDB kernels):
//   Int32   : int4                                  -> int32_t
//   Int64   : int8 / int4                           -> int64_t
//   Date    : date, days since 1970-01-01           -> int32_t
//   Decimal : numeric, scaled by 10^scale (exact)   -> int64_t
//   Double  : float8 / float4                       -> double
//   Text    : varchar / text (bytes, no terminator)  -> StringRef
// A column is NOT NULL unless its ColumnSpec is `nullable`: a NULL in any other
// projected column makes the runtime throw. A nullable column's cells hold 0 /
// an empty string where Batch::IsNull says NULL.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace accelerator_codegen::pg_base {

enum class ColType : uint8_t { Int32, Int64, Date, Decimal, Text, Double };

struct ColumnSpec {
	std::string name;
	ColType type;
	int scale = 0; // Decimal only: values are scaled by 10^scale
	bool nullable = false; // not allowed on a filtered column
};

enum class FilterOp : uint8_t {
	Between,    // lo <= v <= hi on an Int32 / Int64 / Date / Decimal column (Decimal in scaled units)
	TextEq,     // v == strings[0]
	TextNotEq,  // v != strings[0]
	TextIn,     // v in strings
	TextPrefix, // v starts with strings[0]
	IntIn,      // v in ints, on an Int32 / Int64 / Date column (e.g. a join key set)
};

struct FilterSpec {
	int column; // index into ScanSpec::columns
	FilterOp op;
	int64_t lo = 0;
	int64_t hi = 0;
	std::vector<std::string> strings;
	std::vector<int64_t> ints; // IntIn keys (any order, duplicates allowed)
};

//! One table scan: projected columns plus conjunctive filters applied during
//! the scan (filter columns are decoded first; other columns only for rows
//! that pass).  When a Between filter (selective per the column histogram) or
//! an IntIn filter (few keys relative to the column's distinct values) is on
//! the leading column of a btree index, the runtime reads only the matching
//! heap tuples through a TID bitmap built from that index (serial and parallel
//! scans alike); every filter is still checked on every returned tuple.  Filters are an optimization: kernels must not rely on them for
//! anything but row selection.
struct ScanSpec {
	std::string table;
	std::vector<ColumnSpec> columns;
	std::vector<FilterSpec> filters;
};

struct StringRef {
	const char *ptr;
	uint32_t len;
	uint32_t pad_;
	std::string_view View() const {
		return std::string_view(ptr, len);
	}
};
static_assert(sizeof(StringRef) == 16, "StringRef layout is shared with the parallel scan");

inline constexpr int kMaxColumns = 16;

//! A batch of rows in columnar form.  Column i points at `count` values of the
//! type given by ScanSpec::columns[i] (int32_t for Int32/Date, int64_t for
//! Int64/Decimal, double for Double, StringRef for Text).  Arrays are 64-byte
//! aligned. nulls[i] is the NULL mask (1 = NULL) of a nullable column, else null.
struct Batch {
	std::size_t count = 0;
	const void *cols[kMaxColumns] = {};
	const uint8_t *nulls[kMaxColumns] = {};

	bool IsNull(int i, std::size_t row) const {
		return nulls[i] != nullptr && nulls[i][row] != 0;
	}
	const double *F64(int i) const {
		return static_cast<const double *>(cols[i]);
	}
	const int32_t *I32(int i) const {
		return static_cast<const int32_t *>(cols[i]);
	}
	const int64_t *I64(int i) const {
		return static_cast<const int64_t *>(cols[i]);
	}
	const StringRef *Str(int i) const {
		return static_cast<const StringRef *>(cols[i]);
	}
};

//! Rows per batch (serial scans and each participant's share of a parallel scan).
inline constexpr std::size_t kBatchRows = 2048;

//! Serial scan in the calling backend.  `consume` is called once per batch;
//! the batch (and its strings) is only valid during the call.
void ScanTable(const ScanSpec &spec, const std::function<void(const Batch &)> &consume);

//! Existence probe through a btree index (an index nested-loop semi/anti
//! join): out[k] = 1 iff `table` has a row visible to the query snapshot with
//! `column` == keys[k].  `column` must be an Int32/Int64/Date column leading a
//! btree index (KernelError otherwise).  One index descent per key (keys are
//! probed in sorted order), stopping at the first visible match, so its cost is
//! proportional to the number of keys, not the table size.  Callable in the
//! leader and inside parallel tasks (e.g. on a participant's share of keys).
std::vector<uint8_t> KeysExist(const std::string &table, const std::string &column,
                               const std::vector<int64_t> &keys);

//! Serial-consumer parallel scan: `participants` - 1 PostgreSQL parallel workers
//! scan and decode shares of the table under the calling backend's active
//! snapshot and stream their batches to this backend, which calls `consume`
//! (batches arrive in an unspecified order; the batch is only valid during the
//! call; arrays are 8-byte aligned).  The rows are exactly those of ScanTable.
//! Runs ScanTable when participants <= 1 or the server cannot supply a worker.
//! For accelerator builds, whose consumer is single-threaded state, not for
//! kernels (those use RunParallel, where every participant also aggregates).
void ScanTableParallel(const ScanSpec &spec, int participants, const std::function<void(const Batch &)> &consume);

//! Heap blocks [first, first + count) of partition `p` of `partitions` over a
//! table of `blocks` blocks: a function of the table size alone, so a
//! partition's rows are the same set whoever scans it.
struct BlockRange {
	uint32_t first;
	uint32_t count;
};
inline BlockRange PartitionBlocks(uint32_t blocks, std::size_t partitions, std::size_t p) {
	const auto bound = [&](std::size_t i) { return static_cast<uint32_t>(uint64_t{blocks} * i / partitions); };
	return {bound(p), bound(p + 1) - bound(p)};
}

//! Decodes the wire form of a batch (ScanTablePartitioned) into reusable
//! aligned buffers; the batch (and its strings, which point into the message)
//! is valid until the next Decode.  Plain C++: pool threads may decode.
class BatchDecoder {
public:
	explicit BatchDecoder(const ScanSpec &spec);
	const Batch &Decode(std::string_view message);

private:
	const ScanSpec &spec;
	std::vector<std::vector<uint64_t>> words; // numeric columns
	std::vector<std::vector<StringRef>> refs; // text columns
	std::vector<std::vector<uint8_t>> masks;  // nullable columns
	Batch batch;
};

//! Partitioned scan for builds that consume partitions in parallel: the table
//! is cut into `partitions` fixed block ranges (PartitionBlocks); PostgreSQL
//! parallel workers (participants - 1 of them) each claim whole partitions and
//! scan them in block order, with no table filters.  This backend only
//! relays: `on_message(partition, message)` gets each batch in wire form (valid
//! during the call), in order within a partition, partitions interleaved; give
//! the message to a BatchDecoder on any thread.  Without a worker this backend
//! scans the partitions itself, in order.
void ScanTablePartitioned(const ScanSpec &spec, int participants, std::size_t partitions,
                          const std::function<void(std::size_t partition, std::string_view message)> &on_message);

//! Number of participants a parallel kernel should use:
//! max_parallel_workers_per_gather + 1, the same knob PostgreSQL's own
//! parallel query obeys (leader + workers).
int ParallelParticipants();

//! Parallel execution model (PostgreSQL backends cannot share buffers or heap
//! access with threads, so parallelism uses PostgreSQL parallel worker
//! processes):
//!
//!   auto parts = RunParallel("tpch_q6_scan", input, {lineitem_spec}, participants);
//!
//! starts participants-1 parallel workers (fewer if the server cannot supply
//! them) and runs the registered task in each worker AND in this backend.
//! Every participant sees the same `input` bytes (parameters, broadcast build
//! data) and pulls its share of each scan in `scans` with ctx.Scan(i, consume):
//! the scans are PostgreSQL parallel table scans under the query snapshot, so
//! all participants together see every row exactly once.  Each participant
//! sets an output byte string (e.g. a serialized partial aggregate); RunParallel
//! returns them (index 0 = this backend), in an unspecified participant order:
//! merges must be order-insensitive or sort by content to be deterministic.
//! Tasks run in separate processes: they must not touch kernel globals or
//! leader memory, only `input`, their scans and their output.
class TaskContext {
public:
	virtual ~TaskContext() = default;
	virtual int Participant() const = 0;  // 0 = leader backend
	virtual int Participants() const = 0; // launched workers + 1
	virtual std::string_view Input() const = 0;
	virtual const ScanSpec &Spec(int scan_index) const = 0;
	virtual void Scan(int scan_index, const std::function<void(const Batch &)> &consume) = 0;
	//! Claims whole partitions (PartitionBlocks of scan `scan_index`, shared by
	//! all participants) until none are left and scans each in block order.
	virtual void ScanPartitions(int scan_index, std::size_t partitions,
	                            const std::function<void(std::size_t partition, const Batch &)> &consume) = 0;
	virtual void SetOutput(std::string output) = 0;
	//! Send a message to the leader while the task runs (workers only; blocks
	//! while the leader is behind).  Used by ScanTableParallel.
	virtual void Emit(std::string_view message) = 0;
};

using TaskFn = void (*)(TaskContext &ctx);

void RegisterTask(const char *name, TaskFn fn);

struct TaskRegistrar {
	TaskRegistrar(const char *name, TaskFn fn) {
		RegisterTask(name, fn);
	}
};

std::vector<std::string> RunParallel(const char *task, std::string_view input, const std::vector<ScanSpec> &scans,
                                     int participants);

//! Little helpers for task input/output bytes (trivially copyable values).
class ByteWriter {
public:
	template <class T>
	void Put(const T &v) {
		static_assert(std::is_trivially_copyable<T>::value, "POD only");
		buf.append(reinterpret_cast<const char *>(&v), sizeof(T));
	}
	template <class T>
	void PutVector(const std::vector<T> &v) {
		static_assert(std::is_trivially_copyable<T>::value, "POD only");
		Put<uint64_t>(v.size());
		buf.append(reinterpret_cast<const char *>(v.data()), v.size() * sizeof(T));
	}
	void PutString(std::string_view s) {
		Put<uint64_t>(s.size());
		buf.append(s.data(), s.size());
	}
	std::string Take() {
		return std::move(buf);
	}
	std::string buf;
};

class ByteReader {
public:
	explicit ByteReader(std::string_view data_p) : data(data_p) {
	}
	template <class T>
	T Get() {
		static_assert(std::is_trivially_copyable<T>::value, "POD only");
		Need(sizeof(T));
		T v;
		std::memcpy(&v, data.data() + pos, sizeof(T));
		pos += sizeof(T);
		return v;
	}
	template <class T>
	std::vector<T> GetVector() {
		const auto n = Get<uint64_t>();
		Need(n * sizeof(T));
		std::vector<T> v(n);
		std::memcpy(v.data(), data.data() + pos, n * sizeof(T));
		pos += n * sizeof(T);
		return v;
	}
	std::string GetString() {
		const auto n = Get<uint64_t>();
		Need(n);
		std::string s(data.data() + pos, n);
		pos += n;
		return s;
	}
	bool Done() const {
		return pos == data.size();
	}

private:
	void Need(std::size_t n) const {
		if (pos + n > data.size()) {
			throw std::runtime_error("ByteReader: truncated input");
		}
	}
	std::string_view data;
	std::size_t pos = 0;
};

//! Run fn(task) for task in [0, tasks) on `threads` std::threads (the calling
//! thread included), for pure C++ work in one process (e.g. a large merge in
//! the leader).  Tasks are claimed dynamically.  Exceptions propagate.
void ParallelFor(std::size_t tasks, int threads, const std::function<void(std::size_t task, int thread)> &fn);

//! Date helpers (days since 1970-01-01, proleptic Gregorian).
int32_t DaysFromCivil(int y, unsigned m, unsigned d);
void CivilFromDays(int32_t days, int &y, unsigned &m, unsigned &d);

//! SQL arguments of the call.
class Args {
public:
	virtual ~Args() = default;
	virtual int Count() const = 0;
	virtual bool IsNull(int i) const = 0;
	virtual int64_t Int(int i) const = 0;          // int2/int4/int8 (numeric/float8 must be integral)
	virtual double Double(int i) const = 0;        // float4/float8/numeric/int
	virtual std::string Text(int i) const = 0;     // text/varchar/bpchar
	virtual int32_t Date(int i) const = 0;         // date -> days since 1970-01-01
	//! numeric / float8 / int argument as a scaled integer (value * 10^scale,
	//! exact for numeric with <= scale fractional digits, rounded half away
	//! from zero for float8).
	virtual int64_t Decimal(int i, int scale) const = 0;
};

//! Result rows.  Values are converted to the function's declared output
//! column types (e.g. Decimal -> numeric or float8, Int -> int4/int8/numeric).
class ResultWriter {
public:
	virtual ~ResultWriter() = default;
	virtual void Text(std::string_view v) = 0;
	virtual void Int(int64_t v) = 0;
	virtual void Double(double v) = 0;
	virtual void Decimal(__int128 scaled, int scale) = 0;
	virtual void Date(int32_t days_since_1970) = 0;
	virtual void Null() = 0;
	virtual void EndRow() = 0; // every column must have been written
};

using KernelFn = void (*)(const Args &args, ResultWriter &out);

//! A kernel and its SQL definition.  `sql_args` is the argument list without
//! parentheses ("date, double precision, double precision"), `sql_returns`
//! the RETURNS clause ("TABLE (revenue numeric)").
struct KernelEntry {
	int query_id;
	bool parallel;
	KernelFn fn;
	const char *sql_args;
	const char *sql_returns;
};

void RegisterKernel(const KernelEntry &entry);

struct KernelRegistrar {
	explicit KernelRegistrar(const KernelEntry &entry) {
		RegisterKernel(entry);
	}
};

//! Throw from kernels for invalid input; reported as a PostgreSQL ERROR.
struct KernelError : std::runtime_error {
	using std::runtime_error::runtime_error;
};

} // namespace accelerator_codegen::pg_base
