-- bulk_insert / lineitem, a second RF chunk (min_chunks = 2 in suite.yaml): the same shape from a
-- different in-window order, so a kernel that only handles the first chunk's key range is caught.
INSERT INTO lineitem
SELECT * REPLACE ((SELECT COALESCE(MAX(l_orderkey), 0) FROM lineitem) + 3003 AS l_orderkey,
                  'Q14RF2' AS l_shipmode, 'Q14RF2' AS l_comment)
FROM lineitem
WHERE l_orderkey = (
    SELECT l_orderkey FROM lineitem
     WHERE l_shipdate >= DATE '1995-06-01' AND l_shipdate < DATE '1995-07-01'
     ORDER BY l_orderkey DESC LIMIT 1)
ORDER BY l_orderkey, l_linenumber;
