INSERT INTO part SELECT source.* REPLACE (
  (SELECT max(p_partkey) + 1 FROM part) AS p_partkey,
  'LARGE STEEL' AS p_type, 'q8-part-c' AS p_comment
) FROM part source WHERE p_comment = 'q8-part-a';
INSERT INTO lineitem SELECT source.* REPLACE (
  (SELECT p_partkey FROM part WHERE p_comment = 'q8-part-c') AS l_partkey,
  l_linenumber + 10 AS l_linenumber, l_extendedprice + 50.00 AS l_extendedprice,
  'q8-extra-part' AS l_comment
) FROM lineitem source WHERE l_comment IN ('q8-line-03-1', 'q8-line-04-2');
