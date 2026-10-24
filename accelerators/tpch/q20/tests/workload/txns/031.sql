INSERT INTO nation SELECT max(n_nationkey) + 1, 'TEST NATION', 1, 'q20-territory' FROM nation;
UPDATE supplier SET s_nationkey = (SELECT n_nationkey FROM nation WHERE n_comment = 'q20-territory') WHERE s_suppkey IN (SELECT s_suppkey FROM supplier WHERE s_comment = 'q20-base' AND s_name LIKE 'Q20 CANADA %' ORDER BY s_suppkey LIMIT 1);
