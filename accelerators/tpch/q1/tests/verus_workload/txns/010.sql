UPDATE lineitem
SET l_quantity = l_quantity + 1.25,
    l_extendedprice = l_extendedprice + 75.01,
    l_comment = 'q1-corrected-batch'
WHERE rowid IN (
    SELECT rowid FROM lineitem
    WHERE l_shipdate <= DATE '1998-08-03'
      AND l_comment NOT LIKE 'q1-%'
    ORDER BY l_shipdate, l_orderkey, l_linenumber LIMIT 8
);
