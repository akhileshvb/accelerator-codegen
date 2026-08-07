#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "accelerator_codegen/aux_state/engine/postgres/postgres_kernel_executor.hpp"

#include <functional>
#include <string>
#include <vector>

#include "accelerator_codegen/aux_state/engine/postgres/postgres_kernel_dispatch.hpp"
#include "accelerator_codegen/aux_state/engine/postgres/postgres_row_writer.hpp"

extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/numeric.h"
}

namespace accelerator_codegen::aux_state::engine::postgres {

static void RequireMaterialize(FunctionCallInfo fcinfo) {
  auto *return_set_info = reinterpret_cast<ReturnSetInfo *>(fcinfo->resultinfo);
  if (!return_set_info ||
      !(return_set_info->allowedModes & SFRM_Materialize)) {
    ereport(ERROR,
            (errmsg("accelerator base kernel functions require materialize mode")));
  }
  InitMaterializedSRF(fcinfo, 0);
}

std::string TextDatum(Datum datum) {
  char *cstring = text_to_cstring(
      reinterpret_cast<::text *>(PG_DETOAST_DATUM_PACKED(datum)));
  std::string result(cstring ? cstring : "");
  if (cstring) {
    pfree(cstring);
  }
  return result;
}

Datum StringRowValueToDatum(const std::string &text, Oid type_oid) {
  if (text == "NULL") {
    return (Datum)0;
  }
  if (type_oid == NUMERICOID) {
    return DirectFunctionCall3(numeric_in, CStringGetDatum(text.c_str()),
                               ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
  }
  if (type_oid == FLOAT8OID || type_oid == FLOAT4OID) {
    return Float8GetDatum(std::stod(text));
  }
  if (type_oid == INT8OID || type_oid == INT4OID) {
    return Int64GetDatum(std::stoll(text));
  }
  if (type_oid == DATEOID) {
    return DirectFunctionCall1(date_in, CStringGetDatum(text.c_str()));
  }
  return CStringGetTextDatum(text.c_str());
}

void WriteStringRows(const std::vector<std::vector<std::string>> &rows,
                     FunctionCallInfo fcinfo) {
  auto *return_set_info = reinterpret_cast<ReturnSetInfo *>(fcinfo->resultinfo);
  TupleDesc tuple_desc = return_set_info->setDesc;
  Tuplestorestate *tuplestore = return_set_info->setResult;
  for (const auto &row : rows) {
    std::vector<Datum> values(tuple_desc->natts);
    bool *nulls = static_cast<bool *>(palloc0(tuple_desc->natts * sizeof(bool)));
    for (int column = 0; column < tuple_desc->natts; ++column) {
      if (column >= static_cast<int>(row.size()) || row[column] == "NULL") {
        nulls[column] = true;
        continue;
      }
      values[column] = StringRowValueToDatum(
          row[column], TupleDescAttr(tuple_desc, column)->atttypid);
    }
    tuplestore_putvalues(tuplestore, tuple_desc, values.data(), nulls);
    pfree(nulls);
  }
}

static PostgresRowWriter MakeRowWriter(FunctionCallInfo fcinfo) {
  return [fcinfo](const std::vector<std::vector<std::string>> &rows) {
    WriteStringRows(rows, fcinfo);
  };
}

static void RunChecked(const std::function<void()> &run) {
  try {
    run();
  } catch (const std::exception &ex) {
    ereport(ERROR, (errmsg("%s", ex.what())));
  }
}

void ExecutePostgresBaseTpchQ1(FunctionCallInfo fcinfo) {
  RequireMaterialize(fcinfo);
  RunChecked([&] { RunPostgresBaseTpchQ1(PG_GETARG_INT32(0), MakeRowWriter(fcinfo)); });
}

void ExecutePostgresBaseTpchQ3(FunctionCallInfo fcinfo) {
  RequireMaterialize(fcinfo);
  RunChecked([&] {
    RunPostgresBaseTpchQ3(TextDatum(PG_GETARG_DATUM(0)), PG_GETARG_DATEADT(1),
                          MakeRowWriter(fcinfo));
  });
}

void ExecutePostgresBaseTpchQ6(FunctionCallInfo fcinfo) {
  RequireMaterialize(fcinfo);
  RunChecked([&] {
    RunPostgresBaseTpchQ6(PG_GETARG_DATEADT(0), PG_GETARG_FLOAT8(1), PG_GETARG_FLOAT8(2),
                          MakeRowWriter(fcinfo));
  });
}

void ExecutePostgresBaseTpchQ10(FunctionCallInfo fcinfo) {
  RequireMaterialize(fcinfo);
  RunChecked(
      [&] { RunPostgresBaseTpchQ10(PG_GETARG_DATEADT(0), MakeRowWriter(fcinfo)); });
}

void ExecutePostgresBaseTpchQ12(FunctionCallInfo fcinfo) {
  RequireMaterialize(fcinfo);
  RunChecked([&] {
    RunPostgresBaseTpchQ12(TextDatum(PG_GETARG_DATUM(0)), TextDatum(PG_GETARG_DATUM(1)),
                           PG_GETARG_DATEADT(2), MakeRowWriter(fcinfo));
  });
}

void ExecutePostgresBaseTpchQ14(FunctionCallInfo fcinfo) {
  RequireMaterialize(fcinfo);
  RunChecked(
      [&] { RunPostgresBaseTpchQ14(PG_GETARG_DATEADT(0), MakeRowWriter(fcinfo)); });
}

void ExecutePostgresBaseTpchQ2(FunctionCallInfo fcinfo) {
  RequireMaterialize(fcinfo);
  RunChecked([&] {
    RunPostgresBaseTpchQ2(PG_GETARG_INT32(0), TextDatum(PG_GETARG_DATUM(1)),
                          TextDatum(PG_GETARG_DATUM(2)), MakeRowWriter(fcinfo));
  });
}

void ExecutePostgresBaseTpchQ4(FunctionCallInfo fcinfo) {
  RequireMaterialize(fcinfo);
  RunChecked(
      [&] { RunPostgresBaseTpchQ4(PG_GETARG_DATEADT(0), MakeRowWriter(fcinfo)); });
}

void ExecutePostgresBaseTpchQ5(FunctionCallInfo fcinfo) {
  RequireMaterialize(fcinfo);
  RunChecked([&] {
    RunPostgresBaseTpchQ5(TextDatum(PG_GETARG_DATUM(0)), PG_GETARG_DATEADT(1),
                          MakeRowWriter(fcinfo));
  });
}

void ExecutePostgresBaseTpchQ7(FunctionCallInfo fcinfo) {
  RequireMaterialize(fcinfo);
  RunChecked([&] {
    RunPostgresBaseTpchQ7(TextDatum(PG_GETARG_DATUM(0)), TextDatum(PG_GETARG_DATUM(1)),
                          MakeRowWriter(fcinfo));
  });
}

void ExecutePostgresBaseTpchQ8(FunctionCallInfo fcinfo) {
  RequireMaterialize(fcinfo);
  RunChecked([&] {
    RunPostgresBaseTpchQ8(TextDatum(PG_GETARG_DATUM(0)), TextDatum(PG_GETARG_DATUM(1)),
                          TextDatum(PG_GETARG_DATUM(2)), MakeRowWriter(fcinfo));
  });
}

void ExecutePostgresBaseTpchQ9(FunctionCallInfo fcinfo) {
  RequireMaterialize(fcinfo);
  RunChecked([&] { RunPostgresBaseTpchQ9(TextDatum(PG_GETARG_DATUM(0)), MakeRowWriter(fcinfo)); });
}

void ExecutePostgresBaseTpchQ11(FunctionCallInfo fcinfo) {
  RequireMaterialize(fcinfo);
  RunChecked([&] {
    RunPostgresBaseTpchQ11(TextDatum(PG_GETARG_DATUM(0)), PG_GETARG_FLOAT8(1),
                           MakeRowWriter(fcinfo));
  });
}

void ExecutePostgresBaseTpchQ13(FunctionCallInfo fcinfo) {
  RequireMaterialize(fcinfo);
  RunChecked(
      [&] { RunPostgresBaseTpchQ13(TextDatum(PG_GETARG_DATUM(0)), MakeRowWriter(fcinfo)); });
}

void ExecutePostgresBaseTpchQ15(FunctionCallInfo fcinfo) {
  RequireMaterialize(fcinfo);
  RunChecked(
      [&] { RunPostgresBaseTpchQ15(PG_GETARG_DATEADT(0), MakeRowWriter(fcinfo)); });
}

void ExecutePostgresBaseTpchQ16(FunctionCallInfo fcinfo) {
  RequireMaterialize(fcinfo);
  RunChecked([&] {
    RunPostgresBaseTpchQ16(TextDatum(PG_GETARG_DATUM(0)), TextDatum(PG_GETARG_DATUM(1)),
                           PG_GETARG_INT32(2), PG_GETARG_INT32(3), PG_GETARG_INT32(4),
                           PG_GETARG_INT32(5), PG_GETARG_INT32(6), PG_GETARG_INT32(7),
                           PG_GETARG_INT32(8), PG_GETARG_INT32(9), MakeRowWriter(fcinfo));
  });
}

void ExecutePostgresBaseTpchQ17(FunctionCallInfo fcinfo) {
  RequireMaterialize(fcinfo);
  RunChecked([&] {
    RunPostgresBaseTpchQ17(TextDatum(PG_GETARG_DATUM(0)), TextDatum(PG_GETARG_DATUM(1)),
                           MakeRowWriter(fcinfo));
  });
}

void ExecutePostgresBaseTpchQ18(FunctionCallInfo fcinfo) {
  RequireMaterialize(fcinfo);
  RunChecked([&] {
    RunPostgresBaseTpchQ18(PG_GETARG_INT64(0), MakeRowWriter(fcinfo));
  });
}

void ExecutePostgresBaseTpchQ19(FunctionCallInfo fcinfo) {
  RequireMaterialize(fcinfo);
  RunChecked([&] {
    RunPostgresBaseTpchQ19(TextDatum(PG_GETARG_DATUM(0)), PG_GETARG_INT64(1),
                           TextDatum(PG_GETARG_DATUM(2)), PG_GETARG_INT64(3),
                           TextDatum(PG_GETARG_DATUM(4)), PG_GETARG_INT64(5),
                           MakeRowWriter(fcinfo));
  });
}

void ExecutePostgresBaseTpchQ20(FunctionCallInfo fcinfo) {
  RequireMaterialize(fcinfo);
  RunChecked([&] {
    RunPostgresBaseTpchQ20(PG_GETARG_DATEADT(0), TextDatum(PG_GETARG_DATUM(1)),
                           TextDatum(PG_GETARG_DATUM(2)), MakeRowWriter(fcinfo));
  });
}

void ExecutePostgresBaseTpchQ21(FunctionCallInfo fcinfo) {
  RequireMaterialize(fcinfo);
  RunChecked(
      [&] { RunPostgresBaseTpchQ21(TextDatum(PG_GETARG_DATUM(0)), MakeRowWriter(fcinfo)); });
}

void ExecutePostgresBaseTpchQ22(FunctionCallInfo fcinfo) {
  RequireMaterialize(fcinfo);
  RunChecked([&] {
    RunPostgresBaseTpchQ22(TextDatum(PG_GETARG_DATUM(0)), TextDatum(PG_GETARG_DATUM(1)),
                           TextDatum(PG_GETARG_DATUM(2)), TextDatum(PG_GETARG_DATUM(3)),
                           TextDatum(PG_GETARG_DATUM(4)), TextDatum(PG_GETARG_DATUM(5)),
                           TextDatum(PG_GETARG_DATUM(6)), MakeRowWriter(fcinfo));
  });
}

void ExecutePostgresBaseSecEdgarQ1(FunctionCallInfo fcinfo) {
  RequireMaterialize(fcinfo);
  RunChecked([&] { RunPostgresBaseSecEdgarQ1(MakeRowWriter(fcinfo)); });
}

void ExecutePostgresBaseSecEdgarQ2(FunctionCallInfo fcinfo) {
  RequireMaterialize(fcinfo);
  RunChecked([&] { RunPostgresBaseSecEdgarQ2(MakeRowWriter(fcinfo)); });
}

void ExecutePostgresBaseSecEdgarQ3(FunctionCallInfo fcinfo) {
  RequireMaterialize(fcinfo);
  RunChecked([&] { RunPostgresBaseSecEdgarQ3(MakeRowWriter(fcinfo)); });
}

void ExecutePostgresBaseSecEdgarQ4(FunctionCallInfo fcinfo) {
  RequireMaterialize(fcinfo);
  RunChecked([&] { RunPostgresBaseSecEdgarQ4(MakeRowWriter(fcinfo)); });
}

void ExecutePostgresBaseSecEdgarQ6(FunctionCallInfo fcinfo) {
  RequireMaterialize(fcinfo);
  RunChecked([&] { RunPostgresBaseSecEdgarQ6(MakeRowWriter(fcinfo)); });
}

void ExecutePostgresBaseSecEdgarQ24(FunctionCallInfo fcinfo) {
  RequireMaterialize(fcinfo);
  RunChecked([&] { RunPostgresBaseSecEdgarQ24(MakeRowWriter(fcinfo)); });
}

} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
