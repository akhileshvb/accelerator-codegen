-- Archive the ordinary order with its lines.
DELETE FROM lineitem WHERE l_orderkey IN (SELECT o_orderkey FROM orders WHERE o_comment = 'q21:ordinary:order');
DELETE FROM orders WHERE o_comment = 'q21:ordinary:order';
