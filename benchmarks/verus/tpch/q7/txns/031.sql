UPDATE lineitem SET l_shipdate=DATE '1996-02-29', l_extendedprice=l_extendedprice+25.25 WHERE l_comment IN ('q7 window 1','q7 window 2');

UPDATE orders SET o_custkey=(SELECT c_custkey FROM customer WHERE c_comment='q7 customer B' ORDER BY c_custkey LIMIT 1) WHERE o_comment='q7 window';
