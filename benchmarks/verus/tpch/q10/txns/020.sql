DELETE FROM lineitem
WHERE l_orderkey = (SELECT o_orderkey FROM orders WHERE o_comment = 'Q10-11') AND l_linenumber = 1;
