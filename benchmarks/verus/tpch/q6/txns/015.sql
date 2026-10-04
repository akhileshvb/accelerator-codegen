UPDATE lineitem SET l_discount = 0.09, l_shipdate = DATE '1996-04-15'
WHERE l_comment LIKE 'q6-batch-order-%' AND l_linenumber = 3;
UPDATE lineitem SET l_discount = 0.02, l_shipdate = DATE '1993-11-15'
WHERE l_comment LIKE 'q6-batch-order-%' AND l_linenumber = 4;
