INSERT INTO supplier
SELECT ps_suppkey, 'Q11 Orphan Activated', '15 Inventory Road',
       (SELECT n_nationkey FROM nation WHERE n_name = 'GERMANY'),
       '49-111-222-3335', 100.00, 'q11-orphan-supplier'
FROM partsupp WHERE ps_comment = 'q11-orphan-stock';
UPDATE partsupp SET ps_availqty = ps_availqty + 2 WHERE ps_comment = 'q11-orphan-stock';
INSERT INTO nation
SELECT s_nationkey, 'Q11 SMALL',
       (SELECT n_regionkey FROM nation WHERE n_comment = 'q11-small-nation'), 'q11-activated-nation'
FROM supplier WHERE s_comment = 'q11-dormant-nation-supplier';
UPDATE partsupp SET ps_supplycost = ps_supplycost + 0.50 WHERE ps_comment = 'q11-dormant-nation-stock';
