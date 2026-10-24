-- sequence (undo of 003): the scratch row back to its original discount, so the answer returns to what
-- it was before 003 (a stale-maintenance bug that skipped the inverse delta shows up here).
UPDATE lineitem SET l_discount = 0.05 WHERE l_comment = 'Q14S1';
