-- point_delete / part: remove the row 002 flipped. Its in-window lineitems lose the join, so both the
-- numerator and the denominator shrink: a dimension delete that must reweight the stored day sums.
DELETE FROM part WHERE p_comment = 'Q14UPD';
