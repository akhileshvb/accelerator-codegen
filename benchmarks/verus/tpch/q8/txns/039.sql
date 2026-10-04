UPDATE nation SET n_name = 'BRAZIL' WHERE n_name = 'BRAZIL ';
UPDATE region SET r_name = 'AMERICA' WHERE r_name = 'america';
UPDATE part SET p_type = 'Q8 EDGE' WHERE p_comment = 'q8-part-a';
UPDATE lineitem SET l_extendedprice = CASE l_comment
  WHEN 'q8-line-01-1' THEN 9999999999999.99
  WHEN 'q8-line-02-1' THEN -9999999999999.99
  ELSE 0.01 END,
  l_discount = CASE l_comment WHEN 'q8-line-01-1' THEN 0.00 WHEN 'q8-line-02-1' THEN 1.01 ELSE 1.00 END
WHERE l_partkey = (SELECT p_partkey FROM part WHERE p_comment = 'q8-part-a');
