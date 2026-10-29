-- Regression (with 054, 055): back to one nation row named CANADA, no big-key or duplicate supplier rows and consistent supplier names, then 043's duplicate nation row and supplier row; 055 joins every CANADA supplier to the two nation rows
DELETE FROM supplier WHERE s_suppkey >= 2147483648;
DELETE FROM supplier WHERE rowid NOT IN (SELECT min(rowid) FROM supplier GROUP BY s_suppkey);
DELETE FROM nation WHERE n_nationkey = 3 AND rowid > (SELECT min(rowid) FROM nation WHERE n_nationkey = 3);
UPDATE nation SET n_name = 'CANADA' WHERE n_nationkey = 3;
UPDATE supplier SET s_name = 'Q20 ' || n_name || ' ' || CASE WHEN s_suppkey % 2 = 0 THEN '1' ELSE '2' END FROM nation WHERE s_nationkey = n_nationkey AND s_comment = 'q20-base';
INSERT INTO nation SELECT * FROM nation WHERE n_name = 'CANADA';
INSERT INTO supplier SELECT * FROM supplier WHERE s_suppkey IN (SELECT s_suppkey FROM supplier WHERE s_comment = 'q20-base' AND s_name LIKE 'Q20 CANADA %' ORDER BY s_suppkey LIMIT 1);
