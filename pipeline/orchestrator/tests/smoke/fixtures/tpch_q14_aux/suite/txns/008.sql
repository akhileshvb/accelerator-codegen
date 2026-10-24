-- point_delete / lineitem: remove the scratch row of 001, so the answer drops back to its pre-001
-- shape (the numerator and denominator shrink together). A maintenance that only ever adds is caught
-- here.
DELETE FROM lineitem WHERE l_comment = 'Q14S1';
