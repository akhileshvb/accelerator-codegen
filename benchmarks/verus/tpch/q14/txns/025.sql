INSERT INTO part
SELECT * REPLACE ('v14-duplicate' AS p_comment)
FROM part WHERE p_comment = 'v14-promo';
INSERT INTO part SELECT * FROM part WHERE p_comment = 'v14-duplicate';
INSERT INTO part
SELECT source.* REPLACE (fixture.type_text AS p_type, 'v14-twin' AS p_comment)
FROM (SELECT * FROM part WHERE p_comment = 'v14-promo') source
CROSS JOIN (VALUES ('PROMO'), ('PROMO '), ('promo POLISHED STEEL'),
                   ('Promo POLISHED STEEL'), (' PROMO POLISHED STEEL'),
                   ('PROMOé STEEL'), ('PROMOTION STEEL')) fixture(type_text);
INSERT INTO lineitem SELECT * FROM lineitem WHERE l_comment = 'v14-base-3';
INSERT INTO lineitem
SELECT * FROM lineitem WHERE l_comment = 'v14-base-3'
ORDER BY l_orderkey, l_linenumber LIMIT 1;
