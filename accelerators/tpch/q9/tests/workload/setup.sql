CREATE TABLE q9_test_seed AS
SELECT p.p_partkey, s.s_suppkey, n.n_nationkey, o.o_orderkey, l.l_linenumber
FROM part p
JOIN lineitem l ON l.l_partkey = p.p_partkey
JOIN supplier s ON s.s_suppkey = l.l_suppkey
JOIN partsupp ps ON ps.ps_partkey = l.l_partkey AND ps.ps_suppkey = l.l_suppkey
JOIN orders o ON o.o_orderkey = l.l_orderkey
JOIN nation n ON n.n_nationkey = s.s_nationkey
WHERE p.p_name LIKE '%green%'
ORDER BY o.o_orderkey, l.l_linenumber, p.p_partkey, s.s_suppkey
LIMIT 1;
CREATE TABLE q9_test_part AS SELECT p.* FROM part p JOIN q9_test_seed t ON p.p_partkey = t.p_partkey LIMIT 1;
CREATE TABLE q9_test_supplier AS SELECT s.* FROM supplier s JOIN q9_test_seed t ON s.s_suppkey = t.s_suppkey LIMIT 1;
CREATE TABLE q9_test_nation AS SELECT n.* FROM nation n JOIN q9_test_seed t ON n.n_nationkey = t.n_nationkey LIMIT 1;
CREATE TABLE q9_test_orders AS SELECT o.* FROM orders o JOIN q9_test_seed t ON o.o_orderkey = t.o_orderkey LIMIT 1;
CREATE TABLE q9_test_lineitem AS SELECT l.* FROM lineitem l JOIN q9_test_seed t ON l.l_orderkey = t.o_orderkey AND l.l_linenumber = t.l_linenumber LIMIT 1;
CREATE TABLE q9_test_partsupp AS SELECT ps.* FROM partsupp ps JOIN q9_test_seed t ON ps.ps_partkey = t.p_partkey AND ps.ps_suppkey = t.s_suppkey LIMIT 1;
CREATE TABLE q9_test_keys AS
SELECT (SELECT max(p_partkey) + 100 FROM part) AS pkey,
       (SELECT max(s_suppkey) + 100 FROM supplier) AS skey,
       (SELECT max(n_nationkey) + 100 FROM nation) AS nkey,
       (SELECT max(o_orderkey) + 100 FROM orders) AS okey;
