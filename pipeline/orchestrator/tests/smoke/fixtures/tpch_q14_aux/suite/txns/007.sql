-- sequence (undo of 006): the join key back to the PROMO part 001 joined, recomputed by the same
-- subquery, so the row is again PROMO-classified.
UPDATE lineitem SET l_partkey =
    (SELECT p_partkey FROM part WHERE p_type LIKE 'PROMO%' ORDER BY p_partkey LIMIT 1)
WHERE l_comment = 'Q14S1';
