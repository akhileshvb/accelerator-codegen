// MySQL plugin runtime for the Verus-verified Q21 kernel (mysql_accel_tpch_q21_verus).
// Built only when the Verus Q21 kernel is available (ACCEL_HAVE_Q21_VERUS).
// Moved verbatim from the former shared src/generated/mysql/runtime.cpp.
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN) && defined(ACCEL_HAVE_Q21_VERUS)

#include "mysql/runtime.hpp"

#include "accelerator_codegen/stateful/mysql/mysql_tpch_table_ids.hpp"
#include "tpch/q21/verus/module.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

// Scans for the Verus Q21 kernel.  Each implements the SQL its interface
// comment names (the kernel's trusted inputs).  They run on a separate CLI
// connection, so they see committed data only: the capture contract then
// holds for autocommit writes (maintenance runs after each statement).

class TpchQ21VerusScanImpl final : public query::TpchQ21VerusScan {
public:
  static q21v_li_row Row(const std::vector<std::string> &cols) {
    return q21v_li_row{TypedInt(cols, 0), TypedInt(cols, 1), TypedDate(cols, 2),
                       TypedDate(cols, 3)};
  }
  void ScanSuppliers(
      const std::function<void(int64_t, const std::string &, int32_t)> &consumer)
      override {
    ScanTypedTuples("SELECT s_suppkey, s_name, s_nationkey FROM supplier",
                    [&](const std::vector<std::string> &cols) {
                      consumer(TypedInt(cols, 0), TypedText(cols, 1),
                               static_cast<int32_t>(TypedInt(cols, 2)));
                    });
  }
  void ScanNations(
      const std::function<void(int32_t, const std::string &)> &consumer) override {
    ScanTypedTuples("SELECT n_nationkey, n_name FROM nation",
                    [&](const std::vector<std::string> &cols) {
                      consumer(static_cast<int32_t>(TypedInt(cols, 0)),
                               TypedText(cols, 1));
                    });
  }
  void ScanFOrderKeys(const std::function<void(int64_t)> &consumer) override {
    ScanTypedTuples("SELECT o_orderkey FROM orders WHERE o_orderstatus = 'F'",
                    [&](const std::vector<std::string> &cols) {
                      consumer(TypedInt(cols, 0));
                    });
  }
  void ScanFLineitems(const std::function<void(const q21v_li_row &)> &consumer) override {
    ScanTypedTuples("SELECT l.l_orderkey, l.l_suppkey, l.l_commitdate, l.l_receiptdate "
                    "FROM lineitem l WHERE EXISTS (SELECT 1 FROM orders o "
                    "WHERE o.o_orderkey = l.l_orderkey AND o.o_orderstatus = 'F')",
                    [&](const std::vector<std::string> &cols) { consumer(Row(cols)); });
  }
  void ScanLineitemsForKeys(const std::vector<int64_t> &keys,
                            const std::function<void(const q21v_li_row &)> &consumer)
      override {
    const std::string sql = "SELECT l_orderkey, l_suppkey, l_commitdate, l_receiptdate "
                            "FROM lineitem WHERE " + KeyListPredicate("l_orderkey", keys);
    ScanTypedTuples(sql, [&](const std::vector<std::string> &cols) { consumer(Row(cols)); });
  }
  void RunReference(const std::string &nation,
                    const std::function<void(const std::string &, int64_t)> &consumer)
      override {
    const std::string sql =
        "SELECT s_name, COUNT(*) AS numwait FROM supplier, lineitem l1, orders, "
        "nation WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND "
        "o_orderstatus = 'F' AND l1.l_receiptdate > l1.l_commitdate AND EXISTS ("
        "SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND "
        "l2.l_suppkey <> l1.l_suppkey) AND NOT EXISTS (SELECT * FROM lineitem l3 "
        "WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND "
        "l3.l_receiptdate > l3.l_commitdate) AND s_nationkey = n_nationkey AND "
        "n_name = " + ByteExactLiteral(nation) + " "
        "GROUP BY s_name ORDER BY numwait DESC, s_name LIMIT 100";
    ScanTypedTuples(sql, [&](const std::vector<std::string> &cols) {
      consumer(TypedText(cols, 0), TypedInt(cols, 1));
    });
  }
};

// The kernel's reference specification orders and compares text byte-wise.
// MySQL does exactly that only under utf8mb4_0900_bin (NO PAD, memcmp), so
// the kernel is used only when the three text columns Q21 reads have it.
bool Q21VerusBinaryCollation() {
  return ColumnsCompareBytewise(
      {{"supplier", "s_name"}, {"nation", "n_name"}, {"orders", "o_orderstatus"}});
}

Runtime<query::TpchQ21VerusModule, query::TpchQ21VerusConfig, TpchQ21VerusScanImpl> &
Q21VerusRuntime() {
  static Runtime<query::TpchQ21VerusModule, query::TpchQ21VerusConfig,
                 TpchQ21VerusScanImpl>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q21_COST_MODEL",
              "config/q21_aux_cost_model.json",
              query::TpchQ21VerusLayout::kAccelerator,
              query::TpchQ21VerusConfig{mysql_tbl::kSupplier, mysql_tbl::kOrders,
                                        mysql_tbl::kLineitem, mysql_tbl::kNation});
  return runtime;
}

std::string ExecuteQuery(const core::QueryArguments &arguments) {
  static const bool binary = Q21VerusBinaryCollation();
  std::string out;
  if (!binary) {
    // Outside the kernel's verified domain: answer with the SQL itself.
    std::string nation;
    if (!arguments.values.empty() && !arguments.values[0].is_null &&
        std::holds_alternative<std::string>(arguments.values[0].payload))
      nation = std::get<std::string>(arguments.values[0].payload);
    TsvSink sink(out);
    TpchQ21VerusScanImpl scan;
    core::AuxRow row;
    scan.RunReference(nation, [&](const std::string &name, int64_t numwait) {
      row.values = {core::AuxValue::Of(core::AuxType::String, name),
                    core::AuxValue::Of(core::AuxType::Int64, numwait)};
      sink.WriteRow(row);
    });
    return out;
  }
  Q21VerusRuntime().Execute(arguments, out);
  return out;
}

} // namespace

// n_name = NULL matches no row (argument parsing would turn NULL into '').
static const MysqlUdfRegistrar kRegisterQ21VerusUdf(MysqlUdfEntry{
    "mysql_accel_tpch_q21_verus", "s", &ExecuteQuery, /*null_first_arg_yields_empty=*/true});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
