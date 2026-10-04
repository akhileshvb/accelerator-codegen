INSERT INTO nation SELECT * FROM nation WHERE n_comment = 'q11-bag-nation';
INSERT INTO supplier SELECT * FROM supplier WHERE s_comment = 'q11-bag-supplier';
INSERT INTO partsupp SELECT * FROM partsupp WHERE ps_comment = 'q11-bag-stock';
INSERT INTO supplier
SELECT s_suppkey, 'Q11 Bag Other Nation', s_address,
       (SELECT n_nationkey FROM nation WHERE n_name = 'FRANCE'), s_phone, s_acctbal, 'q11-bag-other'
FROM supplier WHERE s_comment = 'q11-bag-supplier' LIMIT 1;
UPDATE partsupp SET ps_availqty = ps_availqty + 1 WHERE ps_comment = 'q11-bag-stock';
DELETE FROM partsupp WHERE ps_comment = 'q11-edge-zero';
