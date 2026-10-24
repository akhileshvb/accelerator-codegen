UPDATE supplier SET s_name = 'Q20 ' || n_name || ' ' || CASE WHEN s_suppkey % 2 = 0 THEN '1' ELSE '2' END FROM nation WHERE s_nationkey = n_nationkey AND s_comment = 'q20-base';
