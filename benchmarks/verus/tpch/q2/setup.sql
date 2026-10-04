CREATE TABLE q2_test_keys AS
SELECT (SELECT max(p_partkey) + 1000 FROM part) AS part_base,
       (SELECT max(s_suppkey) + 1000 FROM supplier) AS supp_base,
       (SELECT max(n_nationkey) + 1000 FROM nation) AS nation_base,
       (SELECT max(r_regionkey) + 1000 FROM region) AS region_base;
CREATE TABLE q2_test_region AS
SELECT region_base::INTEGER AS r_regionkey, 'EUROPE' AS r_name,
       'Catalog distribution region' AS r_comment FROM q2_test_keys;
CREATE TABLE q2_test_nation AS
SELECT nation_base::INTEGER AS n_nationkey, 'GERMANY' AS n_name,
       region_base::INTEGER AS n_regionkey, 'Catalog distribution nation' AS n_comment
FROM q2_test_keys;
CREATE TABLE q2_test_suppliers AS
SELECT (supp_base + offset_id)::INTEGER AS s_suppkey,
       CASE offset_id WHEN 0 THEN 'Catalog Supplier A' ELSE 'Catalog Supplier B' END AS s_name,
       '17 Market Street' AS s_address, nation_base::INTEGER AS s_nationkey,
       '49-123-456-7890' AS s_phone,
       CAST(9000 - offset_id * 500 AS DECIMAL(15,2)) AS s_acctbal,
       'Standard catalog supplier' AS s_comment
FROM q2_test_keys CROSS JOIN range(2) AS offsets(offset_id);
CREATE TABLE q2_test_parts AS
SELECT (part_base + offset_id)::INTEGER AS p_partkey, 'brass catalog fitting' AS p_name,
       'Manufacturer#1' AS p_mfgr, 'Brand#11' AS p_brand, 'STANDARD BRASS' AS p_type,
       CASE WHEN offset_id = 125 THEN 10 ELSE 15 END::INTEGER AS p_size,
       'SM BOX' AS p_container, CAST(100.00 AS DECIMAL(15,2)) AS p_retailprice,
       'catalog fitting' AS p_comment
FROM q2_test_keys CROSS JOIN range(126) AS offsets(offset_id);
INSERT INTO region SELECT * FROM q2_test_region;
INSERT INTO nation SELECT * FROM q2_test_nation;
INSERT INTO supplier SELECT * FROM q2_test_suppliers;
INSERT INTO part SELECT * FROM q2_test_parts;
INSERT INTO partsupp
SELECT p_partkey, s_suppkey, 100, CAST(40 + (s_suppkey - supp_base) * 15 AS DECIMAL(15,2)),
       'Regular catalog stock'
FROM q2_test_parts CROSS JOIN q2_test_suppliers CROSS JOIN q2_test_keys;
CREATE VIEW q2_test_winners AS
SELECT p_partkey, p_size, s_suppkey, s_acctbal, n_nationkey, r_regionkey, r_name, ps_supplycost
FROM part JOIN partsupp ON p_partkey = ps_partkey
JOIN supplier ON s_suppkey = ps_suppkey
JOIN nation ON n_nationkey = s_nationkey
JOIN region ON r_regionkey = n_regionkey
WHERE p_type LIKE '%BRASS'
QUALIFY ps_supplycost = min(ps_supplycost) OVER (PARTITION BY p_partkey, r_name);
