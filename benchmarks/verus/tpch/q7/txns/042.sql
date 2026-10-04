INSERT INTO lineitem
SELECT (SELECT max(o_orderkey)+1 FROM orders), l_partkey, (SELECT s_suppkey FROM supplier WHERE s_comment='q7 supplier A' ORDER BY s_suppkey LIMIT 1), 1,
       l_quantity, 432.10, 0.04, l_tax, l_returnflag, l_linestatus,
       DATE '1995-12-31', l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode, 'q7 orphan'
FROM lineitem WHERE l_comment='q7 live 1';

UPDATE lineitem SET l_extendedprice=l_extendedprice+0.02 WHERE l_comment='q7 live 2';
