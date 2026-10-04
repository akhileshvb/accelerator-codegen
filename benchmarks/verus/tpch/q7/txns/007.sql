UPDATE orders SET o_custkey=(SELECT c_custkey FROM customer WHERE c_comment='q7 customer A' ORDER BY c_custkey LIMIT 1) WHERE o_comment='q7 live';
