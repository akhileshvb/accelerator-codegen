UPDATE lineitem SET l_suppkey=(SELECT s_suppkey FROM supplier WHERE s_comment='q5 fixture supplier' ORDER BY s_nationkey DESC LIMIT 1), l_comment='q5 rerouted line'
WHERE l_orderkey=(SELECT min(o_orderkey) FROM orders WHERE o_comment='q5 fixture order') AND l_linenumber=1;
