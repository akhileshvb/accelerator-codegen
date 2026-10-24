INSERT INTO nation
SELECT (SELECT max(n_nationkey) + 1 FROM nation), 'Q11 CAFÉ', n_regionkey, 'q11-new-market'
FROM nation WHERE n_name = 'FRANCE';
INSERT INTO supplier
SELECT (SELECT max(s_suppkey) + 1 FROM supplier), 'Q11 Market Depot', '14 Inventory Road',
       n_nationkey, '33-111-222-3334', 350.00, 'q11-market-depot'
FROM nation WHERE n_comment = 'q11-new-market';
INSERT INTO partsupp
SELECT (SELECT max(ps_partkey) FROM partsupp) + batch.offset_key,
       s_suppkey, batch.quantity, batch.cost, 'q11-market-stock'
FROM supplier CROSS JOIN (VALUES (1, 50, 70.25), (2, 125, 45.50)) AS batch(offset_key, quantity, cost)
WHERE s_comment = 'q11-market-depot';
