UPDATE supplier SET s_comment = 'customer Complaints' WHERE s_name = 'Supplier#V16-extreme';
INSERT INTO part
SELECT p.p_partkey, 'Verus text ' || CAST(v.n AS VARCHAR), p.p_mfgr,
       v.brand, v.typ, 14, p.p_container, p.p_retailprice, 'v16 byte collation'
FROM part p CROSS JOIN (VALUES
    (1, 'brand#45', 'small brushed tin'),
    (2, 'Brand#45 ', 'SMALL BRUSHED'),
    (3, ' Brand#45', 'SMALL BRUSHEDX'),
    (4, 'Bränd#45', ' SMALL BRUSHED'),
    (5, 'Brand#45', 'SMALL BRUSHE'),
    (6, 'Brand#45', 'SMALL  BRUSHED'),
    (7, 'Brand#45 ', 'SMALL BRUSHE'),
    (8, 'Brand#45', 'SMALL BRUSHE ')
) v(n, brand, typ)
WHERE p.p_name = 'Verus duplicate 11';
UPDATE supplier SET s_comment = 'Customer complaints' WHERE s_name = 'Supplier#V16-07';
