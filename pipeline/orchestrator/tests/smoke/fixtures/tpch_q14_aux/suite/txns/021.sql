-- bulk_update / lineitem: move the discount of the whole chunk of 020 by +0.01 in one statement, so
-- every row of the chunk has to be re-accumulated into its part/day summary exactly once.
UPDATE lineitem SET l_discount = l_discount + 0.01 WHERE l_shipmode = 'Q14RF1';
