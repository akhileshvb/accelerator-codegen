UPDATE orders SET o_orderpriority = '4-NOT SPECIFIED'
WHERE o_comment LIKE 'q4:linefirst:%';
