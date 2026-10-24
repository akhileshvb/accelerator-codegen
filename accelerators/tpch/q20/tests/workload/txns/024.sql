DELETE FROM lineitem WHERE l_comment = 'q20-alternate';
DELETE FROM partsupp WHERE ps_comment = 'q20-alternate';
DELETE FROM part WHERE p_comment = 'q20-alternate';
UPDATE partsupp SET ps_availqty = 21 WHERE ps_comment = 'q20-base' AND ps_suppkey IN (SELECT s_suppkey FROM supplier WHERE s_comment = 'q20-base' AND s_name LIKE 'Q20 CANADA %');
