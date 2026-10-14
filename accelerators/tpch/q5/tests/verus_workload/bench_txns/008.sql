UPDATE lineitem SET l_extendedprice=1200.00, l_discount=0.03
WHERE rowid=(SELECT rowid FROM lineitem WHERE l_comment='q5 fixture line' ORDER BY l_orderkey, l_linenumber LIMIT 1);
