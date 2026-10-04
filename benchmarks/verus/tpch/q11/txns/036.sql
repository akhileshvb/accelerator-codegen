DELETE FROM supplier WHERE s_comment = 'q11-orphan-supplier';
UPDATE partsupp SET ps_supplycost = ps_supplycost + 5.00 WHERE ps_comment = 'q11-orphan-stock';
INSERT INTO supplier
SELECT ps_suppkey, 'Q11 Orphan Activated', '15 Inventory Road',
       (SELECT n_nationkey FROM nation WHERE n_name = 'FRANCE'),
       '33-111-222-3335', 100.00, 'q11-orphan-supplier'
FROM partsupp WHERE ps_comment = 'q11-orphan-stock';
DELETE FROM nation WHERE n_comment = 'q11-activated-nation';
UPDATE partsupp SET ps_availqty = ps_availqty + 3 WHERE ps_comment = 'q11-dormant-nation-stock';
