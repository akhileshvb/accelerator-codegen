SELECT
  s_acctbal, s_name, n_name, p_partkey, p_mfgr,
  s_address, s_phone, s_comment
FROM accel_tpch_q2_matview
WHERE p_size = 10
  AND type_pattern = 'BRASS'
  AND region_name = 'AFRICA'
ORDER BY s_acctbal DESC, n_name, s_name, p_partkey
