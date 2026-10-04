DELETE FROM customer WHERE c_comment='q5 duplicate customer';
DELETE FROM supplier WHERE s_comment='q5 duplicate supplier';
DELETE FROM orders WHERE o_comment='q5 duplicate order';
DELETE FROM lineitem WHERE l_comment='q5 duplicate line';
DELETE FROM lineitem WHERE rowid IN (
    SELECT rowid FROM lineitem
    WHERE l_comment='q5 fixture line' AND l_linenumber=1
      AND l_orderkey=(SELECT min(o_orderkey) FROM orders
          JOIN customer ON o_custkey=c_custkey
          JOIN nation ON c_nationkey=n_nationkey
          WHERE o_comment='q5 fixture order' AND c_comment='q5 fixture customer'
            AND n_name='INDIA')
    ORDER BY rowid DESC LIMIT 1
);
