// PostgreSQL scan + Runtime for SEC EDGAR Q1 (module only).
//
// The state, its maintenance, the build (q1::Builder) and the result emission
// are the engine-neutral code in sec_edgar/q1/common/, shared with DuckDB. This
// file supplies what is Postgres-specific: the initial scan, which reads `pre`
// through the table access method (no SQL), the Runtime binding and the
// argument handling.
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Include the module header before PostgreSQL headers (macro collisions).
#include "sec_edgar/q1/common/module.hpp"
#include "sec_edgar/q1/layout.hpp"

#include <stdexcept>
#include <string>

#include "postgres/engine_runtime.hpp"
#include "postgres/runtime.hpp"

extern "C" {
#include "access/table.h"
#include "access/tableam.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "utils/rel.h"
}

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

// Table id the capture trigger passes for `pre` (scripts/*sec_edgar*mutation_triggers.sql).
constexpr core::TableId kPreTableId = 3;

// pre.{stmt, rfile, adsh, line}: the compact mutation row, in this order.
constexpr const char *kPreColumns[4] = {"stmt", "rfile", "adsh", "line"};

// 1-based attribute numbers of kPreColumns in pre; false when the table or a
// column is missing or has an unsupported type (text columns must be
// varchar/text: bpchar comparison semantics differ; line must be integer).
bool ResolvePreColumns(Oid &relid, int (&attno)[4], std::string &error) {
  relid = RelnameGetRelid("pre");
  if (!OidIsValid(relid)) {
    error = "relation \"pre\" not found";
    return false;
  }
  for (int i = 0; i < 4; ++i) {
    attno[i] = get_attnum(relid, kPreColumns[i]);
    if (attno[i] == InvalidAttrNumber) {
      error = std::string("column pre.") + kPreColumns[i] + " not found";
      return false;
    }
    const Oid type = get_atttype(relid, attno[i]);
    const bool ok = i < 3 ? (type == VARCHAROID || type == TEXTOID) : type == INT4OID;
    if (!ok) {
      error = std::string("column pre.") + kPreColumns[i] + " has an unsupported type";
      return false;
    }
  }
  return true;
}

// Streams pre through the table access method under the active snapshot.
// fn(stmt, stmt_len, stmt_null, rfile, rfile_len, rfile_null, adsh, adsh_len,
//    adsh_null, line, line_null); the pointers are valid for the call only.
template <typename Fn>
void ScanPreNative(Fn &&fn) {
  Oid relid = InvalidOid;
  int attno[4];
  std::string error;
  if (!ResolvePreColumns(relid, attno, error)) {
    throw std::runtime_error("sec_edgar q1: " + error);
  }
  ++t_table_scans;
  Relation rel = table_open(relid, AccessShareLock);
  TableScanDesc scan = nullptr;
  TupleTableSlot *slot = nullptr;
  try {
    scan = table_beginscan(rel, GetActiveSnapshot(), 0, nullptr);
    slot = table_slot_create(rel, nullptr);
    int last = 0;
    for (int a : attno) {
      last = a > last ? a : last;
    }
    // A text value as (data, length); detoasts only if it is not plain inline.
    struct Text {
      const char *data = nullptr;
      std::size_t len = 0;
      varlena *copy = nullptr;
    };
    auto text_of = [&](int i, Text &t) {
      t.copy = nullptr;
      t.data = nullptr;
      t.len = 0;
      if (slot->tts_isnull[attno[i] - 1]) {
        return true;
      }
      varlena *orig = reinterpret_cast<varlena *>(DatumGetPointer(slot->tts_values[attno[i] - 1]));
      varlena *packed = pg_detoast_datum_packed(orig);
      if (packed != orig) {
        t.copy = packed;
      }
      t.data = VARDATA_ANY(packed);
      t.len = VARSIZE_ANY_EXHDR(packed);
      return false;
    };
    while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
      CHECK_FOR_INTERRUPTS();
      slot_getsomeattrs(slot, last);
      Text stmt, rfile, adsh;
      const bool stmt_null = text_of(0, stmt);
      const bool rfile_null = text_of(1, rfile);
      const bool adsh_null = text_of(2, adsh);
      const bool line_null = slot->tts_isnull[attno[3] - 1];
      const int32_t line = line_null ? 0 : DatumGetInt32(slot->tts_values[attno[3] - 1]);
      fn(stmt.data, stmt.len, stmt_null, rfile.data, rfile.len, rfile_null, adsh.data, adsh.len, adsh_null, line,
         line_null);
      if (stmt.copy) pfree(stmt.copy);
      if (rfile.copy) pfree(rfile.copy);
      if (adsh.copy) pfree(adsh.copy);
    }
  } catch (...) {
    if (slot) ExecDropSingleTupleTableSlot(slot);
    if (scan) table_endscan(scan);
    table_close(rel, AccessShareLock);
    throw;
  }
  ExecDropSingleTupleTableSlot(slot);
  table_endscan(scan);
  table_close(rel, AccessShareLock);
}

class SecEdgarQ1Scan final : public query::SecEdgarQ1InitialScan {
public:
  void ScanPre(const std::function<void(const query::SecEdgarQ1PreInput &)> &consumer) const override {
    ScanPreNative([&](const char *stmt, std::size_t stmt_len, bool stmt_null, const char *rfile,
                      std::size_t rfile_len, bool rfile_null, const char *adsh, std::size_t adsh_len,
                      bool adsh_null, int32_t line, bool line_null) {
      query::SecEdgarQ1PreInput input;
      input.stmt_is_null = stmt_null;
      if (!stmt_null) input.stmt.assign(stmt, stmt_len);
      input.rfile_is_null = rfile_null;
      if (!rfile_null) input.rfile.assign(rfile, rfile_len);
      input.adsh_is_null = adsh_null;
      if (!adsh_null) input.adsh.assign(adsh, adsh_len);
      input.line_is_null = line_null;
      input.line = line;
      consumer(input);
    });
  }
  bool ScanPreInto(query::q1::Builder &builder) const override {
    ScanPreNative([&](const char *stmt, std::size_t stmt_len, bool stmt_null, const char *rfile,
                      std::size_t rfile_len, bool rfile_null, const char *adsh, std::size_t adsh_len,
                      bool adsh_null, int32_t line, bool line_null) {
      builder.ConsumeRowView(stmt, stmt_len, stmt_null, rfile, rfile_len, rfile_null, adsh, adsh_len, adsh_null,
                             line, line_null);
    });
    return true;
  }
};

using Q1Runtime = Runtime<query::SecEdgarQ1Module, query::SecEdgarQ1Config, SecEdgarQ1Scan>;

// Created on first use. The capture attnums are resolved by column name here
// (never ereport inside a static initialiser).
Q1Runtime &SecEdgarQ1RuntimeInstance(const int (&attno)[4]) {
  static Q1Runtime *runtime = nullptr;
  if (!runtime) {
    query::SecEdgarQ1Config config;
    config.pre_table = kPreTableId;
    config.numeric_avg = true;
    for (int a : attno) {
      config.pre_attnums.push_back(a - 1);
    }
    runtime = new Q1Runtime("ACCELERATOR_CODEGEN_POSTGRES_SEC_EDGAR_Q1_COST_MODEL",
                            "config/sec_edgar_q1_aux_cost_model.json",
                            core::SecEdgarQ1Layout::kAccelerator, std::move(config));
  }
  return *runtime;
}

// The capture trigger projects pre by the attnums resolved at first use; a
// later ALTER TABLE that moves a column would make captured rows wrong.
int g_attno[4] = {0, 0, 0, 0};

} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresSecEdgarQ1(FunctionCallInfo fcinfo) {
  Oid relid = InvalidOid;
  int attno[4];
  std::string error;
  if (!detail::ResolvePreColumns(relid, attno, error)) {
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("accel_sec_edgar_q1_aux: %s", error.c_str())));
  }
  if (detail::g_attno[0] == 0) {
    for (int i = 0; i < 4; ++i) {
      detail::g_attno[i] = attno[i];
    }
  }
  for (int i = 0; i < 4; ++i) {
    if (detail::g_attno[i] != attno[i]) {
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("accel_sec_edgar_q1_aux: table \"pre\" changed shape since the accelerator was "
                             "created in this session; reconnect")));
    }
  }
  detail::SecEdgarQ1RuntimeInstance(detail::g_attno).Execute(core::QueryArguments{}, fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(
    PostgresQueryExecutor{"sec_edgar", 1, kPostgresVariantAux, &ExecutePostgresSecEdgarQ1});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
