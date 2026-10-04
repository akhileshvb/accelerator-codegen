UPDATE lineitem SET l_quantity = CASE l_linenumber
  WHEN 1 THEN 300.01 WHEN 2 THEN -0.01 ELSE 0.00 END
WHERE l_comment = 'q18:line:01';
UPDATE lineitem SET l_quantity = CASE l_linenumber WHEN 1 THEN -0.01 ELSE 0.00 END
WHERE l_comment = 'q18:line:02';
UPDATE lineitem SET l_quantity = 0.00 WHERE l_comment = 'q18:line:03';
