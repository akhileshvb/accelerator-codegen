UPDATE lineitem SET l_suppkey = (SELECT s_suppkey FROM supplier WHERE s_comment = 'q15-new-supplier')
WHERE rowid = (SELECT rowid FROM lineitem WHERE l_comment <> 'q15-new-supplier-line'
  AND l_shipdate >= DATE '1996-01-01' AND l_shipdate < DATE '1996-04-01'
  ORDER BY l_extendedprice * (1 - l_discount) DESC, l_orderkey, l_linenumber LIMIT 1);
