-- Back to one 'q6-boundary-7' row, so the table stays bounded (see 034)
DELETE FROM lineitem WHERE l_comment = 'q6-boundary-7' AND rowid > (SELECT min(rowid) FROM lineitem WHERE l_comment = 'q6-boundary-7');
