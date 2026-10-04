DELETE FROM partsupp WHERE ps_comment = 'q11-small-2';
INSERT INTO partsupp
SELECT (SELECT max(ps_partkey) + 1 FROM partsupp), s_suppkey, 4, 60.00, 'q11-small-2'
FROM supplier WHERE s_comment = 'q11-small-supplier';
