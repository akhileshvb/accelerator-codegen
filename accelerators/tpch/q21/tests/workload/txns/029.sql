-- A supplier with late deliveries on finished orders relocates to SAUDI ARABIA and is renamed.
UPDATE supplier
SET s_nationkey = COALESCE((SELECT MIN(n_nationkey) FROM nation WHERE n_name = 'SAUDI ARABIA'), s_nationkey),
    s_name = 'Supplier#relocated'
WHERE s_suppkey = (
  SELECT MIN(l_suppkey) FROM lineitem JOIN orders ON o_orderkey = l_orderkey
  WHERE o_orderstatus = 'F' AND l_receiptdate > l_commitdate
    AND l_suppkey IN (SELECT s_suppkey FROM supplier JOIN nation ON s_nationkey = n_nationkey
                      WHERE n_name <> 'SAUDI ARABIA'));
