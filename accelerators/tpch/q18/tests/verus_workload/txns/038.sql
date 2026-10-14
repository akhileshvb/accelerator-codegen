UPDATE lineitem SET l_orderkey = (
  SELECT o_orderkey FROM orders WHERE o_comment = 'q18:order:05'
) WHERE l_comment = 'q18:line:05';
UPDATE orders SET o_custkey = (
  SELECT c_custkey FROM customer WHERE c_comment = 'q18:customer:05'
) WHERE o_comment IN ('q18:order:05', 'q18:batch:05', 'q18:replacement:05');
