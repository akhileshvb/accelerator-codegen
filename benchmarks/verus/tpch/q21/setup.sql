-- Adversarial identity and ordering rows.
INSERT INTO nation (n_nationkey, n_name, n_regionkey, n_comment) VALUES
  (1000000000, '', 0, NULL),
  (1000000001, '  WHITESPACE  ', 0, 'spaces are significant except CHAR padding'),
  (1000000002, 'CaseTrap', 0, 'mixed-case nation'),
  (1000000003, 'CASETRAP', 0, 'uppercase nation'),
  (1000000004, 'ÄÖÜ', 0, 'non-ASCII bytes'),
  (-2147483648, 'NEGATIVE NATION', 0, 'minimum key'),
  (1000000002, 'duplicate namespace key', 0, 'same key as CaseTrap'),
  (1100000000, 'LARGE ANSWER NATION', 0, 'more than one hundred groups');

INSERT INTO supplier (s_suppkey, s_name, s_address, s_nationkey, s_phone, s_acctbal, s_comment) VALUES
  (2000000000, 'duplicate name', NULL, 1000000002, '000', 0.00, ''),
  (2000000001, 'duplicate name', 'dup-b', 1000000003, '000', 0.00, ''),
  (2000000002, 'duplicate name', 'dup-c', 1000000002, '000', 0.00, ''),
  (2000000003, 'empty answer supplier', '', 1000000000, '', -999999999999.99, ''),
  (2000000004, 'whitespace  supplier', ' ', 1000000001, ' ', 999999999999.99, NULL),
  (2000000005, 'ÄÖÜ supplier', 'ÄÖÜ', 1000000004, 'ÄÖÜ', 0.00, 'ÄÖÜ'),
  (2000000006, 'negative key', 'neg', -2147483648, '000', 0.00, ''),
  (2000000000, 'duplicate key name', 'dup-key', 1000000002, '000', 0.00, 'same key as another supplier'),
  (2000000007, 'namespace pending supplier', 'pending', 1000000007, '000', 0.00, 'namespace is inserted later'),
  (2000000008, 'isolated peer', 'isolated', 1000000003, '000', 0.00, 'peer is inserted later');

-- Two identical late primary lines per target order, so bag multiplicity matters.
INSERT INTO orders (o_orderkey, o_custkey, o_orderstatus, o_totalprice, o_orderdate, o_orderpriority, o_clerk, o_shippriority, o_comment) VALUES
  (2000000000, 1, 'F', 0.00, DATE '1970-01-01', 'low', 'c', 0, 'entry order'),
  (2000000001, 1, 'F', 0.00, DATE '1970-01-01', 'low', 'c', 0, 'exit order'),
  (2000000002, 1, 'O', 0.00, DATE '1970-01-01', 'low', 'c', 0, 'non-finished order'),
  (2000000003, 1, 'F', 0.00, DATE '1970-01-01', 'low', 'c', 0, 'namespace order'),
  (2000000004, 1, 'F', 0.00, DATE '1970-01-01', 'low', 'c', 0, 'duplicate-name order'),
  (2000000005, 1, 'F', 0.00, DATE '1970-01-01', 'low', 'c', 0, 'boundary order'),
  (2000000006, 1, 'F', 0.00, DATE '1970-01-01', 'low', 'c', 0, 'rescan order'),
  (2000000007, 1, 'F', 0.00, DATE '1970-01-01', 'low', 'c', 0, 'peer arrives in a transaction'),
  (2000000008, 1, 'F', 0.00, DATE '1970-01-01', 'low', 'c', 0, 'namespace arrives in a transaction'),
  (2000000009, 1, 'F', 0.00, DATE '1970-01-01', 'low', 'c', 0, 'not-yet-late neighborhood'),
  (-2147483648, 1, 'F', 0.00, DATE '1970-01-01', 'low', 'c', 0, 'negative-key order'),
  (2000000005, 2, 'F', 0.00, DATE '1970-01-01', 'low', 'c', 0, 'duplicate order key');

INSERT INTO lineitem (l_orderkey, l_partkey, l_suppkey, l_linenumber, l_quantity, l_extendedprice, l_discount, l_tax, l_returnflag, l_linestatus, l_shipdate, l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode, l_comment) VALUES
  (2000000000, 1, 2000000002, 1, 0.00, 0.00, 0.00, 0.00, 'A', 'F', DATE '1970-01-01', DATE '2000-01-01', DATE '2000-01-02', '', '', ''),
  (2000000000, 2, 2000000002, 1, 0.00, 0.00, 0.00, 0.00, 'A', 'F', DATE '1970-01-01', DATE '2000-01-01', DATE '2000-01-02', '', '', 'duplicate primary line'),
  (2000000000, 3, 2000000006, 1, 0.00, 0.00, 0.00, 0.00, 'A', 'F', DATE '1970-01-01', DATE '2000-01-01', DATE '2000-01-01', '', '', 'not-late peer'),
  (2000000001, 1, 2000000002, 1, 0.00, 0.00, 0.00, 0.00, 'A', 'F', DATE '1970-01-01', DATE '2000-01-01', DATE '2000-01-02', '', '', ''),
  (2000000001, 2, 2000000002, 1, 0.00, 0.00, 0.00, 0.00, 'A', 'F', DATE '1970-01-01', DATE '2000-01-01', DATE '2000-01-02', '', '', 'duplicate primary line'),
  (2000000001, 3, 2000000006, 1, 0.00, 0.00, 0.00, 0.00, 'A', 'F', DATE '1970-01-01', DATE '2000-01-01', DATE '2000-01-01', '', '', 'not-late peer'),
  (2000000002, 1, 2000000002, 1, 0.00, 0.00, 0.00, 0.00, 'A', 'F', DATE '1970-01-01', DATE '2000-01-01', DATE '2000-01-02', '', '', ''),
  (2000000002, 2, 2000000006, 1, 0.00, 0.00, 0.00, 0.00, 'A', 'F', DATE '1970-01-01', DATE '2000-01-01', DATE '2000-01-01', '', '', ''),
  (2000000003, 1, 2000000003, 1, 0.00, 0.00, 0.00, 0.00, 'A', 'F', DATE '1970-01-01', DATE '2000-01-01', DATE '2000-01-02', '', '', ''),
  (2000000003, 2, 2000000006, 1, 0.00, 0.00, 0.00, 0.00, 'A', 'F', DATE '1970-01-01', DATE '2000-01-01', DATE '2000-01-01', '', '', ''),
  (2000000004, 1, 2000000000, 1, 0.00, 0.00, 0.00, 0.00, 'A', 'F', DATE '1970-01-01', DATE '2000-01-01', DATE '2000-01-02', '', '', ''),
  (2000000004, 2, 2000000000, 1, 0.00, 0.00, 0.00, 0.00, 'A', 'F', DATE '1970-01-01', DATE '2000-01-01', DATE '2000-01-02', '', '', 'duplicate primary line'),
  (2000000004, 3, 2000000001, 1, 0.00, 0.00, 0.00, 0.00, 'A', 'F', DATE '1970-01-01', DATE '2000-01-01', DATE '2000-01-01', '', '', 'same-name not-late peer'),
  (2000000005, 1, 2000000004, 1, 0.00, 0.00, 0.00, 0.00, 'A', 'F', DATE '1970-01-01', DATE '1970-01-01', DATE '1970-01-02', '', '', 'receipt is one day late'),
  (2000000005, 2, 2000000006, 1, 0.00, 0.00, 0.00, 0.00, 'A', 'F', DATE '1970-01-01', DATE '1970-01-01', DATE '1970-01-01', '', '', 'same-day peer'),
  (2000000006, 1, 2000000005, 1, 0.00, 0.00, 0.00, 0.00, 'A', 'F', DATE '1970-01-01', DATE '1970-01-01', DATE '1970-01-02', '', '', 'rescan primary'),
  (2000000006, 2, 2000000006, 1, 0.00, 0.00, 0.00, 0.00, 'A', 'F', DATE '1970-01-01', DATE '1970-01-01', DATE '1970-01-01', '', '', 'rescan peer'),
  (2000000007, 1, 2000000008, 1, 0.00, 0.00, 0.00, 0.00, 'A', 'F', DATE '1970-01-01', DATE '2000-01-01', DATE '2000-01-02', '', '', 'only late line until transaction'),
  (2000000008, 1, 2000000007, 1, 0.00, 0.00, 0.00, 0.00, 'A', 'F', DATE '1970-01-01', DATE '2000-01-01', DATE '2000-01-02', '', '', NULL),
  (2000000008, 2, 2000000006, 1, 0.00, 0.00, 0.00, 0.00, 'A', 'F', DATE '1970-01-01', DATE '2000-01-01', DATE '2000-01-01', '', '', 'not-late peer'),
  (2000000009, 1, 2000000003, 1, 0.00, 0.00, 0.00, 0.00, 'A', 'F', DATE '1970-01-01', DATE '2000-01-01', DATE '2000-01-01', '', '', 'will become late'),
  (2000000009, 2, 2000000006, 1, 0.00, 0.00, 0.00, 0.00, 'A', 'F', DATE '1970-01-01', DATE '2000-01-01', DATE '2000-01-01', '', '', 'different supplier'),
  (-2147483648, 1, 2000000006, 1, 0.00, 0.00, 0.00, 0.00, 'A', 'F', DATE '1970-01-01', DATE '1970-01-01', DATE '1970-01-02', '', '', ''),
  (-2147483648, 2, 2000000002, 1, 0.00, 0.00, 0.00, 0.00, 'A', 'F', DATE '1970-01-01', DATE '1970-01-01', DATE '1970-01-01', '', '', 'not-late peer');

-- More than LIMIT 100 qualifying supplier-name groups for one namespace.
INSERT INTO supplier (s_suppkey, s_name, s_address, s_nationkey, s_phone, s_acctbal, s_comment)
SELECT 2200000000 + i, 'large supplier ' || lpad(i::VARCHAR, 3, '0'), '', 1100000000, '000', 0.00, NULL
FROM range(120) AS generated(i);

INSERT INTO orders (o_orderkey, o_custkey, o_orderstatus, o_totalprice, o_orderdate, o_orderpriority, o_clerk, o_shippriority, o_comment)
SELECT 2200000000 + i, 1, 'F', 0.00, DATE '1970-01-01', 'low', 'c', 0, NULL
FROM range(120) AS generated(i);

INSERT INTO lineitem (l_orderkey, l_partkey, l_suppkey, l_linenumber, l_quantity, l_extendedprice, l_discount, l_tax, l_returnflag, l_linestatus, l_shipdate, l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode, l_comment)
SELECT 2200000000 + i, 1, 2200000000 + i, 1, 0.00, 0.00, 0.00, 0.00, 'A', 'F', DATE '1970-01-01', DATE '2000-01-01', DATE '2000-01-02', '', '', NULL
FROM range(120) AS generated(i);

INSERT INTO lineitem (l_orderkey, l_partkey, l_suppkey, l_linenumber, l_quantity, l_extendedprice, l_discount, l_tax, l_returnflag, l_linestatus, l_shipdate, l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode, l_comment)
SELECT 2200000000 + i, 2, 2200000099, 1, 0.00, 0.00, 0.00, 0.00, 'A', 'F', DATE '1970-01-01', DATE '2000-01-01', DATE '2000-01-01', '', '', 'shared non-late peer'
FROM range(120) AS generated(i);
