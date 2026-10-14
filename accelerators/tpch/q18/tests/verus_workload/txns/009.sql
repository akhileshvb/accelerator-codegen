UPDATE orders SET o_custkey = (
  SELECT c_custkey FROM customer WHERE c_comment = 'q18:customer:02'
) WHERE o_comment = 'q18:new:order';
