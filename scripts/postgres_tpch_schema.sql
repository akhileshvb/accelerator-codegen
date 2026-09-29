CREATE TABLE region (
  r_regionkey integer NOT NULL,
  r_name character varying COLLATE "C" NOT NULL,
  r_comment character varying COLLATE "C" NOT NULL
);

CREATE TABLE nation (
  n_nationkey integer NOT NULL,
  n_name character varying COLLATE "C" NOT NULL,
  n_regionkey integer NOT NULL,
  n_comment character varying COLLATE "C" NOT NULL
);

CREATE TABLE customer (
  c_custkey bigint NOT NULL,
  c_name character varying COLLATE "C" NOT NULL,
  c_address character varying COLLATE "C" NOT NULL,
  c_nationkey integer NOT NULL,
  c_phone character varying COLLATE "C" NOT NULL,
  c_acctbal numeric(15,2) NOT NULL,
  c_mktsegment character varying COLLATE "C" NOT NULL,
  c_comment character varying COLLATE "C" NOT NULL
);

CREATE TABLE supplier (
  s_suppkey bigint NOT NULL,
  s_name character varying COLLATE "C" NOT NULL,
  s_address character varying COLLATE "C" NOT NULL,
  s_nationkey integer NOT NULL,
  s_phone character varying COLLATE "C" NOT NULL,
  s_acctbal numeric(15,2) NOT NULL,
  s_comment character varying COLLATE "C" NOT NULL
);

CREATE TABLE part (
  p_partkey bigint NOT NULL,
  p_name character varying COLLATE "C" NOT NULL,
  p_mfgr character varying COLLATE "C" NOT NULL,
  p_brand character varying COLLATE "C" NOT NULL,
  p_type character varying COLLATE "C" NOT NULL,
  p_size integer NOT NULL,
  p_container character varying COLLATE "C" NOT NULL,
  p_retailprice numeric(15,2) NOT NULL,
  p_comment character varying COLLATE "C" NOT NULL
);

CREATE TABLE partsupp (
  ps_partkey bigint NOT NULL,
  ps_suppkey bigint NOT NULL,
  ps_availqty integer NOT NULL,
  ps_supplycost numeric(15,2) NOT NULL,
  ps_comment character varying COLLATE "C" NOT NULL
);

CREATE TABLE orders (
  o_orderkey bigint NOT NULL,
  o_custkey bigint NOT NULL,
  o_orderstatus character varying COLLATE "C" NOT NULL,
  o_totalprice numeric(15,2) NOT NULL,
  o_orderdate date NOT NULL,
  o_orderpriority character varying COLLATE "C" NOT NULL,
  o_clerk character varying COLLATE "C" NOT NULL,
  o_shippriority integer NOT NULL,
  o_comment character varying COLLATE "C" NOT NULL
);

CREATE TABLE lineitem (
  l_orderkey bigint NOT NULL,
  l_partkey bigint NOT NULL,
  l_suppkey bigint NOT NULL,
  l_linenumber bigint NOT NULL,
  l_quantity numeric(15,2) NOT NULL,
  l_extendedprice numeric(15,2) NOT NULL,
  l_discount numeric(15,2) NOT NULL,
  l_tax numeric(15,2) NOT NULL,
  l_returnflag character varying COLLATE "C" NOT NULL,
  l_linestatus character varying COLLATE "C" NOT NULL,
  l_shipdate date NOT NULL,
  l_commitdate date NOT NULL,
  l_receiptdate date NOT NULL,
  l_shipinstruct character varying COLLATE "C" NOT NULL,
  l_shipmode character varying COLLATE "C" NOT NULL,
  l_comment character varying COLLATE "C" NOT NULL
);
