UPDATE lineitem
SET l_partkey = (SELECT p_partkey FROM part WHERE p_comment = 'v14-promo'),
    l_extendedprice = l_extendedprice + 0.01
WHERE rowid = (
    SELECT lineitem.rowid FROM lineitem JOIN part ON l_partkey = p_partkey
    WHERE l_shipdate >= DATE '1994-06-01' AND l_shipdate < DATE '1994-07-01'
      AND p_type NOT LIKE 'PROMO%' AND l_comment NOT LIKE 'v14-%'
    ORDER BY l_orderkey, l_linenumber LIMIT 1
);
