UPDATE partsupp SET ps_availqty = 24 WHERE ps_comment = 'q20-base';
UPDATE lineitem SET l_quantity = 40.00 WHERE l_comment = 'q20-base-C';
DELETE FROM lineitem WHERE l_comment = 'q20-base-B' AND l_suppkey IN (SELECT s_suppkey FROM supplier WHERE s_comment = 'q20-base' AND s_name LIKE 'Q20 CANADA %');
