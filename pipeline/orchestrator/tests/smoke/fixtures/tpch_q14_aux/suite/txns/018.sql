-- bulk_delete / lineitem: remove the whole chunk of 017 at once, the way an order-level refresh chunk
-- is withdrawn. The answer must return to its pre-017 value, so no per-row residue may stay behind.
DELETE FROM lineitem WHERE l_shipmode = 'Q14BIG';
