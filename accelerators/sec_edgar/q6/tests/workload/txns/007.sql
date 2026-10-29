-- value and label updates, a company renamed
UPDATE num SET value = value * 2 WHERE adsh = 'e-1' AND tag = 'Pow2';
UPDATE pre SET plabel = 'Renamed' WHERE adsh = 'e-3' AND tag = 'Dup';
UPDATE sub SET name = 'EDGE ONE RENAMED' WHERE adsh = 'e-5';
UPDATE sub SET fy = 2022 WHERE adsh = 'e-4';
