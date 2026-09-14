CREATE TABLE region (
  r_regionkey INT NOT NULL,
  r_name VARCHAR(255) NOT NULL,
  r_comment VARCHAR(255) NOT NULL,
  PRIMARY KEY (r_regionkey)
);

CREATE TABLE nation (
  n_nationkey INT NOT NULL,
  n_name VARCHAR(255) NOT NULL,
  n_regionkey INT NOT NULL,
  n_comment VARCHAR(255) NOT NULL,
  PRIMARY KEY (n_nationkey)
);

CREATE TABLE customer (
  c_custkey BIGINT NOT NULL,
  c_name VARCHAR(255) NOT NULL,
  c_address VARCHAR(255) NOT NULL,
  c_nationkey INT NOT NULL,
  c_phone VARCHAR(255) NOT NULL,
  c_acctbal DECIMAL(15,2) NOT NULL,
  c_mktsegment VARCHAR(255) NOT NULL,
  c_comment VARCHAR(255) NOT NULL,
  PRIMARY KEY (c_custkey)
);

CREATE TABLE supplier (
  s_suppkey BIGINT NOT NULL,
  s_name VARCHAR(255) NOT NULL,
  s_address VARCHAR(255) NOT NULL,
  s_nationkey INT NOT NULL,
  s_phone VARCHAR(255) NOT NULL,
  s_acctbal DECIMAL(15,2) NOT NULL,
  s_comment VARCHAR(255) NOT NULL,
  PRIMARY KEY (s_suppkey)
);

CREATE TABLE part (
  p_partkey BIGINT NOT NULL,
  p_name VARCHAR(255) NOT NULL,
  p_mfgr VARCHAR(255) NOT NULL,
  p_brand VARCHAR(255) NOT NULL,
  p_type VARCHAR(255) NOT NULL,
  p_size INT NOT NULL,
  p_container VARCHAR(255) NOT NULL,
  p_retailprice DECIMAL(15,2) NOT NULL,
  p_comment VARCHAR(255) NOT NULL,
  PRIMARY KEY (p_partkey)
);

CREATE TABLE partsupp (
  ps_partkey BIGINT NOT NULL,
  ps_suppkey BIGINT NOT NULL,
  ps_availqty INT NOT NULL,
  ps_supplycost DECIMAL(15,2) NOT NULL,
  ps_comment VARCHAR(255) NOT NULL,
  PRIMARY KEY (ps_partkey, ps_suppkey)
);

CREATE TABLE orders (
  o_orderkey BIGINT NOT NULL,
  o_custkey BIGINT NOT NULL,
  o_orderstatus VARCHAR(255) NOT NULL,
  o_totalprice DECIMAL(15,2) NOT NULL,
  o_orderdate DATE NOT NULL,
  o_orderpriority VARCHAR(255) NOT NULL,
  o_clerk VARCHAR(255) NOT NULL,
  o_shippriority INT NOT NULL,
  o_comment VARCHAR(255) NOT NULL,
  PRIMARY KEY (o_orderkey)
);

CREATE TABLE lineitem (
  l_orderkey BIGINT NOT NULL,
  l_partkey BIGINT NOT NULL,
  l_suppkey BIGINT NOT NULL,
  l_linenumber BIGINT NOT NULL,
  l_quantity DECIMAL(15,2) NOT NULL,
  l_extendedprice DECIMAL(15,2) NOT NULL,
  l_discount DECIMAL(15,2) NOT NULL,
  l_tax DECIMAL(15,2) NOT NULL,
  l_returnflag VARCHAR(255) NOT NULL,
  l_linestatus VARCHAR(255) NOT NULL,
  l_shipdate DATE NOT NULL,
  l_commitdate DATE NOT NULL,
  l_receiptdate DATE NOT NULL,
  l_shipinstruct VARCHAR(255) NOT NULL,
  l_shipmode VARCHAR(255) NOT NULL,
  l_comment VARCHAR(255) NOT NULL,
  PRIMARY KEY (l_orderkey, l_linenumber)
);
