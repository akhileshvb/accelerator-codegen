-- key_move / lineitem on real data: move one joined in-window line's l_shipdate (a grouping attribute
-- for the daily summaries, and the filter attribute) to a date past the window, so it leaves the
-- answer. l_comment is the bookmark 012 uses to find it again; the query reads no comment column.
UPDATE lineitem SET l_shipdate = DATE '1995-08-15', l_comment = 'Q14M1'
WHERE (l_orderkey, l_linenumber) IN (
    SELECT l.l_orderkey, l.l_linenumber FROM lineitem l JOIN part p ON p.p_partkey = l.l_partkey
     WHERE l.l_shipdate >= DATE '1995-06-01' AND l.l_shipdate < DATE '1995-07-01'
     ORDER BY l.l_orderkey, l.l_linenumber LIMIT 1);
