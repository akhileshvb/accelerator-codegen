INSERT INTO lineitem
SELECT saved.* FROM test_q15_saved_lines saved
WHERE saved.l_shipdate >= DATE '1996-01-01' AND saved.l_shipdate < DATE '1996-04-01'
  AND NOT EXISTS (SELECT 1 FROM lineitem current_line
    WHERE current_line.l_orderkey = saved.l_orderkey AND current_line.l_linenumber = saved.l_linenumber);
UPDATE lineitem SET l_extendedprice = saved.l_extendedprice, l_discount = saved.l_discount
FROM test_q15_saved_lines saved
WHERE lineitem.l_orderkey = saved.l_orderkey AND lineitem.l_linenumber = saved.l_linenumber
  AND saved.l_shipdate >= DATE '1996-01-01' AND saved.l_shipdate < DATE '1996-04-01';
