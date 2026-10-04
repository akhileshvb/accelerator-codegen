DELETE FROM part WHERE rowid = (SELECT max(rowid) FROM part WHERE p_name = 'Verus catalog 11');
DELETE FROM partsupp WHERE rowid = (
    SELECT max(rowid) FROM partsupp
    WHERE ps_partkey = (SELECT p_partkey FROM part WHERE p_name = 'Verus catalog 11')
      AND ps_suppkey = (SELECT s_suppkey FROM supplier WHERE s_name = 'Supplier#V16-05')
);
