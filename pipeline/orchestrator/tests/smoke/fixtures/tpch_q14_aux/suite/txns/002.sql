-- point_update / part: flip a non-PROMO part row that has in-window lineitems to PROMO. The joined row
-- count does not change, so only the numerator moves: a weight-only dimension update. 004 removes the
-- row again, so the key ends without it.
UPDATE part SET p_type = 'PROMO Q14UPD', p_comment = 'Q14UPD'
WHERE p_partkey = (
    SELECT q.p_partkey FROM part q JOIN lineitem l ON l.l_partkey = q.p_partkey
     WHERE l.l_shipdate >= DATE '1995-06-01' AND l.l_shipdate < DATE '1995-07-01'
       AND NOT (q.p_type LIKE 'PROMO%')
     ORDER BY q.p_partkey LIMIT 1)
  AND NOT (p_type LIKE 'PROMO%');
