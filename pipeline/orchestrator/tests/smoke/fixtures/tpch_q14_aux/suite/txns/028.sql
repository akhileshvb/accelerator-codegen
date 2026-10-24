-- rollback / lineitem: the same in-window chunk insert as 020, applied and then aborted. The
-- accelerator must see nothing at all: no staged maintenance, no epoch, no publish. A suite whose
-- only negative test is this one still catches a maintenance that applies before commit.
-- rollback
INSERT INTO lineitem
SELECT * REPLACE ((SELECT COALESCE(MAX(l_orderkey), 0) FROM lineitem) + 4004 AS l_orderkey,
                  'Q14RB' AS l_shipmode, 'Q14RB' AS l_comment)
FROM lineitem
WHERE l_shipdate >= DATE '1995-06-01' AND l_shipdate < DATE '1995-07-01'
ORDER BY l_orderkey, l_linenumber LIMIT 4;
