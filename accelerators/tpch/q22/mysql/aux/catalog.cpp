// MySQL query catalog entry for TPC-H Q22, registered at load time.
#include "backends/mysql/api/catalog/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

const MysqlQueryCatalogRegistrar kRegisterTpchQ22(MysqlSharedQueryCatalogEntry
    {
        22,
        "tpch",
        "tpch_q22",
        /* base_sql */
        "WITH custsale AS ("
        "SELECT SUBSTRING(c_phone, 1, 2) AS cntrycode, c_acctbal "
        "FROM customer "
        "WHERE SUBSTRING(c_phone, 1, 2) IN ('30', '19', '14', '28', '11', '22', '32') "
        "AND c_acctbal > (SELECT AVG(c_acctbal) FROM customer WHERE c_acctbal > 0.00 "
        "AND SUBSTRING(c_phone, 1, 2) IN ('30', '19', '14', '28', '11', '22', '32')) "
        "AND NOT EXISTS (SELECT * FROM orders WHERE o_custkey = c_custkey)) "
        "SELECT cntrycode, COUNT(*) AS numcust, SUM(c_acctbal) AS totacctbal "
        "FROM custsale GROUP BY cntrycode ORDER BY cntrycode",
        /* accelerator_sql */
        "SELECT mysql_accel_tpch_q22_aux('30', '19', '14', '28', '11', '22', '32')",
        /* aux_sql */
        "SELECT mysql_accel_tpch_q22_aux('30', '19', '14', '28', '11', '22', '32')",
        /* fallback_sql */
        "SELECT cntrycode, COUNT(*) AS numcust, SUM(c_acctbal) AS totacctbal "
        "FROM (SELECT SUBSTRING(c_phone, 1, 2) AS cntrycode, c_acctbal "
        "FROM customer "
        "WHERE SUBSTRING(c_phone, 1, 2) IN ($1, $2, $3, $4, $5, $6, $7) "
        "AND c_acctbal > (SELECT AVG(c_acctbal) FROM customer WHERE c_acctbal > 0.00 "
        "AND SUBSTRING(c_phone, 1, 2) IN ($1, $2, $3, $4, $5, $6, $7)) "
        "AND NOT EXISTS (SELECT * FROM orders WHERE o_custkey = c_custkey)) AS custsale "
        "GROUP BY cntrycode ORDER BY cntrycode"
    });

} // namespace
} // namespace accelerator_codegen::aux_state::engine::mysql
