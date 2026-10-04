UPDATE partsupp SET ps_supplycost = CASE
WHEN ps_suppkey = (SELECT -supp_base FROM q2_test_keys) THEN 12.00 ELSE 9.00 END
WHERE ps_partkey = (SELECT part_base + 125 FROM q2_test_keys)
AND ps_suppkey IN ((SELECT -supp_base FROM q2_test_keys), (SELECT supp_base + 1 FROM q2_test_keys));
