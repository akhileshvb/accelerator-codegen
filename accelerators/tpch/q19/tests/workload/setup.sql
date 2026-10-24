INSERT INTO part
SELECT template.* REPLACE (
    key_base + slot AS p_partkey,
    'q19-stock-' || lpad(CAST(slot AS VARCHAR), 2, '0') AS p_name,
    brand AS p_brand,
    size AS p_size,
    container AS p_container,
    'q19 fixture' AS p_comment
)
FROM (SELECT * FROM part ORDER BY p_partkey LIMIT 1) AS template
CROSS JOIN (SELECT max(p_partkey) AS key_base FROM part) AS base
CROSS JOIN (VALUES
    (1, 'Brand#44', 1, 'SM CASE'),
    (2, 'Brand#44', 2, 'SM BOX'),
    (3, 'Brand#44', 4, 'SM PACK'),
    (4, 'Brand#44', 5, 'SM PKG'),
    (5, 'Brand#13', 1, 'MED BAG'),
    (6, 'Brand#13', 5, 'MED BOX'),
    (7, 'Brand#13', 9, 'MED PKG'),
    (8, 'Brand#13', 10, 'MED PACK'),
    (9, 'Brand#24', 1, 'LG CASE'),
    (10, 'Brand#24', 5, 'LG BOX'),
    (11, 'Brand#24', 14, 'LG PACK'),
    (12, 'Brand#24', 15, 'LG PKG')
) AS stock(slot, brand, size, container);

INSERT INTO orders
SELECT template.* REPLACE (
    key_base + row_number() OVER (ORDER BY p_name) AS o_orderkey,
    'O' AS o_orderstatus,
    10000.00 AS o_totalprice,
    DATE '1996-03-01' AS o_orderdate,
    'q19-initial-' || right(p_name, 2) AS o_comment
)
FROM (SELECT * FROM orders ORDER BY o_orderkey LIMIT 1) AS template
CROSS JOIN (SELECT max(o_orderkey) AS key_base FROM orders) AS base
CROSS JOIN (SELECT p_name FROM part WHERE p_comment = 'q19 fixture') AS stock;

INSERT INTO lineitem
SELECT template.* REPLACE (
    o_orderkey AS l_orderkey,
    p_partkey AS l_partkey,
    line_number AS l_linenumber,
    CAST(CASE WHEN p_container LIKE 'SM %' THEN 1
              WHEN p_container LIKE 'MED %' THEN 10 ELSE 20 END
         + quantity_offset AS DECIMAL(15,2)) AS l_quantity,
    CAST((CASE WHEN p_container LIKE 'SM %' THEN 1
               WHEN p_container LIKE 'MED %' THEN 10 ELSE 20 END
          + quantity_offset) * 125.37 AS DECIMAL(15,2)) AS l_extendedprice,
    0.06 AS l_discount,
    DATE '1996-03-10' AS l_shipdate,
    DATE '1996-03-09' AS l_commitdate,
    DATE '1996-03-12' AS l_receiptdate,
    'DELIVER IN PERSON' AS l_shipinstruct,
    CASE WHEN line_number % 2 = 0 THEN 'AIR REG' ELSE 'AIR' END AS l_shipmode,
    'q19-line-' || right(p_name, 2) AS l_comment
)
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS template
CROSS JOIN (SELECT p_partkey, p_name, p_container FROM part WHERE p_comment = 'q19 fixture') AS stock
JOIN orders ON o_comment = 'q19-initial-' || right(p_name, 2)
CROSS JOIN (VALUES (1, 0.00), (2, 5.00), (3, 10.00), (4, -0.01), (5, 10.01)) AS quantities(line_number, quantity_offset);
