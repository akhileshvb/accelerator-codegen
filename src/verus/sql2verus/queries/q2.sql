-- TPC-H Q2 (catalog text), :part_size as $1, :region_name as $2, LIMIT 100. The part_type_pattern ('%BRASS') stays a literal: sql2verus takes only a literal LIKE pattern.
SELECT s_acctbal, s_name, n_name, p_partkey, p_mfgr, s_address, s_phone, s_comment
FROM part, supplier, partsupp, nation, region
WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND p_size = $1
  AND p_type LIKE '%BRASS' AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey
  AND r_name = $2
  AND ps_supplycost = (SELECT MIN(ps_supplycost) FROM partsupp, supplier, nation, region
                       WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND s_nationkey = n_nationkey
                         AND n_regionkey = r_regionkey AND r_name = $2)
ORDER BY s_acctbal DESC, n_name, s_name, p_partkey
LIMIT 100
