// Base PostgreSQL accelerators: PostgreSQL-facing runtime.
//
//   * ScanTable: heap scan under the active snapshot, decoded into columnar
//     batches (filter columns first, the rest only for passing rows).
//   * RunParallel: per-call ParallelContext; every participant (workers + this
//     backend) runs a registered task over its share of PostgreSQL parallel
//     table scans and returns an output byte string through a shm_mq.
//   * Args / ResultWriter: SQL argument decoding and a materialized SRF result.
//   * accel_base_dispatch: the single C entry point of every kernel
//     function (resolved by the SQL function's name), and
//     accel_base_register(schema) which creates the SQL functions.
//
// PostgreSQL errors are converted to C++ exceptions at every PostgreSQL call
// site (no longjmp across C++ frames); C++ exceptions are converted back to
// ereport(ERROR) at the entry points.
#include "backends/postgres/api/base/pg_base.hpp"
#include "runtime_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/stratnum.h"
#include "access/parallel.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "nodes/makefuncs.h"
#include "nodes/tidbitmap.h"
#include "optimizer/cost.h"
#include "port/atomics.h"
#include "storage/bufmgr.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/shm_mq.h"
#include "storage/shm_toc.h"
#include "lib/stringinfo.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"
#include "utils/wait_event.h"
}

// The base runtime is part of the main module: parallel workers and the SQL
// functions it creates load their entry points from it.
#define ACCEL_BASE_PG_LIB "accelerator_codegen_pg"

namespace accelerator_codegen::pg_base {
namespace {

constexpr int32_t kPgEpochOffsetDays = 10957; // 2000-01-01 - 1970-01-01

// ---------------------------------------------------------------------------
// PostgreSQL error -> C++ exception
// ---------------------------------------------------------------------------

struct PgError : std::runtime_error {
	PgError(int code_p, const std::string &msg) : std::runtime_error(msg), code(code_p) {
	}
	int code;
};

//! Run `body` (PostgreSQL/C code only: no C++ objects with destructors may be
//! created inside it) and turn a PostgreSQL ERROR into a PgError.
template <class F>
void PgGuard(F &&body) {
	MemoryContext oldcontext = CurrentMemoryContext;
	ErrorData *volatile edata = nullptr;
	PG_TRY();
	{
		body();
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();
	}
	PG_END_TRY();
	if (edata) {
		const int code = edata->sqlerrcode;
		std::string msg = edata->message ? edata->message : "PostgreSQL error";
		FreeErrorData(edata);
		throw PgError(code, msg);
	}
}

[[noreturn]] void ReportAndExit(int code, const char *msg) {
	ereport(ERROR, (errcode(code), errmsg("%s", msg)));
	pg_unreachable();
}

// ---------------------------------------------------------------------------
// Value decoding
// ---------------------------------------------------------------------------

const int64_t kPow10[19] = {1LL,
                            10LL,
                            100LL,
                            1000LL,
                            10000LL,
                            100000LL,
                            1000000LL,
                            10000000LL,
                            100000000LL,
                            1000000000LL,
                            10000000000LL,
                            100000000000LL,
                            1000000000000LL,
                            10000000000000LL,
                            100000000000000LL,
                            1000000000000000LL,
                            10000000000000000LL,
                            100000000000000000LL,
                            1000000000000000000LL};

__int128 Pow10Wide(int e) {
	__int128 v = 1;
	for (int i = 0; i < e; i++) {
		v *= 10;
	}
	return v;
}

//! Decode a numeric varlena (packed or not, already detoasted) to value*10^scale.
//! Returns false when the value has more fractional digits than `scale`
//! (then *out holds the truncated value), is NaN/Inf, or overflows int64.
bool DecodeNumeric(const char *data, size_t len, int scale, int64_t *out, bool *inexact) {
	*inexact = false;
	*out = 0;
	if (len < 2) {
		return false;
	}
	uint16 header;
	std::memcpy(&header, data, 2);
	int weight;
	bool negative;
	const char *digits;
	size_t ndigits;
	if ((header & 0xC000) == 0x8000) { // NUMERIC_SHORT
		negative = (header & 0x2000) != 0;
		weight = (header & 0x0040) ? (~0x003F | (header & 0x003F)) : (header & 0x003F);
		digits = data + 2;
		ndigits = (len - 2) / 2;
	} else if ((header & 0xC000) == 0xC000) { // NaN / +-Inf
		return false;
	} else { // long format: n_sign_dscale, n_weight
		if (len < 4) {
			return false;
		}
		negative = (header & 0xC000) == 0x4000;
		int16 w;
		std::memcpy(&w, data + 2, 2);
		weight = w;
		digits = data + 4;
		ndigits = (len - 4) / 2;
	}
	__int128 acc = 0;
	for (size_t i = 0; i < ndigits; i++) {
		uint16 dg;
		std::memcpy(&dg, digits + 2 * i, 2);
		if (dg == 0) {
			continue;
		}
		const int e = 4 * (weight - static_cast<int>(i)) + scale;
		if (e >= 0) {
			if (e > 34) {
				return false;
			}
			acc += static_cast<__int128>(dg) * Pow10Wide(e);
		} else if (e > -5) {
			const int64_t div = kPow10[-e];
			if (dg % div != 0) {
				*inexact = true;
			}
			acc += dg / div;
		} else {
			*inexact = true;
		}
	}
	if (negative) {
		acc = -acc;
	}
	if (acc > INT64_MAX || acc < INT64_MIN) {
		return false;
	}
	*out = static_cast<int64_t>(acc);
	return !*inexact;
}

// ---------------------------------------------------------------------------
// Scan specs: (de)serialization and resolution against a relation
// ---------------------------------------------------------------------------

std::string SerializeSpec(const ScanSpec &spec) {
	ByteWriter w;
	w.PutString(spec.table);
	w.Put<uint32_t>(static_cast<uint32_t>(spec.columns.size()));
	for (const auto &c : spec.columns) {
		w.PutString(c.name);
		w.Put<uint8_t>(static_cast<uint8_t>(c.type));
		w.Put<int32_t>(c.scale);
	}
	w.Put<uint32_t>(static_cast<uint32_t>(spec.filters.size()));
	for (const auto &f : spec.filters) {
		w.Put<int32_t>(f.column);
		w.Put<uint8_t>(static_cast<uint8_t>(f.op));
		w.Put<int64_t>(f.lo);
		w.Put<int64_t>(f.hi);
		w.Put<uint32_t>(static_cast<uint32_t>(f.strings.size()));
		for (const auto &s : f.strings) {
			w.PutString(s);
		}
		w.PutVector(f.ints);
	}
	return w.Take();
}

ScanSpec DeserializeSpec(std::string_view bytes) {
	ByteReader r(bytes);
	ScanSpec spec;
	spec.table = r.GetString();
	const auto ncols = r.Get<uint32_t>();
	for (uint32_t i = 0; i < ncols; i++) {
		ColumnSpec c;
		c.name = r.GetString();
		c.type = static_cast<ColType>(r.Get<uint8_t>());
		c.scale = r.Get<int32_t>();
		spec.columns.push_back(std::move(c));
	}
	const auto nfilters = r.Get<uint32_t>();
	for (uint32_t i = 0; i < nfilters; i++) {
		FilterSpec f;
		f.column = r.Get<int32_t>();
		f.op = static_cast<FilterOp>(r.Get<uint8_t>());
		f.lo = r.Get<int64_t>();
		f.hi = r.Get<int64_t>();
		const auto ns = r.Get<uint32_t>();
		for (uint32_t k = 0; k < ns; k++) {
			f.strings.push_back(r.GetString());
		}
		f.ints = r.GetVector<int64_t>();
		spec.filters.push_back(std::move(f));
	}
	return spec;
}

void ValidateSpec(const ScanSpec &spec) {
	if (spec.columns.empty() || spec.columns.size() > static_cast<size_t>(kMaxColumns)) {
		throw KernelError("scan of " + spec.table + ": 1.." + std::to_string(kMaxColumns) + " columns required");
	}
	for (const auto &f : spec.filters) {
		if (f.column < 0 || f.column >= static_cast<int>(spec.columns.size())) {
			throw KernelError("scan of " + spec.table + ": filter column out of range");
		}
		const ColType t = spec.columns[f.column].type;
		const bool text = t == ColType::Text;
		const bool text_op = f.op != FilterOp::Between && f.op != FilterOp::IntIn;
		if (text != text_op) {
			throw KernelError("scan of " + spec.table + ": filter op does not match column type");
		}
		if (f.op == FilterOp::IntIn && t == ColType::Decimal) {
			throw KernelError("scan of " + spec.table + ": IntIn needs an Int32/Int64/Date column");
		}
		if (text_op && f.strings.empty()) {
			throw KernelError("scan of " + spec.table + ": text filter without strings");
		}
	}
}

//! Copy of a spec with IntIn key lists sorted and deduplicated.
ScanSpec Normalize(const ScanSpec &spec) {
	ScanSpec out = spec;
	for (auto &f : out.filters) {
		if (f.op == FilterOp::IntIn) {
			std::sort(f.ints.begin(), f.ints.end());
			f.ints.erase(std::unique(f.ints.begin(), f.ints.end()), f.ints.end());
		}
	}
	return out;
}

struct ColInfo {
	AttrNumber att;
	Oid typid;
	ColType type;
	int scale;
	size_t width;
	bool filtered;
};

//! Resolved form of a ScanSpec (plain data: safe to use inside PgGuard bodies).
struct Resolved {
	const ScanSpec *spec = nullptr;
	int ncols = 0;
	ColInfo cols[kMaxColumns];
	AttrNumber max_att = 0;
	int filter_cols[kMaxColumns];
	int nfilter_cols = 0;
	int other_cols[kMaxColumns];
	int nother_cols = 0;
};

//! Must run inside PgGuard (catalog lookups may error); throws nothing itself
//! except through ereport.
void ResolveColumns(Relation rel, const ScanSpec &spec, Resolved &out, char *err, size_t errlen) {
	err[0] = '\0';
	TupleDesc desc = RelationGetDescr(rel);
	out.spec = &spec;
	out.ncols = static_cast<int>(spec.columns.size());
	bool filtered[kMaxColumns] = {};
	for (const auto &f : spec.filters) {
		filtered[f.column] = true;
	}
	for (int i = 0; i < out.ncols; i++) {
		const auto &c = spec.columns[i];
		AttrNumber att = InvalidAttrNumber;
		for (int a = 0; a < desc->natts; a++) {
			Form_pg_attribute attr = TupleDescAttr(desc, a);
			if (!attr->attisdropped && std::strcmp(NameStr(attr->attname), c.name.c_str()) == 0) {
				att = attr->attnum;
				break;
			}
		}
		if (att == InvalidAttrNumber) {
			snprintf(err, errlen, "column %s not found in %s", c.name.c_str(), RelationGetRelationName(rel));
			return;
		}
		Form_pg_attribute attr = TupleDescAttr(desc, att - 1);
		const Oid typid = attr->atttypid;
		bool ok = false;
		size_t width = 8;
		switch (c.type) {
		case ColType::Int32:
			ok = typid == INT4OID || typid == INT2OID;
			width = 4;
			break;
		case ColType::Int64:
			ok = typid == INT8OID || typid == INT4OID || typid == INT2OID;
			break;
		case ColType::Date:
			ok = typid == DATEOID;
			width = 4;
			break;
		case ColType::Decimal:
			ok = typid == NUMERICOID || typid == INT8OID || typid == INT4OID || typid == INT2OID;
			break;
		case ColType::Text:
			ok = typid == TEXTOID || typid == VARCHAROID;
			width = sizeof(StringRef);
			break;
		}
		if (!ok) {
			snprintf(err, errlen, "column %s.%s has unsupported type %u for the requested kind",
			         RelationGetRelationName(rel), c.name.c_str(), typid);
			return;
		}
		out.cols[i] = ColInfo {att, typid, c.type, c.scale, width, filtered[i]};
		out.max_att = std::max(out.max_att, att);
		if (filtered[i]) {
			out.filter_cols[out.nfilter_cols++] = i;
		} else {
			out.other_cols[out.nother_cols++] = i;
		}
	}
}

// ---------------------------------------------------------------------------
// Batch filling (C-style: runs inside PgGuard)
// ---------------------------------------------------------------------------

struct BatchBuffers {
	void *cols[kMaxColumns] = {};
	char *arena = nullptr;
	size_t arena_cap = 0;
	size_t arena_used = 0;
	~BatchBuffers() {
		for (auto *p : cols) {
			std::free(p);
		}
		std::free(arena);
	}
	void Allocate(const Resolved &rs) {
		for (int i = 0; i < rs.ncols; i++) {
			cols[i] = std::aligned_alloc(64, ((rs.cols[i].width * kBatchRows) + 63) / 64 * 64);
			if (!cols[i]) {
				throw std::bad_alloc();
			}
		}
		arena_cap = 64 * 1024;
		arena = static_cast<char *>(std::malloc(arena_cap));
		if (!arena) {
			throw std::bad_alloc();
		}
	}
};

//! Flattened TID bitmap shared by the participants of a parallel scan:
//! `count` entries, entry k at base + offsets[k]:
//!   FlatEntry header followed by max(ntuples, 0) OffsetNumbers.
struct FlatEntry {
	uint32 blockno;
	int16 ntuples; // -1 = lossy page (all tuples)
	uint16 recheck;
};

struct Cursor {
	Relation rel = nullptr;
	TableScanDesc scan = nullptr;
	TupleTableSlot *slot = nullptr;
	MemoryContext tmp = nullptr;
	bool done = false;
	uint64 tuples = 0;
	// Serial bitmap heap scan (own TID bitmap).
	TIDBitmap *tbm = nullptr;
	TBMIterator *iter = nullptr;
	TBMIterateResult *tbmres = nullptr;
	// Parallel bitmap heap scan over a flattened shared bitmap.
	const char *flat_base = nullptr;
	const uint64 *flat_offsets = nullptr;
	uint32 flat_count = 0;
	pg_atomic_uint32 *flat_next = nullptr;
	uint32 claim_pos = 0;
	uint32 claim_end = 0;
	TBMIterateResult *flat_res = nullptr; // private buffer sized for a full page
	bool flat_block = false;
};

constexpr uint32 kFlatClaim = 16; // bitmap entries (heap blocks) claimed at a time

//! Next visible tuple into cur.slot (sequential, bitmap or shared-bitmap scan).
inline bool NextTuple(Cursor &cur) {
	if (cur.flat_res) {
		for (;;) {
			if (cur.flat_block && table_scan_bitmap_next_tuple(cur.scan, cur.flat_res, cur.slot)) {
				return true;
			}
			cur.flat_block = false;
			if (cur.claim_pos == cur.claim_end) {
				const uint32 start = pg_atomic_fetch_add_u32(cur.flat_next, kFlatClaim);
				if (start >= cur.flat_count) {
					return false;
				}
				cur.claim_pos = start;
				cur.claim_end = std::min(start + kFlatClaim, cur.flat_count);
			}
			const char *p = cur.flat_base + cur.flat_offsets[cur.claim_pos++];
			FlatEntry e;
			std::memcpy(&e, p, sizeof(e));
			cur.flat_res->blockno = e.blockno;
			cur.flat_res->ntuples = e.ntuples;
			cur.flat_res->recheck = e.recheck != 0;
			if (e.ntuples > 0) {
				std::memcpy(cur.flat_res->offsets, p + sizeof(FlatEntry), sizeof(OffsetNumber) * e.ntuples);
			}
			cur.flat_block = table_scan_bitmap_next_block(cur.scan, cur.flat_res);
		}
	}
	if (!cur.iter) {
		return table_scan_getnextslot(cur.scan, ForwardScanDirection, cur.slot);
	}
	for (;;) {
		if (cur.tbmres && table_scan_bitmap_next_tuple(cur.scan, cur.tbmres, cur.slot)) {
			return true;
		}
		cur.tbmres = tbm_iterate(cur.iter);
		if (!cur.tbmres) {
			return false;
		}
		if (!table_scan_bitmap_next_block(cur.scan, cur.tbmres)) {
			cur.tbmres = nullptr;
		}
	}
}

//! Text value of an attribute (detoasted into the cursor's tmp context).
inline void TextOf(Datum d, const char **ptr, uint32_t *len) {
	struct varlena *v = reinterpret_cast<struct varlena *>(DatumGetPointer(d));
	if (VARATT_IS_EXTENDED(v) && !VARATT_IS_SHORT(v)) {
		v = pg_detoast_datum_packed(v);
	}
	*ptr = VARDATA_ANY(v);
	*len = static_cast<uint32_t>(VARSIZE_ANY_EXHDR(v));
}

//! Decode column i of the current tuple into row `row` of the batch.  Text is
//! stored as a raw (ptr,len) into the tuple; CopyText moves it to the arena.
inline void DecodeColumn(const Resolved &rs, int i, TupleTableSlot *slot, BatchBuffers &bb, size_t row) {
	const ColInfo &c = rs.cols[i];
	if (slot->tts_isnull[c.att - 1]) {
		ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
		                errmsg("base accelerator: unexpected NULL in %s", rs.spec->columns[i].name.c_str())));
	}
	const Datum d = slot->tts_values[c.att - 1];
	switch (c.type) {
	case ColType::Int32:
		static_cast<int32_t *>(bb.cols[i])[row] =
		    c.typid == INT2OID ? DatumGetInt16(d) : DatumGetInt32(d);
		break;
	case ColType::Date:
		static_cast<int32_t *>(bb.cols[i])[row] = DatumGetDateADT(d) + kPgEpochOffsetDays;
		break;
	case ColType::Int64:
		static_cast<int64_t *>(bb.cols[i])[row] =
		    c.typid == INT8OID ? DatumGetInt64(d) : (c.typid == INT4OID ? DatumGetInt32(d) : DatumGetInt16(d));
		break;
	case ColType::Decimal: {
		int64_t v;
		if (c.typid == NUMERICOID) {
			const char *p;
			uint32_t len;
			TextOf(d, &p, &len);
			bool inexact;
			if (!DecodeNumeric(p, len, c.scale, &v, &inexact)) {
				ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				                errmsg("base accelerator: %s value not representable at scale %d",
				                       rs.spec->columns[i].name.c_str(), c.scale)));
			}
		} else {
			const int64_t iv =
			    c.typid == INT8OID ? DatumGetInt64(d) : (c.typid == INT4OID ? DatumGetInt32(d) : DatumGetInt16(d));
			v = iv * kPow10[c.scale];
		}
		static_cast<int64_t *>(bb.cols[i])[row] = v;
		break;
	}
	case ColType::Text: {
		const char *p;
		uint32_t len;
		TextOf(d, &p, &len);
		auto &s = static_cast<StringRef *>(bb.cols[i])[row];
		s.ptr = p;
		s.len = len;
		s.pad_ = 0;
		break;
	}
	}
}

inline void CopyText(BatchBuffers &bb, StringRef &s) {
	if (bb.arena_used + s.len > bb.arena_cap) {
		size_t cap = bb.arena_cap * 2;
		while (cap < bb.arena_used + s.len) {
			cap *= 2;
		}
		char *grown = static_cast<char *>(std::realloc(bb.arena, cap));
		if (!grown) {
			ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("base accelerator: out of memory")));
		}
		bb.arena = grown;
		bb.arena_cap = cap;
	}
	std::memcpy(bb.arena + bb.arena_used, s.ptr, s.len);
	// Offset for now; FixupText turns it into a pointer once the batch is full.
	s.ptr = reinterpret_cast<const char *>(static_cast<uintptr_t>(bb.arena_used));
	bb.arena_used += s.len;
}

inline bool PassesFilter(const Resolved &rs, const FilterSpec &f, const BatchBuffers &bb, size_t row) {
	const ColInfo &c = rs.cols[f.column];
	switch (f.op) {
	case FilterOp::Between:
	case FilterOp::IntIn: {
		const int64_t v = (c.type == ColType::Int32 || c.type == ColType::Date)
		                      ? static_cast<const int32_t *>(bb.cols[f.column])[row]
		                      : static_cast<const int64_t *>(bb.cols[f.column])[row];
		if (f.op == FilterOp::Between) {
			return v >= f.lo && v <= f.hi;
		}
		return std::binary_search(f.ints.begin(), f.ints.end(), v);
	}
	default:
		break;
	}
	const auto &s = static_cast<const StringRef *>(bb.cols[f.column])[row];
	const std::string_view v(s.ptr, s.len);
	switch (f.op) {
	case FilterOp::TextEq:
		return v == f.strings[0];
	case FilterOp::TextNotEq:
		return v != f.strings[0];
	case FilterOp::TextIn:
		for (const auto &x : f.strings) {
			if (v == x) {
				return true;
			}
		}
		return false;
	case FilterOp::TextPrefix:
		return v.size() >= f.strings[0].size() && std::memcmp(v.data(), f.strings[0].data(), f.strings[0].size()) == 0;
	default:
		return false;
	}
}

//! Fill up to kBatchRows passing rows.  Must run inside PgGuard.
size_t FillBatch(Cursor &cur, const Resolved &rs, BatchBuffers &bb) {
	MemoryContextReset(cur.tmp);
	MemoryContext old = MemoryContextSwitchTo(cur.tmp);
	bb.arena_used = 0;
	size_t count = 0;
	const auto &filters = rs.spec->filters;
	while (count < kBatchRows) {
		if ((++cur.tuples & 1023) == 0) {
			CHECK_FOR_INTERRUPTS();
		}
		if (!NextTuple(cur)) {
			cur.done = true;
			break;
		}
		slot_getsomeattrs(cur.slot, rs.max_att);
		// Filters in order; each column is decoded when first needed.
		uint32 decoded = 0;
		bool pass = true;
		for (const auto &f : filters) {
			if (!(decoded & (1u << f.column))) {
				DecodeColumn(rs, f.column, cur.slot, bb, count);
				decoded |= 1u << f.column;
			}
			if (!PassesFilter(rs, f, bb, count)) {
				pass = false;
				break;
			}
		}
		if (!pass) {
			continue;
		}
		for (int i = 0; i < rs.ncols; i++) {
			if (!(decoded & (1u << i))) {
				DecodeColumn(rs, i, cur.slot, bb, count);
			}
		}
		for (int i = 0; i < rs.ncols; i++) {
			if (rs.cols[i].type == ColType::Text) {
				CopyText(bb, static_cast<StringRef *>(bb.cols[i])[count]);
			}
		}
		count++;
	}
	MemoryContextSwitchTo(old);
	for (int i = 0; i < rs.ncols; i++) {
		if (rs.cols[i].type == ColType::Text) {
			auto *s = static_cast<StringRef *>(bb.cols[i]);
			for (size_t r = 0; r < count; r++) {
				s[r].ptr = bb.arena + reinterpret_cast<uintptr_t>(s[r].ptr);
			}
		}
	}
	return count;
}

//! Drain an open cursor through `consume`.  Closes the cursor.
void DrainCursor(Cursor &cur, const Resolved &rs, const std::function<void(const Batch &)> &consume) {
	BatchBuffers bb;
	bb.Allocate(rs);
	Batch batch;
	for (int i = 0; i < rs.ncols; i++) {
		batch.cols[i] = bb.cols[i];
	}
	while (!cur.done) {
		size_t count = 0;
		PgGuard([&] { count = FillBatch(cur, rs, bb); });
		if (count) {
			batch.count = count;
			consume(batch);
		}
	}
	PgGuard([&] {
		if (cur.iter) {
			tbm_end_iterate(cur.iter);
			tbm_free(cur.tbm);
		}
		if (cur.flat_res) {
			pfree(cur.flat_res);
		}
		table_endscan(cur.scan);
		ExecDropSingleTupleTableSlot(cur.slot);
		table_close(cur.rel, NoLock);
		MemoryContextDelete(cur.tmp);
	});
	cur.scan = nullptr;
}

//! Fraction of the column's histogram inside [lo, hi] (1.0 without stats).
double RangeSelectivity(Oid relid, const ColInfo &c, int64_t lo, int64_t hi) {
	HeapTuple st = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(relid), Int16GetDatum(c.att), BoolGetDatum(false));
	if (!HeapTupleIsValid(st)) {
		return 1.0;
	}
	double sel = 1.0;
	AttStatsSlot sslot;
	if (get_attstatsslot(&sslot, st, STATISTIC_KIND_HISTOGRAM, InvalidOid, ATTSTATSSLOT_VALUES)) {
		int inside = 0;
		for (int i = 0; i < sslot.nvalues; i++) {
			const Datum d = sslot.values[i];
			const int64_t v = c.typid == DATEOID ? static_cast<int64_t>(DatumGetDateADT(d)) + kPgEpochOffsetDays
			                  : c.typid == INT8OID ? DatumGetInt64(d)
			                  : c.typid == INT4OID ? DatumGetInt32(d)
			                                        : DatumGetInt16(d);
			inside += v >= lo && v <= hi;
		}
		if (sslot.nvalues > 1) {
			sel = static_cast<double>(inside) / sslot.nvalues;
		}
		free_attstatsslot(&sslot);
	}
	ReleaseSysCache(st);
	return sel;
}

//! Switch a freshly opened serial cursor to a bitmap heap scan when one of the
//! spec's Between filters is on the leading column of a plain btree index and
//! the column histogram puts it at <= kBitmapMaxSelectivity of the rows.  The
//! filters are still evaluated on every returned tuple (lossy pages included).
constexpr double kBitmapMaxSelectivity = 0.35;

//! Estimated fraction of rows matching `nkeys` distinct key values (pg_statistic
//! n_distinct; 1.0 without stats).
double KeySelectivity(Relation rel, const ColInfo &c, size_t nkeys) {
	HeapTuple st = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(RelationGetRelid(rel)), Int16GetDatum(c.att),
	                               BoolGetDatum(false));
	if (!HeapTupleIsValid(st)) {
		return 1.0;
	}
	double ndistinct = reinterpret_cast<Form_pg_statistic>(GETSTRUCT(st))->stadistinct;
	ReleaseSysCache(st);
	if (ndistinct < 0) {
		ndistinct = -ndistinct * std::max<double>(rel->rd_rel->reltuples, 1.0);
	}
	if (ndistinct <= 0) {
		return 1.0;
	}
	return std::min(1.0, static_cast<double>(nkeys) / ndistinct);
}

constexpr double kBitmapMaxKeySelectivity = 0.2;

//! A valid, non-partial btree index whose leading column is `att` (InvalidOid if none).
Oid FindBtreeIndex(Relation rel, AttrNumber att) {
	List *indexes = RelationGetIndexList(rel);
	ListCell *lc;
	Oid found = InvalidOid;
	foreach (lc, indexes) {
		Relation ir = index_open(lfirst_oid(lc), AccessShareLock);
		if (ir->rd_rel->relam == BTREE_AM_OID && ir->rd_index->indkey.values[0] == att &&
		    heap_attisnull(ir->rd_indextuple, Anum_pg_index_indpred, NULL) && ir->rd_index->indisvalid) {
			found = RelationGetRelid(ir);
		}
		index_close(ir, AccessShareLock);
		if (OidIsValid(found)) {
			break;
		}
	}
	list_free(indexes);
	return found;
}

//! Choose the filter + btree index for a bitmap scan: the most selective
//! eligible Between (histogram estimate <= kBitmapMaxSelectivity) or IntIn
//! (keys / n_distinct <= kBitmapMaxKeySelectivity) filter whose column leads a
//! valid, non-partial btree index.  False -> sequential scan.
bool ChooseBitmapIndex(Relation rel, const Resolved &rs, int *filter_index, Oid *index_oid) {
	const char *off = getenv("ACCEL_BASE_NO_INDEX");
	if (off && *off && *off != '0') {
		return false;
	}
	auto index_on = [&](AttrNumber att) { return FindBtreeIndex(rel, att); };
	const Oid relid = RelationGetRelid(rel);
	int best = -1;
	Oid best_index = InvalidOid;
	double best_sel = 2.0;
	for (size_t k = 0; k < rs.spec->filters.size(); k++) {
		const auto &f = rs.spec->filters[k];
		const ColInfo &c = rs.cols[f.column];
		if (!(c.typid == DATEOID || c.typid == INT4OID || c.typid == INT8OID || c.typid == INT2OID)) {
			continue;
		}
		double sel;
		if (f.op == FilterOp::Between) {
			sel = RangeSelectivity(relid, c, f.lo, f.hi);
			if (sel > kBitmapMaxSelectivity) {
				continue;
			}
		} else if (f.op == FilterOp::IntIn) {
			sel = KeySelectivity(rel, c, f.ints.size());
			if (sel > kBitmapMaxKeySelectivity) {
				continue;
			}
		} else {
			continue;
		}
		if (sel >= best_sel) {
			continue;
		}
		const Oid index = index_on(c.att);
		if (OidIsValid(index)) {
			best = static_cast<int>(k);
			best_index = index;
			best_sel = sel;
		}
	}
	if (best < 0) {
		return false;
	}
	*filter_index = best;
	*index_oid = best_index;
	return true;
}

//! Index key datum of `v` for a column of type typid; false if out of range.
bool KeyDatum(Oid typid, int64_t v, Datum *out) {
	if (typid == DATEOID) {
		const int64_t pg = v - kPgEpochOffsetDays;
		if (pg < PG_INT32_MIN || pg > PG_INT32_MAX) {
			return false;
		}
		*out = DateADTGetDatum(static_cast<DateADT>(pg));
	} else if (typid == INT8OID) {
		*out = Int64GetDatum(v);
	} else if (typid == INT4OID) {
		if (v < PG_INT32_MIN || v > PG_INT32_MAX) {
			return false;
		}
		*out = Int32GetDatum(static_cast<int32>(v));
	} else {
		if (v < PG_INT16_MIN || v > PG_INT16_MAX) {
			return false;
		}
		*out = Int16GetDatum(static_cast<int16>(v));
	}
	return true;
}

//! TID bitmap of the tuples the chosen index filter can match (see
//! ChooseBitmapIndex), or nullptr for a sequential scan.  The bitmap may be
//! lossy and include extra tuples: the scan filters are applied regardless.
TIDBitmap *BuildBitmap(Relation rel, const Resolved &rs, Snapshot snapshot) {
	int fi;
	Oid index_oid;
	if (!ChooseBitmapIndex(rel, rs, &fi, &index_oid)) {
		return nullptr;
	}
	const FilterSpec &f = rs.spec->filters[fi];
	const ColInfo &c = rs.cols[f.column];
	Relation index = index_open(index_oid, AccessShareLock);
	const Oid opfamily = index->rd_opfamily[0];
	ScanKeyData keys[2];
	int nkeys = 0;
	auto add_key = [&](StrategyNumber strategy, int64_t bound) {
		Datum d;
		const Oid opno = get_opfamily_member(opfamily, c.typid, c.typid, strategy);
		if (!KeyDatum(c.typid, bound, &d) || !OidIsValid(opno)) {
			return;
		}
		ScanKeyInit(&keys[nkeys++], 1, strategy, get_opcode(opno), d);
	};
	if (f.op == FilterOp::Between) {
		add_key(BTGreaterEqualStrategyNumber, f.lo);
		add_key(BTLessEqualStrategyNumber, f.hi);
	} else {
		// key = ANY(array): btree supports array keys in bitmap scans.
		Datum *elems = static_cast<Datum *>(palloc(sizeof(Datum) * std::max<size_t>(f.ints.size(), 1)));
		int n = 0;
		for (const int64_t v : f.ints) {
			if (KeyDatum(c.typid, v, &elems[n])) {
				n++;
			}
		}
		const Oid opno = get_opfamily_member(opfamily, c.typid, c.typid, BTEqualStrategyNumber);
		if (OidIsValid(opno)) {
			ArrayType *arr = construct_array_builtin(elems, n, c.typid);
			ScanKeyEntryInitialize(&keys[nkeys++], SK_SEARCHARRAY, 1, BTEqualStrategyNumber, c.typid,
			                       index->rd_indcollation[0], get_opcode(opno), PointerGetDatum(arr));
		}
	}
	if (nkeys == 0) {
		index_close(index, AccessShareLock);
		return nullptr;
	}
	TIDBitmap *tbm = tbm_create(work_mem * 1024L, NULL);
	IndexScanDesc iscan = index_beginscan_bitmap(index, snapshot, nkeys);
	index_rescan(iscan, keys, nkeys, NULL, 0);
	index_getbitmap(iscan, tbm);
	index_endscan(iscan);
	index_close(index, AccessShareLock);
	return tbm;
}

bool TryBitmapScan(Cursor &cur, const Resolved &rs, Snapshot snapshot) {
	TIDBitmap *tbm = BuildBitmap(cur.rel, rs, snapshot);
	if (!tbm) {
		return false;
	}
	cur.tbm = tbm;
	cur.scan = table_beginscan_bm(cur.rel, snapshot, 0, NULL, true);
	cur.iter = tbm_begin_iterate(cur.tbm);
	return true;
}

//! Flatten a TID bitmap (consumed) into `out`: uint64 offsets[count] followed
//! by the entries; returns the entry count.
uint32 FlattenBitmap(TIDBitmap *tbm, StringInfo out) {
	StringInfoData entries;
	initStringInfo(&entries);
	StringInfoData offsets;
	initStringInfo(&offsets);
	uint32 count = 0;
	TBMIterator *it = tbm_begin_iterate(tbm);
	TBMIterateResult *r;
	while ((r = tbm_iterate(it)) != NULL) {
		while (entries.len % 4 != 0) {
			appendStringInfoChar(&entries, '\0');
		}
		const uint64 off = static_cast<uint64>(entries.len);
		appendBinaryStringInfo(&offsets, reinterpret_cast<const char *>(&off), sizeof(off));
		FlatEntry e {r->blockno, static_cast<int16>(r->ntuples), static_cast<uint16>(r->recheck ? 1 : 0)};
		appendBinaryStringInfo(&entries, reinterpret_cast<const char *>(&e), sizeof(e));
		if (r->ntuples > 0) {
			appendBinaryStringInfo(&entries, reinterpret_cast<const char *>(r->offsets),
			                       static_cast<int>(sizeof(OffsetNumber) * r->ntuples));
		}
		count++;
	}
	tbm_end_iterate(it);
	tbm_free(tbm);
	// out = offsets (rebased past the offset table) + entries
	const uint64 table_bytes = static_cast<uint64>(count) * sizeof(uint64);
	for (uint32 k = 0; k < count; k++) {
		uint64 off;
		std::memcpy(&off, offsets.data + k * sizeof(uint64), sizeof(off));
		off += table_bytes;
		appendBinaryStringInfo(out, reinterpret_cast<const char *>(&off), sizeof(off));
	}
	appendBinaryStringInfo(out, entries.data, entries.len);
	pfree(entries.data);
	pfree(offsets.data);
	return count;
}

} // namespace

// ---------------------------------------------------------------------------
// Serial scan
// ---------------------------------------------------------------------------

void ScanTable(const ScanSpec &spec_in, const std::function<void(const Batch &)> &consume) {
	ValidateSpec(spec_in);
	const ScanSpec spec = Normalize(spec_in);
	Resolved rs;
	Cursor cur;
	char err[256];
	PgGuard([&] {
		RangeVar *rv = makeRangeVar(NULL, pstrdup(spec.table.c_str()), -1);
		cur.rel = table_openrv(rv, AccessShareLock);
		ResolveColumns(cur.rel, spec, rs, err, sizeof(err));
		if (err[0]) {
			table_close(cur.rel, AccessShareLock);
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("%s", err)));
		}
		if (!TryBitmapScan(cur, rs, GetActiveSnapshot())) {
			cur.scan = table_beginscan(cur.rel, GetActiveSnapshot(), 0, NULL);
		}
		cur.slot = table_slot_create(cur.rel, NULL);
		cur.tmp = AllocSetContextCreate(CurrentMemoryContext, "accel base scan", ALLOCSET_DEFAULT_SIZES);
	});
	DrainCursor(cur, rs, consume);
}

int ParallelParticipants() {
	return std::max(1, max_parallel_workers_per_gather + 1);
}

std::vector<uint8_t> KeysExist(const std::string &table, const std::string &column,
                               const std::vector<int64_t> &keys) {
	std::vector<uint8_t> out(keys.size(), 0);
	if (keys.empty()) {
		return out;
	}
	// Probe in key order (index locality); results land at the caller's positions.
	std::vector<uint32_t> order(keys.size());
	for (uint32_t k = 0; k < order.size(); k++) {
		order[k] = k;
	}
	std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return keys[a] < keys[b]; });
	char err[256];
	err[0] = '\0';
	uint8_t *result = out.data();
	const uint32_t *ord = order.data();
	const int64_t *key_values = keys.data();
	const size_t n = keys.size();
	PgGuard([&] {
		RangeVar *rv = makeRangeVar(NULL, pstrdup(table.c_str()), -1);
		Relation rel = table_openrv(rv, AccessShareLock);
		const AttrNumber att = get_attnum(RelationGetRelid(rel), column.c_str());
		Oid typid = InvalidOid;
		if (att != InvalidAttrNumber) {
			typid = TupleDescAttr(RelationGetDescr(rel), att - 1)->atttypid;
		}
		const Oid index_oid = att == InvalidAttrNumber ? InvalidOid : FindBtreeIndex(rel, att);
		if (!(typid == INT8OID || typid == INT4OID || typid == INT2OID || typid == DATEOID) || !OidIsValid(index_oid)) {
			snprintf(err, sizeof(err), "KeysExist: %s.%s needs an integer/date column leading a btree index",
			         table.c_str(), column.c_str());
			table_close(rel, AccessShareLock);
			return;
		}
		Relation index = index_open(index_oid, AccessShareLock);
		const Oid opno = get_opfamily_member(index->rd_opfamily[0], typid, typid, BTEqualStrategyNumber);
		const RegProcedure proc = get_opcode(opno);
		IndexScanDesc scan = index_beginscan(rel, index, GetActiveSnapshot(), 1, 0);
		TupleTableSlot *slot = table_slot_create(rel, NULL);
		ScanKeyData skey;
		for (size_t i = 0; i < n; i++) {
			if ((i & 1023) == 0) {
				CHECK_FOR_INTERRUPTS();
			}
			const uint32_t k = ord[i];
			if (i > 0 && key_values[k] == key_values[ord[i - 1]]) {
				result[k] = result[ord[i - 1]];
				continue;
			}
			Datum d;
			if (!KeyDatum(typid, key_values[k], &d)) {
				continue;
			}
			ScanKeyInit(&skey, 1, BTEqualStrategyNumber, proc, d);
			index_rescan(scan, &skey, 1, NULL, 0);
			result[k] = index_getnext_slot(scan, ForwardScanDirection, slot) ? 1 : 0;
		}
		index_endscan(scan);
		ExecDropSingleTupleTableSlot(slot);
		index_close(index, NoLock);
		table_close(rel, NoLock);
	});
	if (err[0]) {
		throw KernelError(err);
	}
	return out;
}

// ---------------------------------------------------------------------------
// Parallel tasks
// ---------------------------------------------------------------------------

namespace {

constexpr uint64 kTocKeyHeader = UINT64CONST(0xACCE550000000001);
constexpr size_t kMaxScans = 8;
constexpr Size kQueueSize = 256 * 1024;
//! Partitioned scans buffer more, so the workers decode ahead while the
//! partitions consume the previous round.
constexpr Size kPartitionedQueueSize = 4 * 1024 * 1024;

//! Scan of the heap blocks [first, first + count) of `relid`, in block order,
//! under the active snapshot (no synchronized start: the order is fixed).
void ScanBlockRange(Oid relid, const ScanSpec &spec, BlockNumber first, BlockNumber count,
                    const std::function<void(const Batch &)> &consume) {
	Resolved rs;
	Cursor cur;
	char err[256];
	PgGuard([&] {
		cur.rel = table_open(relid, AccessShareLock);
		ResolveColumns(cur.rel, spec, rs, err, sizeof(err));
		if (err[0]) {
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("%s", err)));
		}
		cur.scan = table_beginscan_strat(cur.rel, GetActiveSnapshot(), 0, NULL, true, false);
		heap_setscanlimits(cur.scan, first, count);
		cur.slot = table_slot_create(cur.rel, NULL);
		cur.tmp = AllocSetContextCreate(CurrentMemoryContext, "accel base range scan", ALLOCSET_DEFAULT_SIZES);
	});
	DrainCursor(cur, rs, consume);
}

struct ParallelHeader {
	char task[64];
	uint32 nscans;
	uint32 participants_requested;
	Oid relid[kMaxScans];
	Size spec_off[kMaxScans];
	Size spec_len[kMaxScans];
	Size pscan_off[kMaxScans];
	Size input_off;
	Size input_len;
	Size queue_off;
	Size queue_size;
	uint32 nqueues;
	pg_atomic_uint64 work_counter; // next partition of a partitioned scan
	uint32 nblocks[kMaxScans];     // table sizes the partitions are cut from
	// Shared bitmap heap scans: scan i reads the flattened TID bitmap at
	// flat_off[i] (flat_count[i] heap blocks, claimed kFlatClaim at a time).
	uint8 flat_mode[kMaxScans];
	Size flat_off[kMaxScans];
	uint32 flat_count[kMaxScans];
	pg_atomic_uint32 flat_next[kMaxScans];
};

class TaskContextImpl final : public TaskContext {
public:
	TaskContextImpl(ParallelHeader *h_p, char *base_p, int participant_p, shm_mq_handle *mqh_p)
	    : h(h_p), base(base_p), participant(participant_p), mqh(mqh_p) {
		for (uint32 i = 0; i < h->nscans; i++) {
			specs.push_back(DeserializeSpec(std::string_view(base + h->spec_off[i], h->spec_len[i])));
		}
	}
	int Participant() const override {
		return participant;
	}
	int Participants() const override {
		return static_cast<int>(h->participants_requested);
	}
	std::string_view Input() const override {
		return std::string_view(base + h->input_off, h->input_len);
	}
	const ScanSpec &Spec(int scan_index) const override {
		return specs.at(scan_index);
	}
	void Emit(std::string_view message) override {
		if (!mqh) {
			throw KernelError("TaskContext::Emit: only workers can emit");
		}
		shm_mq_result res = SHM_MQ_DETACHED;
		PgGuard([&] { res = shm_mq_send(mqh, message.size(), message.data(), false, false); });
		if (res != SHM_MQ_SUCCESS) {
			throw KernelError("base accelerator: could not send a batch to the leader");
		}
	}
	void Scan(int scan_index, const std::function<void(const Batch &)> &consume) override {
		if (scan_index < 0 || scan_index >= static_cast<int>(specs.size())) {
			throw KernelError("TaskContext::Scan: scan index out of range");
		}
		const ScanSpec &spec = specs[scan_index];
		Resolved rs;
		Cursor cur;
		char err[256];
		ParallelTableScanDesc pscan = reinterpret_cast<ParallelTableScanDesc>(base + h->pscan_off[scan_index]);
		const Oid relid = h->relid[scan_index];
		const bool flat = h->flat_mode[scan_index] != 0;
		PgGuard([&] {
			cur.rel = table_open(relid, AccessShareLock);
			ResolveColumns(cur.rel, spec, rs, err, sizeof(err));
			if (err[0]) {
				ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("%s", err)));
			}
			if (flat) {
				const char *fb = base + h->flat_off[scan_index];
				cur.flat_count = h->flat_count[scan_index];
				cur.flat_offsets = reinterpret_cast<const uint64 *>(fb);
				cur.flat_base = fb;
				cur.flat_next = &h->flat_next[scan_index];
				cur.flat_res = static_cast<TBMIterateResult *>(
				    palloc0(offsetof(TBMIterateResult, offsets) + sizeof(OffsetNumber) * MaxHeapTuplesPerPage));
				cur.scan = table_beginscan_bm(cur.rel, GetActiveSnapshot(), 0, NULL, true);
			} else {
				cur.scan = table_beginscan_parallel(cur.rel, pscan);
			}
			cur.slot = table_slot_create(cur.rel, NULL);
			cur.tmp = AllocSetContextCreate(CurrentMemoryContext, "accel base pscan", ALLOCSET_DEFAULT_SIZES);
		});
		DrainCursor(cur, rs, consume);
	}
	void ScanPartitions(int scan_index, std::size_t partitions,
	                    const std::function<void(std::size_t, const Batch &)> &consume) override {
		if (scan_index < 0 || scan_index >= static_cast<int>(specs.size())) {
			throw KernelError("TaskContext::ScanPartitions: scan index out of range");
		}
		for (;;) {
			const uint64 p = pg_atomic_fetch_add_u64(&h->work_counter, 1);
			if (p >= partitions) {
				return;
			}
			const BlockRange blocks = PartitionBlocks(h->nblocks[scan_index], partitions, p);
			if (blocks.count) {
				ScanBlockRange(h->relid[scan_index], specs[scan_index], blocks.first, blocks.count,
				               [&](const Batch &batch) { consume(p, batch); });
			}
		}
	}
	void SetOutput(std::string out) override {
		output = std::move(out);
	}

	std::string output;


private:
	ParallelHeader *h;
	char *base;
	int participant;
	shm_mq_handle *mqh; // workers only
	std::vector<ScanSpec> specs;
};

//! Run the task for one participant; returns an error message or "".
std::string RunTask(ParallelHeader *h, char *base, int participant, shm_mq_handle *mqh, std::string &output) {
	TaskFn fn = FindTask(h->task);
	if (!fn) {
		return std::string("base accelerator: unknown parallel task ") + h->task;
	}
	try {
		TaskContextImpl ctx(h, base, participant, mqh);
		fn(ctx);
		output = std::move(ctx.output);
	} catch (const std::exception &e) {
		return e.what();
	}
	return "";
}

} // namespace

namespace {

constexpr const char *kStreamScanTask = "base_scan_stream";

//! Messages of a streaming run.
using MessageSink = std::function<void(std::string_view)>;

//! Hand every message of the workers' queues to `sink` until all have detached.
//! The queues are polled in turn; with nothing to read the leader sleeps on its
//! latch, which a sender sets after writing (and the workers' error / exit
//! signals set too, so a failed worker raises its error here).  The empty
//! message that ends a task's output is not passed on.
void DrainWorkers(shm_mq_handle **mqh, int workers, const MessageSink &sink) {
	std::vector<bool> open(workers, true);
	int remaining = workers;
	while (remaining > 0) {
		bool progress = false;
		for (int i = 0; i < workers; i++) {
			if (!open[i]) {
				continue;
			}
			Size nbytes = 0;
			void *data = nullptr;
			shm_mq_result res = SHM_MQ_DETACHED;
			PgGuard([&] { res = shm_mq_receive(mqh[i], &nbytes, &data, true); });
			if (res == SHM_MQ_SUCCESS) {
				progress = true;
				if (nbytes) {
					sink(std::string_view(static_cast<const char *>(data), nbytes));
				}
			} else if (res == SHM_MQ_DETACHED) {
				open[i] = false;
				remaining--;
			}
		}
		if (!progress && remaining > 0) {
			PgGuard([&] {
				(void)WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, 0, PG_WAIT_EXTENSION);
				ResetLatch(MyLatch);
				CHECK_FOR_INTERRUPTS();
			});
		}
	}
}

//! RunParallel's engine.  With a `sink` the leader runs no task: it receives the
//! workers' Emit()ted messages (round-robin as they arrive) instead, and
//! `launched` reports how many workers started (0: nothing ran, the caller
//! scans itself).
std::vector<std::string> RunParallelImpl(const char *task, std::string_view input, const std::vector<ScanSpec> &scans,
                                         int participants, const MessageSink *sink, int &launched,
                                         Size queue_size = kQueueSize) {
	if (std::strlen(task) >= sizeof(ParallelHeader::task)) {
		throw KernelError("parallel task name too long");
	}
	if (scans.size() > kMaxScans) {
		throw KernelError("too many scans in one parallel task");
	}
	if (!FindTask(task)) {
		throw KernelError(std::string("unknown parallel task ") + task);
	}
	std::vector<std::string> spec_bytes;
	std::vector<ScanSpec> norm;
	for (const auto &s : scans) {
		ValidateSpec(s);
		norm.push_back(Normalize(s));
		spec_bytes.push_back(SerializeSpec(norm.back()));
	}
	const int nworkers = std::max(0, participants - 1);

	ParallelContext *pcxt = nullptr;
	ParallelHeader *h = nullptr;
	char *base = nullptr;
	shm_mq_handle *mqh[1024];
	Relation rels[kMaxScans] = {};
	if (nworkers > 1024) {
		throw KernelError("too many parallel workers");
	}

	// Layout inside one toc chunk: header | specs | input | pscans | queues.
	PgGuard([&] {
		Snapshot snapshot = GetActiveSnapshot();
		StringInfoData flat[kMaxScans];
		uint32 flat_count[kMaxScans];
		bool flat_mode[kMaxScans];
		for (size_t i = 0; i < scans.size(); i++) {
			RangeVar *rv = makeRangeVar(NULL, pstrdup(scans[i].table.c_str()), -1);
			rels[i] = table_openrv(rv, AccessShareLock);
			Resolved rs;
			char err[256];
			ResolveColumns(rels[i], norm[i], rs, err, sizeof(err));
			TIDBitmap *tbm = err[0] ? nullptr : BuildBitmap(rels[i], rs, snapshot);
			initStringInfo(&flat[i]);
			flat_mode[i] = tbm != nullptr;
			flat_count[i] = tbm ? FlattenBitmap(tbm, &flat[i]) : 0;
		}
		Size total = MAXALIGN(sizeof(ParallelHeader));
		Size flat_off[kMaxScans];
		for (size_t i = 0; i < scans.size(); i++) {
			flat_off[i] = total;
			total = MAXALIGN(total + flat[i].len);
		}
		Size spec_off[kMaxScans], pscan_off[kMaxScans], pscan_size[kMaxScans];
		for (size_t i = 0; i < scans.size(); i++) {
			spec_off[i] = total;
			total = MAXALIGN(total + spec_bytes[i].size());
		}
		const Size input_off = total;
		total = BUFFERALIGN(total + input.size());
		for (size_t i = 0; i < scans.size(); i++) {
			pscan_off[i] = total;
			pscan_size[i] = table_parallelscan_estimate(rels[i], snapshot);
			total = BUFFERALIGN(total + pscan_size[i]);
		}
		const Size queue_off = total;
		total += static_cast<Size>(nworkers) * queue_size;

		EnterParallelMode();
		pcxt = CreateParallelContext("$libdir/" ACCEL_BASE_PG_LIB, "accel_base_worker_main", nworkers);
		shm_toc_estimate_chunk(&pcxt->estimator, total);
		shm_toc_estimate_keys(&pcxt->estimator, 1);
		InitializeParallelDSM(pcxt);
		base = static_cast<char *>(shm_toc_allocate(pcxt->toc, total));
		shm_toc_insert(pcxt->toc, kTocKeyHeader, base);
		h = reinterpret_cast<ParallelHeader *>(base);
		std::memset(h, 0, sizeof(ParallelHeader));
		std::strncpy(h->task, task, sizeof(h->task) - 1);
		h->nscans = static_cast<uint32>(scans.size());
		h->participants_requested = static_cast<uint32>(nworkers + 1);
		pg_atomic_init_u64(&h->work_counter, 0);
		for (size_t i = 0; i < scans.size(); i++) {
			h->nblocks[i] = RelationGetNumberOfBlocks(rels[i]);
			h->flat_mode[i] = flat_mode[i];
			h->flat_off[i] = flat_off[i];
			h->flat_count[i] = flat_count[i];
			pg_atomic_init_u32(&h->flat_next[i], 0);
			if (flat[i].len) {
				std::memcpy(base + flat_off[i], flat[i].data, flat[i].len);
			}
			pfree(flat[i].data);
			h->relid[i] = RelationGetRelid(rels[i]);
			h->spec_off[i] = spec_off[i];
			h->spec_len[i] = spec_bytes[i].size();
			std::memcpy(base + spec_off[i], spec_bytes[i].data(), spec_bytes[i].size());
			h->pscan_off[i] = pscan_off[i];
			table_parallelscan_initialize(rels[i], reinterpret_cast<ParallelTableScanDesc>(base + pscan_off[i]),
			                              snapshot);
		}
		h->input_off = input_off;
		h->input_len = input.size();
		if (!input.empty()) {
			std::memcpy(base + input_off, input.data(), input.size());
		}
		h->queue_off = queue_off;
		h->queue_size = queue_size;
		h->nqueues = static_cast<uint32>(pcxt->nworkers);
		for (int i = 0; i < pcxt->nworkers; i++) {
			shm_mq *mq = shm_mq_create(base + queue_off + static_cast<Size>(i) * queue_size, queue_size);
			shm_mq_set_receiver(mq, MyProc);
			mqh[i] = shm_mq_attach(mq, pcxt->seg, NULL);
		}
		LaunchParallelWorkers(pcxt);
		for (int i = 0; i < pcxt->nworkers_launched; i++) {
			shm_mq_set_handle(mqh[i], pcxt->worker[i].bgwhandle);
		}
	});

	std::vector<std::string> outputs;
	launched = pcxt->nworkers_launched;
	if (sink) {
		DrainWorkers(mqh, launched, *sink);
	} else {
		outputs.emplace_back();
		const std::string leader_error = RunTask(h, base, 0, nullptr, outputs[0]);
		if (!leader_error.empty()) {
			// The transaction abort that follows tears down the parallel context.
			throw KernelError(leader_error);
		}
		for (int i = 0; i < launched; i++) {
			Size nbytes = 0;
			void *data = nullptr;
			shm_mq_result res = SHM_MQ_DETACHED;
			PgGuard([&] { res = shm_mq_receive(mqh[i], &nbytes, &data, false); });
			if (res != SHM_MQ_SUCCESS) {
				// Worker failed: WaitForParallelWorkersToFinish re-throws its error.
				break;
			}
			outputs.emplace_back(static_cast<const char *>(data), nbytes);
		}
	}
	PgGuard([&] {
		WaitForParallelWorkersToFinish(pcxt);
		DestroyParallelContext(pcxt);
		ExitParallelMode();
		for (size_t i = 0; i < scans.size(); i++) {
			table_close(rels[i], NoLock);
		}
	});
	if (!sink && static_cast<int>(outputs.size()) != launched + 1) {
		throw KernelError("parallel worker exited without output");
	}
	return outputs;
}

} // namespace

std::vector<std::string> RunParallel(const char *task, std::string_view input, const std::vector<ScanSpec> &scans,
                                     int participants) {
	int launched = 0;
	return RunParallelImpl(task, input, scans, participants, nullptr, launched);
}

namespace {

//! Worker side of ScanTableParallel: stream every batch of the scan.
void StreamScanTask(TaskContext &ctx) {
	const ScanSpec &spec = ctx.Spec(0);
	ctx.Scan(0, [&](const Batch &batch) { ctx.Emit(EncodeBatch(spec, batch)); });
}
const TaskRegistrar kStreamScanRegistrar(kStreamScanTask, &StreamScanTask);

//! Worker side of ScanTablePartitioned: claim partitions, stream their batches
//! tagged with the partition number.
constexpr const char *kStreamPartitionsTask = "base_scan_partitions";
void StreamPartitionsTask(TaskContext &ctx) {
	const ScanSpec &spec = ctx.Spec(0);
	ByteReader input(ctx.Input());
	const auto partitions = input.Get<uint64_t>();
	ctx.ScanPartitions(0, partitions, [&](std::size_t partition, const Batch &batch) {
		ByteWriter message;
		message.Put<uint32_t>(static_cast<uint32_t>(partition));
		ctx.Emit(message.buf + EncodeBatch(spec, batch));
	});
}
const TaskRegistrar kStreamPartitionsRegistrar(kStreamPartitionsTask, &StreamPartitionsTask);

//! The table's oid and block count, read in the leader.
std::pair<Oid, uint32_t> TableBlocks(const std::string &table) {
	std::pair<Oid, uint32_t> out;
	PgGuard([&] {
		Relation rel = table_openrv(makeRangeVar(NULL, pstrdup(table.c_str()), -1), AccessShareLock);
		out = {RelationGetRelid(rel), RelationGetNumberOfBlocks(rel)};
		table_close(rel, NoLock);
	});
	return out;
}

} // namespace

void ScanTableParallel(const ScanSpec &spec_in, int participants, const std::function<void(const Batch &)> &consume) {
	ValidateSpec(spec_in);
	const ScanSpec spec = Normalize(spec_in);
	BatchDecoder decoder(spec);
	int launched = 0;
	if (participants > 1) {
		const MessageSink sink = [&](std::string_view message) { consume(decoder.Decode(message)); };
		RunParallelImpl(kStreamScanTask, {}, {spec}, participants, &sink, launched);
	}
	if (launched == 0) {
		ScanTable(spec, consume);
	}
}

void ScanTablePartitioned(const ScanSpec &spec, int participants, std::size_t partitions,
                          const std::function<void(std::size_t, std::string_view)> &on_message) {
	ValidateSpec(spec);
	if (!spec.filters.empty()) {
		throw KernelError("scan of " + spec.table + ": partitioned scans take no filters");
	}
	int launched = 0;
	if (participants > 1) {
		ByteWriter input;
		input.Put<uint64_t>(partitions);
		const MessageSink sink = [&](std::string_view message) {
			uint32_t partition;
			std::memcpy(&partition, message.data(), sizeof(partition));
			on_message(partition, message.substr(sizeof(partition)));
		};
		RunParallelImpl(kStreamPartitionsTask, input.buf, {spec}, participants, &sink, launched, kPartitionedQueueSize);
	}
	if (launched == 0) {
		const auto [relid, blocks] = TableBlocks(spec.table);
		for (std::size_t p = 0; p < partitions; p++) {
			const BlockRange range = PartitionBlocks(blocks, partitions, p);
			if (range.count) {
				ScanBlockRange(relid, spec, range.first, range.count,
				               [&](const Batch &batch) { on_message(p, EncodeBatch(spec, batch)); });
			}
		}
	}
}

} // namespace accelerator_codegen::pg_base

// ---------------------------------------------------------------------------
// Worker entry point
// ---------------------------------------------------------------------------

extern "C" PGDLLEXPORT void accel_base_worker_main(dsm_segment *seg, shm_toc *toc);

extern "C" void accel_base_worker_main(dsm_segment *seg, shm_toc *toc) {
	using namespace accelerator_codegen::pg_base;
	char *base = static_cast<char *>(shm_toc_lookup(toc, kTocKeyHeader, false));
	auto *h = reinterpret_cast<ParallelHeader *>(base);
	const int worker = ParallelWorkerNumber;
	shm_mq *mq = reinterpret_cast<shm_mq *>(base + h->queue_off + static_cast<Size>(worker) * h->queue_size);
	shm_mq_set_sender(mq, MyProc);
	shm_mq_handle *mqh = shm_mq_attach(mq, seg, NULL);
	std::string output;
	std::string error = RunTask(h, base, worker + 1, mqh, output);
	if (!error.empty()) {
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s", error.c_str())));
	}
	const shm_mq_result res = shm_mq_send(mqh, output.size(), output.data(), false, true);
	if (res != SHM_MQ_SUCCESS) {
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("base accelerator: could not send task output")));
	}
	shm_mq_detach(mqh);
}

// ---------------------------------------------------------------------------
// Arguments and results
// ---------------------------------------------------------------------------

namespace accelerator_codegen::pg_base {
namespace {

int64_t ParseScaledDecimal(const char *text, int scale) {
	// Exact parse of [-]digits[.digits], rounding half away from zero beyond `scale`.
	const char *p = text;
	bool neg = false;
	while (*p == ' ') {
		p++;
	}
	if (*p == '-' || *p == '+') {
		neg = *p == '-';
		p++;
	}
	__int128 v = 0;
	while (*p >= '0' && *p <= '9') {
		v = v * 10 + (*p++ - '0');
	}
	int frac = 0;
	int round_digit = 0;
	if (*p == '.') {
		p++;
		while (*p >= '0' && *p <= '9') {
			if (frac < scale) {
				v = v * 10 + (*p - '0');
				frac++;
			} else if (frac == scale) {
				round_digit = *p - '0';
				frac++;
			}
			p++;
		}
	}
	for (int f = std::min(frac, scale); f < scale; f++) {
		v *= 10;
	}
	if (round_digit >= 5) {
		v += 1;
	}
	if (neg) {
		v = -v;
	}
	if (v > INT64_MAX || v < INT64_MIN) {
		throw KernelError("decimal argument out of range");
	}
	return static_cast<int64_t>(v);
}

class ArgsImpl final : public Args {
public:
	explicit ArgsImpl(FunctionCallInfo fcinfo_p) : fcinfo(fcinfo_p) {
		const int n = PG_NARGS();
		for (int i = 0; i < n; i++) {
			types.push_back(get_fn_expr_argtype(fcinfo->flinfo, i));
		}
	}
	int Count() const override {
		return static_cast<int>(types.size());
	}
	bool IsNull(int i) const override {
		Check(i);
		return PG_ARGISNULL(i);
	}
	int64_t Int(int i) const override {
		const Datum d = Get(i);
		switch (types[i]) {
		case INT2OID:
			return DatumGetInt16(d);
		case INT4OID:
			return DatumGetInt32(d);
		case INT8OID:
			return DatumGetInt64(d);
		case FLOAT8OID: {
			const double v = DatumGetFloat8(d);
			if (v != std::floor(v)) {
				throw KernelError("integer argument expected");
			}
			return static_cast<int64_t>(v);
		}
		case NUMERICOID:
			return Decimal(i, 0);
		default:
			throw KernelError("argument " + std::to_string(i + 1) + " is not an integer");
		}
	}
	double Double(int i) const override {
		const Datum d = Get(i);
		switch (types[i]) {
		case FLOAT8OID:
			return DatumGetFloat8(d);
		case FLOAT4OID:
			return DatumGetFloat4(d);
		case INT2OID:
		case INT4OID:
		case INT8OID:
			return static_cast<double>(Int(i));
		case NUMERICOID: {
			double v = 0;
			PgGuard([&] { v = DatumGetFloat8(DirectFunctionCall1(numeric_float8, d)); });
			return v;
		}
		default:
			throw KernelError("argument " + std::to_string(i + 1) + " is not numeric");
		}
	}
	std::string Text(int i) const override {
		const Datum d = Get(i);
		if (types[i] != TEXTOID && types[i] != VARCHAROID && types[i] != BPCHAROID && types[i] != UNKNOWNOID) {
			throw KernelError("argument " + std::to_string(i + 1) + " is not text");
		}
		const char *p = nullptr;
		uint32_t len = 0;
		PgGuard([&] { TextOf(d, &p, &len); });
		return std::string(p, len);
	}
	int32_t Date(int i) const override {
		const Datum d = Get(i);
		if (types[i] != DATEOID) {
			throw KernelError("argument " + std::to_string(i + 1) + " is not a date");
		}
		return DatumGetDateADT(d) + kPgEpochOffsetDays;
	}
	int64_t Decimal(int i, int scale) const override {
		const Datum d = Get(i);
		switch (types[i]) {
		case INT2OID:
		case INT4OID:
		case INT8OID:
			return Int(i) * kPow10[scale];
		case FLOAT8OID:
		case FLOAT4OID: {
			const double v = Double(i) * static_cast<double>(kPow10[scale]);
			return static_cast<int64_t>(std::llround(v));
		}
		case NUMERICOID: {
			char *text = nullptr;
			PgGuard([&] { text = DatumGetCString(DirectFunctionCall1(numeric_out, d)); });
			return ParseScaledDecimal(text, scale);
		}
		default:
			throw KernelError("argument " + std::to_string(i + 1) + " is not numeric");
		}
	}

private:
	void Check(int i) const {
		if (i < 0 || i >= Count()) {
			throw KernelError("argument index out of range");
		}
	}
	Datum Get(int i) const {
		Check(i);
		if (PG_ARGISNULL(i)) {
			throw KernelError("argument " + std::to_string(i + 1) + " is NULL");
		}
		return PG_GETARG_DATUM(i);
	}
	FunctionCallInfo fcinfo;
	std::vector<Oid> types;
};

std::string DecimalString(__int128 v, int scale) {
	const bool neg = v < 0;
	unsigned __int128 m = neg ? static_cast<unsigned __int128>(-(v + 1)) + 1 : static_cast<unsigned __int128>(v);
	std::string digits;
	do {
		digits.push_back(static_cast<char>('0' + static_cast<int>(m % 10)));
		m /= 10;
	} while (m);
	while (static_cast<int>(digits.size()) <= scale) {
		digits.push_back('0');
	}
	std::reverse(digits.begin(), digits.end());
	if (scale > 0) {
		digits.insert(digits.size() - static_cast<size_t>(scale), ".");
	}
	return neg ? "-" + digits : digits;
}

class WriterImpl final : public ResultWriter {
public:
	WriterImpl(Tuplestorestate *store_p, TupleDesc desc_p) : store(store_p), desc(desc_p) {
		values.resize(desc->natts);
		nulls.resize(desc->natts);
	}
	void Text(std::string_view v) override {
		const Oid t = Next();
		if (t != TEXTOID && t != VARCHAROID && t != BPCHAROID) {
			Mismatch("text");
		}
		PgGuard([&] { values[col] = PointerGetDatum(cstring_to_text_with_len(v.data(), static_cast<int>(v.size()))); });
		Advance();
	}
	void Int(int64_t v) override {
		const Oid t = Next();
		PgGuard([&] {
			switch (t) {
			case INT8OID:
				values[col] = Int64GetDatum(v);
				break;
			case INT4OID:
				if (v < PG_INT32_MIN || v > PG_INT32_MAX) {
					ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("integer out of range")));
				}
				values[col] = Int32GetDatum(static_cast<int32>(v));
				break;
			case NUMERICOID:
				values[col] = NumericGetDatum(int64_to_numeric(v));
				break;
			case FLOAT8OID:
				values[col] = Float8GetDatum(static_cast<double>(v));
				break;
			default:
				ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg("integer result for a non-numeric column")));
			}
		});
		Advance();
	}
	void Double(double v) override {
		const Oid t = Next();
		PgGuard([&] {
			if (t == FLOAT8OID) {
				values[col] = Float8GetDatum(v);
			} else if (t == NUMERICOID) {
				values[col] = DirectFunctionCall1(float8_numeric, Float8GetDatum(v));
			} else {
				ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg("double result for a non-float column")));
			}
		});
		Advance();
	}
	void Decimal(__int128 scaled, int scale) override {
		const Oid t = Next();
		const std::string s = DecimalString(scaled, scale);
		PgGuard([&] {
			if (t == NUMERICOID) {
				values[col] = DirectFunctionCall3(numeric_in, CStringGetDatum(s.c_str()), ObjectIdGetDatum(InvalidOid),
				                                  Int32GetDatum(-1));
			} else if (t == FLOAT8OID) {
				values[col] = DirectFunctionCall1(float8in, CStringGetDatum(s.c_str()));
			} else if ((t == INT8OID || t == INT4OID) && scale == 0) {
				values[col] = t == INT8OID ? Int64GetDatum(static_cast<int64_t>(scaled))
				                           : Int32GetDatum(static_cast<int32>(scaled));
			} else {
				ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg("decimal result for a non-numeric column")));
			}
		});
		Advance();
	}
	void Date(int32_t days) override {
		const Oid t = Next();
		if (t != DATEOID) {
			Mismatch("date");
		}
		values[col] = DateADTGetDatum(days - kPgEpochOffsetDays);
		Advance();
	}
	void Null() override {
		Next();
		nulls[col] = true;
		Advance();
	}
	void EndRow() override {
		if (col != desc->natts) {
			throw KernelError("result row has " + std::to_string(col) + " of " + std::to_string(desc->natts) +
			                  " columns");
		}
		PgGuard([&] { tuplestore_putvalues(store, desc, values.data(), nulls.data()); });
		col = 0;
		std::fill(nulls.begin(), nulls.end(), false);
	}

private:
	Oid Next() {
		if (col >= desc->natts) {
			throw KernelError("too many result columns");
		}
		nulls[col] = false;
		return TupleDescAttr(desc, col)->atttypid;
	}
	void Advance() {
		col++;
	}
	[[noreturn]] void Mismatch(const char *what) {
		throw KernelError(std::string(what) + " result for column " + std::to_string(col + 1) + " of another type");
	}
	Tuplestorestate *store;
	TupleDesc desc;
	std::vector<Datum> values;
	int col = 0;

public:
	// bool[] for tuplestore_putvalues (std::vector<bool> is not an array).
	struct BoolArray {
		std::unique_ptr<bool[]> p;
		size_t n = 0;
		void resize(size_t k) {
			p.reset(new bool[k]());
			n = k;
		}
		bool &operator[](size_t i) {
			return p[i];
		}
		bool *data() {
			return p.get();
		}
		bool *begin() {
			return p.get();
		}
		bool *end() {
			return p.get() + n;
		}
	} nulls;
};

bool ParseFunctionName(const char *name, int &query_id, bool &parallel) {
	static const char kPrefix[] = "accel_tpch_q";
	if (std::strncmp(name, kPrefix, sizeof(kPrefix) - 1) != 0) {
		return false;
	}
	const char *p = name + sizeof(kPrefix) - 1;
	char *end = nullptr;
	const long q = std::strtol(p, &end, 10);
	if (end == p) {
		return false;
	}
	query_id = static_cast<int>(q);
	if (*end == '\0') {
		parallel = false;
		return true;
	}
	if (std::strcmp(end, "_parallel") == 0) {
		parallel = true;
		return true;
	}
	return false;
}

} // namespace
} // namespace accelerator_codegen::pg_base

// ---------------------------------------------------------------------------
// SQL entry points
// ---------------------------------------------------------------------------

extern "C" {

PG_FUNCTION_INFO_V1(accel_base_dispatch);
Datum accel_base_dispatch(PG_FUNCTION_ARGS) {
	using namespace accelerator_codegen::pg_base;
	const char *fname = get_func_name(fcinfo->flinfo->fn_oid);
	int query_id = 0;
	bool parallel = false;
	if (!fname || !ParseFunctionName(fname, query_id, parallel)) {
		ereport(ERROR, (errmsg("accel_base_dispatch: unexpected function name %s", fname ? fname : "?")));
	}
	const KernelEntry *entry = FindKernel(query_id, parallel);
	if (!entry) {
		ereport(ERROR, (errmsg("no base kernel registered for %s", fname)));
	}
	ReturnSetInfo *rsinfo = reinterpret_cast<ReturnSetInfo *>(fcinfo->resultinfo);
	// Single-column RETURNS TABLE functions are scalar SRFs: use the executor's
	// expected descriptor when it supplies one.
	InitMaterializedSRF(fcinfo, rsinfo && rsinfo->expectedDesc ? MAT_SRF_USE_EXPECTED_DESC : 0);

	char message[1024];
	int code = 0;
	message[0] = '\0';
	try {
		ArgsImpl args(fcinfo);
		WriterImpl writer(rsinfo->setResult, rsinfo->setDesc);
		entry->fn(args, writer);
	} catch (const PgError &e) {
		code = e.code;
		snprintf(message, sizeof(message), "%s", e.what());
	} catch (const std::exception &e) {
		code = ERRCODE_INTERNAL_ERROR;
		snprintf(message, sizeof(message), "%s: %s", fname, e.what());
	}
	if (code) {
		ereport(ERROR, (errcode(code), errmsg("%s", message)));
	}
	return (Datum)0;
}

//! accel_base_register(schema text) -> number of functions created.
PG_FUNCTION_INFO_V1(accel_base_register);
Datum accel_base_register(PG_FUNCTION_ARGS) {
	using namespace accelerator_codegen::pg_base;
	const char *schema = text_to_cstring(PG_GETARG_TEXT_PP(0));
	const char *qschema = quote_identifier(schema);
	std::vector<KernelEntry> entries(KernelRegistry().begin(), KernelRegistry().end());
	std::sort(entries.begin(), entries.end(), [](const KernelEntry &a, const KernelEntry &b) {
		return a.query_id != b.query_id ? a.query_id < b.query_id : a.parallel < b.parallel;
	});
	if (SPI_connect() != SPI_OK_CONNECT) {
		ereport(ERROR, (errmsg("SPI_connect failed")));
	}
	std::string sql = std::string("CREATE SCHEMA IF NOT EXISTS ") + qschema;
	SPI_execute(sql.c_str(), false, 0);
	for (const auto &e : entries) {
		const std::string name = std::string(qschema) + ".accel_tpch_q" + std::to_string(e.query_id) +
		                         (e.parallel ? "_parallel" : "");
		sql = "DROP FUNCTION IF EXISTS " + name + "(" + e.sql_args + ") CASCADE";
		if (SPI_execute(sql.c_str(), false, 0) < 0) {
			ereport(ERROR, (errmsg("failed: %s", sql.c_str())));
		}
		sql = "CREATE FUNCTION " + name + "(" + e.sql_args + ") RETURNS " + e.sql_returns +
		      " AS '$libdir/" ACCEL_BASE_PG_LIB "', 'accel_base_dispatch' LANGUAGE C VOLATILE PARALLEL UNSAFE";
		if (SPI_execute(sql.c_str(), false, 0) < 0) {
			ereport(ERROR, (errmsg("failed: %s", sql.c_str())));
		}
	}
	SPI_finish();
	PG_RETURN_INT32(static_cast<int32>(entries.size()));
}

} // extern "C"
