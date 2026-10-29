-- a filing with two sub rows of different units
INSERT INTO sub SELECT * REPLACE ('DUP UNIT' AS name, 2022 AS fy) FROM sub WHERE fy = 2021 AND adsh NOT LIKE 'e-%' AND adsh NOT LIKE 't-%' AND adsh NOT LIKE 'cp-%' ORDER BY adsh LIMIT 2;
