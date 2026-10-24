UPDATE lineitem SET l_shipdate = DATE '1995-07-01'
WHERE rowid IN (
    SELECT lineitem.rowid FROM lineitem JOIN part ON l_partkey = p_partkey
    WHERE l_shipdate >= DATE '1995-06-01' AND l_shipdate < DATE '1995-07-01'
      AND l_comment NOT LIKE 'v14-%'
    ORDER BY l_orderkey DESC, l_linenumber LIMIT 12
);
