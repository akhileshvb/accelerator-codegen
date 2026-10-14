UPDATE lineitem SET l_extendedprice=l_extendedprice+12.50
WHERE rowid=(SELECT rowid FROM lineitem WHERE l_comment='q5 fixture line' ORDER BY l_orderkey, l_linenumber LIMIT 1);
