-- Regression (with 053, 055): see 053
UPDATE supplier SET s_name = 'Q20 tied é', s_address = CASE WHEN s_suppkey % 2 = 0 THEN 'Address A' ELSE 'Address B ' END WHERE s_suppkey IN (SELECT s_suppkey FROM supplier WHERE s_comment = 'q20-base' AND s_name LIKE 'Q20 CANADA %');
UPDATE nation SET n_name = 'CANADA ' WHERE n_name = 'CANADA';
UPDATE part SET p_name = ' gainsboro test ' || ps_suppkey FROM partsupp WHERE p_partkey = ps_partkey AND ps_suppkey IN (SELECT s_suppkey FROM supplier WHERE s_comment = 'q20-base' AND s_name LIKE 'Q20 CANADA %' ORDER BY s_suppkey LIMIT 1);
