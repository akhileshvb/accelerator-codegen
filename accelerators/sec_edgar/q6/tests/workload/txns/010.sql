-- everything of the edge filings goes
DELETE FROM num WHERE adsh LIKE 'e-%' OR adsh LIKE 't-%';
DELETE FROM pre WHERE adsh LIKE 'e-%' OR adsh LIKE 't-%';
DELETE FROM sub WHERE adsh LIKE 'e-%' OR adsh LIKE 't-%';
