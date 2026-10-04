DELETE FROM nation WHERE rowid = (SELECT rowid FROM nation WHERE n_comment = 'q11-bag-nation' ORDER BY rowid LIMIT 1);
DELETE FROM supplier WHERE rowid = (SELECT rowid FROM supplier WHERE s_comment = 'q11-bag-supplier' ORDER BY rowid LIMIT 1);
DELETE FROM partsupp WHERE rowid = (SELECT rowid FROM partsupp WHERE ps_comment = 'q11-bag-stock' ORDER BY rowid LIMIT 1);
UPDATE supplier SET s_nationkey = (SELECT n_nationkey FROM nation WHERE n_name = 'GERMANY')
WHERE s_comment = 'q11-bag-other';
UPDATE nation SET n_name = 'Q11 SMALL'
WHERE rowid = (SELECT rowid FROM nation WHERE n_comment = 'q11-bag-nation' ORDER BY rowid LIMIT 1);
INSERT INTO partsupp
SELECT ps_partkey - 1, ps_suppkey, 1, 0.00, 'q11-edge-zero'
FROM partsupp WHERE ps_comment = 'q11-edge-negative';
