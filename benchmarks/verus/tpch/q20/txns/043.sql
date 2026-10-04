INSERT INTO nation SELECT * FROM nation WHERE n_name = 'CANADA';
INSERT INTO supplier SELECT * FROM supplier WHERE s_suppkey IN (SELECT s_suppkey FROM supplier WHERE s_comment = 'q20-base' AND s_name LIKE 'Q20 CANADA %' ORDER BY s_suppkey LIMIT 1);
