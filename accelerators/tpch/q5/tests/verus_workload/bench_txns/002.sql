UPDATE lineitem SET l_discount=0.04
WHERE rowid=(SELECT rowid FROM lineitem WHERE l_comment='q5 fixture line' ORDER BY l_orderkey, l_linenumber LIMIT 1);
