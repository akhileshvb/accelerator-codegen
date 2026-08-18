-- TPC-H indexes for Postgres aux / base SQL benchmarks (SF1+SF20).
-- Q21 especially needs lineitem(l_orderkey) for correlated EXISTS subqueries;
-- without it, SF20 base Q21 does not finish in hours under
-- max_parallel_workers_per_gather=0.

CREATE INDEX IF NOT EXISTS lineitem_l_orderkey_idx ON lineitem (l_orderkey);
CREATE INDEX IF NOT EXISTS lineitem_l_suppkey_idx ON lineitem (l_suppkey);
CREATE INDEX IF NOT EXISTS lineitem_l_partkey_idx ON lineitem (l_partkey);
CREATE INDEX IF NOT EXISTS orders_o_orderkey_idx ON orders (o_orderkey);
CREATE INDEX IF NOT EXISTS orders_o_custkey_idx ON orders (o_custkey);
CREATE INDEX IF NOT EXISTS supplier_s_suppkey_idx ON supplier (s_suppkey);
CREATE INDEX IF NOT EXISTS supplier_s_nationkey_idx ON supplier (s_nationkey);
CREATE INDEX IF NOT EXISTS nation_n_nationkey_idx ON nation (n_nationkey);
CREATE INDEX IF NOT EXISTS customer_c_custkey_idx ON customer (c_custkey);
CREATE INDEX IF NOT EXISTS part_p_partkey_idx ON part (p_partkey);
CREATE INDEX IF NOT EXISTS partsupp_ps_partkey_suppkey_idx
  ON partsupp (ps_partkey, ps_suppkey);

ANALYZE lineitem;
ANALYZE orders;
ANALYZE supplier;
ANALYZE nation;
ANALYZE customer;
ANALYZE part;
ANALYZE partsupp;
