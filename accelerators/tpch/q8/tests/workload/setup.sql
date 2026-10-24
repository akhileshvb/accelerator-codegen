INSERT INTO customer
SELECT source.* REPLACE (
  (SELECT max(c_custkey) + 1 FROM customer) AS c_custkey,
  (SELECT n_nationkey FROM nation WHERE n_name = 'BRAZIL' ORDER BY n_nationkey LIMIT 1) AS c_nationkey,
  'q8-customer-a' AS c_comment
) FROM customer source ORDER BY source.c_custkey LIMIT 1;
INSERT INTO customer
SELECT source.* REPLACE (
  (SELECT max(c_custkey) + 1 FROM customer) AS c_custkey,
  (SELECT n_nationkey FROM nation WHERE n_name = 'CANADA' ORDER BY n_nationkey LIMIT 1) AS c_nationkey,
  'q8-customer-b' AS c_comment
) FROM customer source ORDER BY source.c_custkey LIMIT 1;
INSERT INTO supplier
SELECT source.* REPLACE (
  (SELECT max(s_suppkey) + 1 FROM supplier) AS s_suppkey,
  (SELECT n_nationkey FROM nation WHERE n_name = 'BRAZIL' ORDER BY n_nationkey LIMIT 1) AS s_nationkey,
  'q8-supplier-a' AS s_comment
) FROM supplier source ORDER BY source.s_suppkey LIMIT 1;
INSERT INTO supplier
SELECT source.* REPLACE (
  (SELECT max(s_suppkey) + 1 FROM supplier) AS s_suppkey,
  (SELECT n_nationkey FROM nation WHERE n_name = 'CANADA' ORDER BY n_nationkey LIMIT 1) AS s_nationkey,
  'q8-supplier-b' AS s_comment
) FROM supplier source ORDER BY source.s_suppkey LIMIT 1;
INSERT INTO part
SELECT source.* REPLACE (
  (SELECT max(p_partkey) + 1 FROM part) AS p_partkey,
  'LARGE STEEL' AS p_type, 'q8-part-a' AS p_comment
) FROM part source ORDER BY source.p_partkey LIMIT 1;
INSERT INTO part
SELECT source.* REPLACE (
  (SELECT max(p_partkey) + 1 FROM part) AS p_partkey,
  'PROMO STEEL' AS p_type, 'q8-part-b' AS p_comment
) FROM part source ORDER BY source.p_partkey LIMIT 1;
INSERT INTO orders
SELECT source.* REPLACE (
  ((SELECT max(o_orderkey) FROM orders) + batch.ordinal) AS o_orderkey,
  (SELECT c_custkey FROM customer WHERE c_comment = CASE WHEN batch.ordinal % 2 = 1 THEN 'q8-customer-a' ELSE 'q8-customer-b' END) AS o_custkey,
  CASE WHEN batch.ordinal % 2 = 1 THEN DATE '1995-04-12' ELSE DATE '1996-09-15' END AS o_orderdate,
  'O' AS o_orderstatus, 1200.00 AS o_totalprice,
  'q8-order-' || lpad(CAST(batch.ordinal AS VARCHAR), 2, '0') AS o_comment
) FROM (SELECT * FROM orders ORDER BY o_orderkey LIMIT 1) source
CROSS JOIN range(1, 7) batch(ordinal);
INSERT INTO lineitem
SELECT source.* REPLACE (
  target.o_orderkey AS l_orderkey,
  (SELECT p_partkey FROM part WHERE p_comment = CASE WHEN target.o_comment IN ('q8-order-05', 'q8-order-06') THEN 'q8-part-b' ELSE 'q8-part-a' END) AS l_partkey,
  (SELECT s_suppkey FROM supplier WHERE s_comment = CASE WHEN batch.ordinal = 1 THEN 'q8-supplier-a' ELSE 'q8-supplier-b' END) AS l_suppkey,
  batch.ordinal AS l_linenumber, 10.00 AS l_quantity,
  CASE WHEN batch.ordinal = 1 THEN 1000.01 ELSE 3000.03 END AS l_extendedprice,
  CASE WHEN batch.ordinal = 1 THEN 0.03 ELSE 0.07 END AS l_discount,
  'q8-line-' || right(target.o_comment, 2) || '-' || CAST(batch.ordinal AS VARCHAR) AS l_comment
) FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) source
CROSS JOIN (SELECT * FROM orders WHERE o_comment LIKE 'q8-order-%') target
CROSS JOIN range(1, 3) batch(ordinal);
