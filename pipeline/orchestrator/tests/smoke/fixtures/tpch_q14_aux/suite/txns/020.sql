-- bulk_insert / lineitem, RF1-shaped chunk: copy every lineitem of one in-window order under a new
-- orderkey, all inside the window, so all 1-7 rows of that order enter the answer at once (mixed PROMO
-- and non-PROMO parts). l_shipmode is the bookmark 021/022 use.
INSERT INTO lineitem
SELECT * REPLACE ((SELECT COALESCE(MAX(l_orderkey), 0) FROM lineitem) + 2002 AS l_orderkey,
                  'Q14RF1' AS l_shipmode, 'Q14RF1' AS l_comment)
FROM lineitem
WHERE l_orderkey = (
    SELECT l_orderkey FROM lineitem
     WHERE l_shipdate >= DATE '1995-06-01' AND l_shipdate < DATE '1995-07-01'
     ORDER BY l_orderkey LIMIT 1)
ORDER BY l_orderkey, l_linenumber;
