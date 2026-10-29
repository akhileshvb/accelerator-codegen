-- Half of the tied groups are deleted again.
DELETE FROM pre WHERE adsh LIKE 'TIE-%' AND tag IN (SELECT 'Tie' || i FROM range(0, 700, 2) t(i));
DELETE FROM num WHERE adsh LIKE 'TIE-%' AND tag IN (SELECT 'Tie' || i FROM range(0, 700, 2) t(i));
