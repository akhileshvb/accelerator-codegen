UPDATE nation SET n_name = 'GERMANY' WHERE n_name = 'GERMANY ' AND n_comment <> 'q11-text-variant';
DELETE FROM partsupp WHERE ps_comment LIKE 'q11-edge-%';
INSERT INTO partsupp
SELECT (SELECT max(ps_partkey) FROM partsupp) + batch.offset_key, s_suppkey,
       1, batch.cost, batch.marker
FROM supplier CROSS JOIN (VALUES
    (1, 100.00, 'q11-edge-final-positive'),
    (2, -100.00, 'q11-edge-final-negative'),
    (3, 0.00, 'q11-edge-final-zero')
) AS batch(offset_key, cost, marker)
WHERE s_comment = 'q11-edge-supplier';
UPDATE partsupp SET ps_partkey = 2147483647 WHERE ps_comment = 'q11-edge-final-positive';
UPDATE partsupp SET ps_partkey = -2147483648 WHERE ps_comment = 'q11-edge-final-negative';
DELETE FROM nation WHERE n_comment = 'q11-bag-nation';
DELETE FROM supplier WHERE s_comment IN ('q11-bag-supplier', 'q11-bag-other');
DELETE FROM partsupp WHERE ps_comment = 'q11-bag-stock';
