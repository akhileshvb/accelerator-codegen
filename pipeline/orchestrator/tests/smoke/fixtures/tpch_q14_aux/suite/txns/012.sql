-- sequence (redo/undo of 011): the row back inside the window on a day-15 date, so the daily summary
-- for the window must gain it back exactly where it left.
UPDATE lineitem SET l_shipdate = DATE '1995-06-15' WHERE l_comment = 'Q14M1';
