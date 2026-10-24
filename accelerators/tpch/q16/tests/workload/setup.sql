INSERT INTO supplier
SELECT (SELECT max(s_suppkey) FROM supplier) + v.n,
       'Supplier#V16-' || lpad(CAST(v.n AS VARCHAR), 2, '0'),
       s.s_address, s.s_nationkey, s.s_phone, s.s_acctbal,
       CASE WHEN v.n = 7 THEN 'v16 Customer account Complaints open'
            ELSE 'v16 supplier approved' END
FROM (SELECT * FROM supplier ORDER BY s_suppkey LIMIT 1) s
CROSS JOIN range(1, 9) v(n);

INSERT INTO part
SELECT (SELECT max(p_partkey) FROM part) + v.n,
       'Verus catalog ' || lpad(CAST(v.n AS VARCHAR), 2, '0'),
       p.p_mfgr, v.brand, v.typ, v.sz, p.p_container, p.p_retailprice,
       'v16 catalog'
FROM (SELECT * FROM part ORDER BY p_partkey LIMIT 1) p
CROSS JOIN (VALUES
    (1, 'Brand#11', 'STANDARD POLISHED TIN', 3),
    (2, 'Brand#12', 'SMALL ANODIZED STEEL', 9),
    (3, 'Brand#13', 'MEDIUM POLISHED COPPER', 14),
    (4, 'Brand#14', 'MEDIUM POLISHED TIN', 19),
    (5, 'Brand#21', 'LARGE PLATED BRASS', 23),
    (6, 'Brand#22', 'ECONOMY ANODIZED TIN', 36),
    (7, 'Brand#23', 'PROMO POLISHED STEEL', 45),
    (8, 'Brand#24', 'LARGE BRUSHED COPPER', 49),
    (9, 'Brand#31', 'STANDARD POLISHED TIN', 1),
    (10, 'Brand#32', 'STANDARD POLISHED TIN', 50),
    (11, 'Brand#41', 'PROMO ANODIZED TIN', 14),
    (12, 'Brand#42', 'PROMO ANODIZED TIN', 14)
) v(n, brand, typ, sz);

INSERT INTO partsupp
SELECT p.p_partkey, s.s_suppkey, 100, 25.00, 'v16 catalog edge'
FROM part p CROSS JOIN supplier s
WHERE p.p_name BETWEEN 'Verus catalog 01' AND 'Verus catalog 10'
  AND s.s_name BETWEEN 'Supplier#V16-01' AND 'Supplier#V16-04';

INSERT INTO partsupp
SELECT p.p_partkey, s.s_suppkey, 20, 30.00, 'v16 singleton edge'
FROM part p JOIN supplier s ON
    (p.p_name = 'Verus catalog 11' AND s.s_name = 'Supplier#V16-05') OR
    (p.p_name = 'Verus catalog 12' AND s.s_name = 'Supplier#V16-06');

INSERT INTO partsupp
SELECT p.p_partkey, s.s_suppkey, 50, 18.00, 'v16 blocked edge'
FROM part p CROSS JOIN supplier s
WHERE p.p_name BETWEEN 'Verus catalog 01' AND 'Verus catalog 04'
  AND s.s_name = 'Supplier#V16-07';
