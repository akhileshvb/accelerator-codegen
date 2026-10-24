-- key_move / part: move every row of a PROMO key that has in-window lineitems to a fresh key. The old
-- key disappears from the join, so its lineitems leave the answer; the new key joins nothing yet.
UPDATE part SET p_partkey = (SELECT COALESCE(MAX(p_partkey), 0) + 1 FROM part)
WHERE p_partkey = (
    SELECT q.p_partkey FROM part q JOIN lineitem l ON l.l_partkey = q.p_partkey
     WHERE l.l_shipdate >= DATE '1995-06-01' AND l.l_shipdate < DATE '1995-07-01'
       AND q.p_type LIKE 'PROMO%'
     ORDER BY q.p_partkey DESC LIMIT 1);
