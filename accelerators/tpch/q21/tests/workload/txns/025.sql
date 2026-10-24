-- Correction: the second delivery was on time after all.
UPDATE lineitem SET l_receiptdate = DATE '1996-01-05'
WHERE l_comment = 'q21:ordinary:line' AND l_linenumber = 2;
