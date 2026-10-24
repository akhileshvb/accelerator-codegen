UPDATE lineitem SET l_shipdate=DATE '1996-02-29', l_extendedprice=l_extendedprice+10.01, l_discount=0.02 WHERE l_comment IN ('q7 C first 1','q7 C second 1');

UPDATE supplier SET s_nationkey=(SELECT n_nationkey FROM nation WHERE n_name='GERMANY' ORDER BY n_nationkey LIMIT 1) WHERE s_comment='q7 supplier C';

UPDATE customer SET c_nationkey=(SELECT n_nationkey FROM nation WHERE n_name='FRANCE' ORDER BY n_nationkey LIMIT 1) WHERE c_comment='q7 customer C';

UPDATE orders SET o_totalprice=o_totalprice+10.01, o_orderstatus='F' WHERE o_comment IN ('q7 C first','q7 C second');
