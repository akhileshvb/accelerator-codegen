INSERT INTO nation SELECT source.* REPLACE (
  (SELECT max(n_nationkey) + 1 FROM nation) AS n_nationkey,
  'q8-nation' AS n_comment
) FROM nation source WHERE n_name = 'BRAZIL';
UPDATE customer SET c_nationkey = (SELECT n_nationkey FROM nation WHERE n_comment = 'q8-nation') WHERE c_comment = 'q8-customer-a';
UPDATE supplier SET s_nationkey = (SELECT n_nationkey FROM nation WHERE n_comment = 'q8-nation') WHERE s_comment = 'q8-supplier-a';
UPDATE lineitem SET l_extendedprice = l_extendedprice + 75.25 WHERE l_comment = 'q8-line-01-1';
