UPDATE lineitem SET l_receiptdate = l_commitdate + 1
WHERE l_comment IN ('q4:seed:11', 'q4:seed:12') AND l_linenumber = 2;
UPDATE orders
SET o_orderdate = CASE WHEN o_comment = 'q4:seed:11' THEN DATE '1993-08-10' ELSE DATE '1993-08-14' END,
    o_orderpriority = CASE WHEN o_comment = 'q4:seed:11' THEN '1-URGENT' ELSE '2-HIGH' END
WHERE o_comment IN ('q4:seed:11', 'q4:seed:12');
