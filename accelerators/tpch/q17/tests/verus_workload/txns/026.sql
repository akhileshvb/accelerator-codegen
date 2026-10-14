DELETE FROM part WHERE rowid = (SELECT MAX(rowid) FROM part WHERE p_name = 'q17-fixture-01');
DELETE FROM lineitem WHERE rowid = (SELECT MAX(rowid) FROM lineitem WHERE l_partkey = (SELECT p_partkey FROM part WHERE p_name = 'q17-fixture-01' LIMIT 1) AND l_linenumber = 1);
INSERT INTO part SELECT template.* REPLACE (variants.brand AS p_brand, variants.container AS p_container, 'q17-text-variant' AS p_name)
FROM part AS template CROSS JOIN (VALUES ('brand#23', 'WRAP BOX'), ('Brand#23 ', 'WRAP BOX'), ('Brand#23', 'WRAP BOX '), ('Brand#23', 'wrap box'), ('Bränd#23', 'WRAP BOX'), ('', '')) AS variants(brand, container)
WHERE template.p_name = 'q17-fixture-01';
