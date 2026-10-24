UPDATE part SET p_name = 'gainsboro test ' || ps_suppkey FROM partsupp WHERE p_partkey = ps_partkey AND p_comment = 'q20-base' AND ps_suppkey % 2 = 0;
