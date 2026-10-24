DELETE FROM lineitem WHERE l_comment = 'q20-base-C' AND l_suppkey IN (SELECT s_suppkey FROM supplier WHERE s_comment = 'q20-base' AND s_name LIKE 'Q20 CANADA %');
