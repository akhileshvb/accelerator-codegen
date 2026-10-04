DELETE FROM partsupp WHERE ps_suppkey IN (SELECT s_suppkey FROM supplier WHERE s_name = 'Supplier#V16-09');
DELETE FROM supplier WHERE s_name = 'Supplier#V16-09';
