INSERT INTO supplier
SELECT (SELECT max(s_suppkey) + 1 FROM supplier), 'Q11 New Depot', '13 Inventory Road',
       n_nationkey, '33-111-222-3333', 750.00, 'q11-new-depot'
FROM nation WHERE n_name = 'FRANCE';
INSERT INTO partsupp
SELECT (SELECT max(ps_partkey) FROM partsupp) + batch.offset_key,
       s_suppkey, batch.quantity, batch.cost, 'q11-new-depot-stock'
FROM supplier CROSS JOIN (VALUES (1, 1000, 200.00), (2, 2400, 355.00), (3, 750, 602.50))
AS batch(offset_key, quantity, cost)
WHERE s_comment = 'q11-new-depot';
