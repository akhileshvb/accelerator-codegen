INSERT INTO part
SELECT v.k, v.nm, p.p_mfgr, '', '', v.sz, p.p_container, 9999999999999.99, 'v16 integer boundary'
FROM (SELECT * FROM part ORDER BY p_partkey LIMIT 1) p
CROSS JOIN (VALUES
    (-2147483648, 'Verus extreme low', -2147483648),
    (2147483647, 'Verus extreme high', 2147483647)
) v(k, nm, sz);
INSERT INTO partsupp VALUES
    (-2147483648, 2147483647, 2147483647, 0.00, 'v16 extreme edge'),
    (2147483647, -2147483648, -2147483648, -9999999999999.99, 'v16 extreme edge');
INSERT INTO supplier
SELECT 2147483647, 'Supplier#V16-extreme', s_address, s_nationkey, s_phone,
       -9999999999999.99, 'CustomerComplaints'
FROM supplier ORDER BY s_suppkey LIMIT 1;
