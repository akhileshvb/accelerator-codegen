-- Non-unique TPC-H indexes for Postgres (safe when load has duplicate keys, e.g. non-classic SF20).
-- Use instead of postgres_tpch_indexes.sql when PRIMARY KEY creation fails.

CREATE INDEX IF NOT EXISTS idx_region_regionkey ON region (r_regionkey);
CREATE INDEX IF NOT EXISTS idx_nation_nationkey ON nation (n_nationkey);
CREATE INDEX IF NOT EXISTS idx_nation_regionkey ON nation (n_regionkey);
CREATE INDEX IF NOT EXISTS idx_customer_custkey ON customer (c_custkey);
CREATE INDEX IF NOT EXISTS idx_customer_nationkey ON customer (c_nationkey);
CREATE INDEX IF NOT EXISTS idx_customer_mktsegment ON customer (c_mktsegment);
CREATE INDEX IF NOT EXISTS idx_supplier_suppkey ON supplier (s_suppkey);
CREATE INDEX IF NOT EXISTS idx_supplier_nationkey ON supplier (s_nationkey);
CREATE INDEX IF NOT EXISTS idx_part_partkey ON part (p_partkey);
CREATE INDEX IF NOT EXISTS idx_part_name ON part (p_name);
CREATE INDEX IF NOT EXISTS idx_partsupp_partkey ON partsupp (ps_partkey);
CREATE INDEX IF NOT EXISTS idx_partsupp_suppkey ON partsupp (ps_suppkey);
CREATE INDEX IF NOT EXISTS idx_partsupp_part_supp ON partsupp (ps_partkey, ps_suppkey);
CREATE INDEX IF NOT EXISTS idx_orders_orderkey ON orders (o_orderkey);
CREATE INDEX IF NOT EXISTS idx_orders_custkey ON orders (o_custkey);
CREATE INDEX IF NOT EXISTS idx_orders_orderdate ON orders (o_orderdate);
CREATE INDEX IF NOT EXISTS idx_lineitem_orderkey ON lineitem (l_orderkey);
CREATE INDEX IF NOT EXISTS idx_lineitem_partkey ON lineitem (l_partkey);
CREATE INDEX IF NOT EXISTS idx_lineitem_suppkey ON lineitem (l_suppkey);
CREATE INDEX IF NOT EXISTS idx_lineitem_part_supp ON lineitem (l_partkey, l_suppkey);
CREATE INDEX IF NOT EXISTS idx_lineitem_shipdate ON lineitem (l_shipdate);

ANALYZE;
