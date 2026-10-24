UPDATE lineitem SET l_quantity = 23.00
WHERE rowid IN (
    SELECT rowid FROM lineitem
    WHERE l_shipdate >= DATE '1994-01-01' AND l_shipdate < DATE '1995-01-01'
      AND l_discount BETWEEN 0.05 AND 0.07 AND l_quantity = 25
      AND l_comment NOT LIKE 'q6-%'
    ORDER BY l_orderkey, l_linenumber LIMIT 3
);
