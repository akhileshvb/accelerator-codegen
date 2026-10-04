INSERT INTO part
SELECT template.* REPLACE (
    (SELECT MAX(p_partkey) FROM part) + sequence.fixture AS p_partkey,
    'q17-fixture-' || LPAD(CAST(sequence.fixture AS VARCHAR), 2, '0') AS p_name,
    catalog.brand AS p_brand, catalog.container AS p_container,
    sequence.fixture AS p_size, 'q17 workload fixture' AS p_comment)
FROM (SELECT * FROM part ORDER BY p_partkey LIMIT 1) AS template
CROSS JOIN range(1, 33) AS sequence(fixture)
JOIN (VALUES
    (0, 'Brand#23', 'WRAP BOX'),
    (1, 'Brand#25', 'MED DRUM'),
    (2, 'Brand#43', 'LG CAN'),
    (3, 'Brand#11', 'SM BOX'),
    (4, 'Brand#12', 'MED BOX'),
    (5, 'Brand#21', 'LG DRUM'),
    (6, 'Brand#32', 'SM BAG'),
    (7, 'Brand#33', 'LG BOX'),
    (8, 'Brand#42', 'MED DRUM'),
    (9, 'Brand#45', 'LG BOX'),
    (10, 'Brand#51', 'MED DRUM'),
    (11, 'Brand#55', 'WRAP JAR')
) AS catalog(ordinal, brand, container) ON (sequence.fixture - 1) % 12 = catalog.ordinal;

INSERT INTO lineitem
SELECT template.* REPLACE (
    (SELECT MAX(l_orderkey) FROM lineitem) + part.p_size AS l_orderkey,
    part.p_partkey AS l_partkey, quantities.ordinal AS l_linenumber,
    quantities.quantity AS l_quantity,
    CAST(100 + part.p_size * 25 + quantities.ordinal * 7 AS DECIMAL(15,2)) AS l_extendedprice,
    'q17-base-' || LPAD(CAST(part.p_size AS VARCHAR), 2, '0') AS l_comment)
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS template
CROSS JOIN part
CROSS JOIN (VALUES (1, 1.00), (2, 3.00), (3, 10.00), (4, 20.00), (5, 40.00)) AS quantities(ordinal, quantity)
WHERE part.p_name LIKE 'q17-fixture-%';
