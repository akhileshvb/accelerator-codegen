DELETE FROM partsupp WHERE ps_comment = 'q20-base' AND ps_suppkey IN (SELECT s_suppkey FROM supplier WHERE s_comment = 'q20-base' AND s_name LIKE 'Q20 CANADA %');
