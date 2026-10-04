UPDATE orders SET o_orderpriority = '2-HIGH'
WHERE o_comment = 'q12_priority_cycle';
UPDATE orders SET o_orderpriority = '1-URGENT'
WHERE o_comment = 'q12_new_b';
