-- key_move / lineitem: move the scratch row's (001) l_partkey, a join key, off its PROMO part onto a
-- part key that is inside the window but not PROMO, so its contribution leaves the numerator. 007
-- moves it back.
UPDATE lineitem SET l_partkey =
    (SELECT q.p_partkey FROM part q JOIN lineitem l ON l.l_partkey = q.p_partkey
      WHERE l.l_shipdate >= DATE '1995-06-01' AND l.l_shipdate < DATE '1995-07-01'
        AND NOT (q.p_type LIKE 'PROMO%')
      ORDER BY q.p_partkey LIMIT 1)
WHERE l_comment = 'Q14S1';
