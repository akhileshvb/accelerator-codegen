INSERT INTO supplier
SELECT template.* REPLACE (
  CAST((SELECT MAX(s_suppkey) FROM supplier) + fixture.offset_key AS INTEGER) AS s_suppkey,
  'Revenue Twin' AS s_name, 'Boundary Avenue' AS s_address,
  '13-111-222-3333' AS s_phone, fixture.marker AS s_comment)
FROM (SELECT * FROM supplier ORDER BY s_suppkey LIMIT 1) template
CROSS JOIN (VALUES (1, 'q15-edge-a'), (2, 'q15-edge-b')) fixture(offset_key, marker);
INSERT INTO lineitem
SELECT template.* REPLACE (
  CAST((SELECT MAX(l_orderkey) FROM lineitem) + fixture.line_no AS INTEGER) AS l_orderkey,
  target.s_suppkey AS l_suppkey, 1 AS l_linenumber, 1.00 AS l_quantity,
  CAST(fixture.price AS DECIMAL(15,2)) AS l_extendedprice,
  CAST(fixture.discount AS DECIMAL(15,2)) AS l_discount,
  fixture.shipdate AS l_shipdate, fixture.shipdate AS l_commitdate,
  fixture.shipdate + 2 AS l_receiptdate, fixture.line_marker AS l_comment)
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) template
CROSS JOIN (VALUES
  (1, 'q15-edge-a', DATE '2000-01-01', 100.00, 0.10, 'q15-edge-a-main'),
  (2, 'q15-edge-b', DATE '2000-03-31', 100.00, 0.10, 'q15-edge-b-main'),
  (3, 'q15-edge-a', DATE '1999-12-31', 500.00, 0.00, 'q15-edge-before'),
  (4, 'q15-edge-b', DATE '2000-04-01', 500.00, 0.00, 'q15-edge-end'),
  (5, 'q15-edge-a', DATE '2000-01-02', 1.00, 1.00, 'q15-edge-zero'))
  fixture(line_no, supplier_marker, shipdate, price, discount, line_marker)
JOIN supplier target ON target.s_comment = fixture.supplier_marker;
