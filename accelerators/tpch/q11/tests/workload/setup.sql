INSERT INTO nation
SELECT (SELECT max(n_nationkey) + 1 FROM nation), 'Q11 SMALL', n_regionkey, 'q11-small-nation'
FROM nation WHERE n_name = 'GERMANY' LIMIT 1;
INSERT INTO nation
SELECT (SELECT max(n_nationkey) + 1 FROM nation), 'Q11 EDGE', n_regionkey, 'q11-edge-nation'
FROM nation WHERE n_name = 'GERMANY' LIMIT 1;
INSERT INTO nation
SELECT (SELECT max(n_nationkey) + 1 FROM nation), 'Q11 BAG', n_regionkey, 'q11-bag-nation'
FROM nation WHERE n_name = 'GERMANY' LIMIT 1;
INSERT INTO nation SELECT * FROM nation WHERE n_comment = 'q11-bag-nation';
INSERT INTO nation
SELECT n_nationkey, variants.name, n_regionkey, 'q11-text-variant'
FROM nation CROSS JOIN (VALUES ('germany'), ('GERMANY '), (' GERMANY')) AS variants(name)
WHERE n_name = 'GERMANY';
INSERT INTO supplier
SELECT (SELECT max(s_suppkey) FROM supplier) + new_supplier.offset_key,
       new_supplier.name, '11 Inventory Road', n.n_nationkey,
       '49-111-222-3333', 500.00, new_supplier.marker
FROM (VALUES
    (1, 'Q11 Depot A', 'GERMANY', 'q11-depot-a'),
    (2, 'Q11 Depot B', 'FRANCE', 'q11-depot-b'),
    (3, 'Q11 Small Supplier', 'Q11 SMALL', 'q11-small-supplier'),
    (4, 'Q11 Edge Supplier', 'Q11 EDGE', 'q11-edge-supplier'),
    (5, 'Q11 Bag Supplier', 'Q11 BAG', 'q11-bag-supplier')
) AS new_supplier(offset_key, name, nation_name, marker)
JOIN (SELECT DISTINCT n_nationkey, n_name FROM nation) n ON n.n_name = new_supplier.nation_name;
INSERT INTO supplier SELECT * FROM supplier WHERE s_comment = 'q11-bag-supplier';
INSERT INTO partsupp
SELECT (SELECT max(ps_partkey) FROM partsupp) + stock.offset_key,
       s.s_suppkey, stock.quantity, stock.cost, stock.marker
FROM (VALUES
    (1, 'Q11 Depot A', 1500, 425.00, 'q11-stock-a1'),
    (2, 'Q11 Depot A', 800, 752.50, 'q11-stock-a2'),
    (1, 'Q11 Depot B', 1200, 410.00, 'q11-stock-b1'),
    (3, 'Q11 Depot B', 600, 900.00, 'q11-stock-b2'),
    (4, 'Q11 Small Supplier', 6, 30.00, 'q11-small-1'),
    (5, 'Q11 Small Supplier', 4, 60.00, 'q11-small-2'),
    (6, 'Q11 Edge Supplier', 1, 9996.99, 'q11-edge-large'),
    (7, 'Q11 Edge Supplier', 1, 0.99, 'q11-edge-below'),
    (8, 'Q11 Edge Supplier', 1, 1.00, 'q11-edge-equal'),
    (9, 'Q11 Edge Supplier', 1, 1.01, 'q11-edge-above'),
    (10, 'Q11 Edge Supplier', 1, 0.00, 'q11-edge-zero'),
    (11, 'Q11 Edge Supplier', 1, -2.00, 'q11-edge-negative'),
    (12, 'Q11 Edge Supplier', 1, 2.00, 'q11-edge-positive'),
    (13, 'Q11 Bag Supplier', 4, 25.00, 'q11-bag-stock')
) AS stock(offset_key, supplier_name, quantity, cost, marker)
JOIN (SELECT DISTINCT s_suppkey, s_name FROM supplier) s ON s.s_name = stock.supplier_name;
INSERT INTO partsupp SELECT * FROM partsupp WHERE ps_comment = 'q11-bag-stock';
INSERT INTO supplier
SELECT (SELECT max(s_suppkey) + 1 FROM supplier), 'Q11 Dormant Nation',
       '12 Inventory Road', (SELECT max(n_nationkey) + 1 FROM nation),
       '49-111-222-3334', 200.00, 'q11-dormant-nation-supplier';
INSERT INTO partsupp
SELECT (SELECT max(ps_partkey) + 1 FROM partsupp), s_suppkey, 5, 50.00, 'q11-dormant-nation-stock'
FROM supplier WHERE s_comment = 'q11-dormant-nation-supplier';
INSERT INTO partsupp
SELECT max(ps_partkey) + 1, (SELECT max(s_suppkey) + 1 FROM supplier), 700, 600.00, 'q11-orphan-stock'
FROM partsupp;
