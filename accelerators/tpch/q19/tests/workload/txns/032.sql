INSERT INTO lineitem SELECT * FROM lineitem WHERE l_comment = 'q19-line-01' AND l_linenumber = 1;
DELETE FROM part WHERE rowid = (SELECT min(rowid) FROM part WHERE p_name = 'q19-stock-01');
