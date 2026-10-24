UPDATE supplier SET s_nationkey=(SELECT min(n_nationkey) FROM nation JOIN region ON n_regionkey=r_regionkey WHERE r_name='EUROPE') WHERE s_comment='q5 new supplier';
UPDATE customer SET c_nationkey=(SELECT s_nationkey FROM supplier WHERE s_comment='q5 new supplier') WHERE c_comment='q5 new customer';
UPDATE orders SET o_orderdate=DATE '1995-09-01' WHERE o_comment='q5 new order';
UPDATE lineitem SET l_extendedprice=5500.00, l_discount=0.07 WHERE l_comment='q5 new line';
