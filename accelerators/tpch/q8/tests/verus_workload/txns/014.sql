INSERT INTO supplier SELECT source.* REPLACE (
  (SELECT max(s_suppkey) + 1 FROM supplier) AS s_suppkey,
  'q8-supplier-c' AS s_comment
) FROM supplier source WHERE s_comment = 'q8-supplier-a';
INSERT INTO lineitem SELECT source.* REPLACE (
  (SELECT s_suppkey FROM supplier WHERE s_comment = 'q8-supplier-c') AS l_suppkey,
  l_linenumber + 20 AS l_linenumber,
  'q8-extra-supplier' AS l_comment
) FROM lineitem source WHERE l_comment IN ('q8-line-05-1', 'q8-line-06-1');
