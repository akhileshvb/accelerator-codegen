DELETE FROM lineitem WHERE l_suppkey IN (SELECT s_suppkey FROM supplier WHERE s_comment = 'q15-new-supplier');
DELETE FROM supplier WHERE s_comment = 'q15-new-supplier';
DELETE FROM orders WHERE o_comment = 'q15-new-supplier-order';
