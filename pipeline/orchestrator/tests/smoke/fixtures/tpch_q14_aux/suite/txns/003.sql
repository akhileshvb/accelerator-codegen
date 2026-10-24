-- point_update / lineitem: the scratch row's (001) discount, a watched column: both sums move. 005
-- puts it back, so a stale maintenance that skipped the inverse delta shows up there.
UPDATE lineitem SET l_discount = 0.15 WHERE l_comment = 'Q14S1';
