-- Doubles the 'q6-boundary-7' rows (see 034)
INSERT INTO lineitem SELECT * FROM lineitem WHERE l_comment = 'q6-boundary-7';
