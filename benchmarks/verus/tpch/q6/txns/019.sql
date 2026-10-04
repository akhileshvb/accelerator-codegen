DELETE FROM lineitem WHERE rowid IN (
    SELECT rowid FROM lineitem WHERE l_shipdate >= DATE '1995-01-01' AND l_shipdate < DATE '1996-01-01'
      AND l_discount BETWEEN 0.03 AND 0.09 AND l_quantity < 24 AND l_comment NOT LIKE 'q6-%'
    ORDER BY l_orderkey, l_linenumber LIMIT 16
);
