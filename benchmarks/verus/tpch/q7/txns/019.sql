UPDATE orders SET o_custkey=(SELECT c_custkey FROM customer WHERE c_comment='q7 customer B' ORDER BY c_custkey LIMIT 1), o_orderstatus='F' WHERE o_comment LIKE 'q7 batch %';
