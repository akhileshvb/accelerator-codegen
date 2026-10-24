INSERT INTO region SELECT source.* REPLACE (
  (SELECT max(r_regionkey) + 1 FROM region) AS r_regionkey,
  'q8-region' AS r_comment
) FROM region source WHERE r_name = 'AMERICA';
UPDATE nation SET n_regionkey = (SELECT r_regionkey FROM region WHERE r_comment = 'q8-region') WHERE n_comment = 'q8-nation';
UPDATE lineitem SET l_discount = 0.08 WHERE l_comment = 'q8-line-02-2';
