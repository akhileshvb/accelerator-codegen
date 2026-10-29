-- a sub row changes name, then another changes fy: their groups move between units and classes
UPDATE sub SET name = 'RENAMED CO' WHERE adsh = (SELECT adsh FROM sub WHERE fy = 2023 AND adsh NOT LIKE 'e-%' AND adsh NOT LIKE 't-%' AND adsh NOT LIKE 'cp-%' ORDER BY adsh LIMIT 1);
UPDATE sub SET fy = 2022 WHERE adsh IN (SELECT adsh FROM sub WHERE fy = 2023 AND adsh NOT LIKE 'e-%' AND adsh NOT LIKE 't-%' AND adsh NOT LIKE 'cp-%' ORDER BY adsh LIMIT 2 OFFSET 5);
