-- A dozen existing filings are removed with all their rows (the foreign keys force the order).
DELETE FROM pre WHERE adsh IN (SELECT adsh FROM sub ORDER BY adsh LIMIT 12);
DELETE FROM num WHERE adsh IN (SELECT adsh FROM sub ORDER BY adsh LIMIT 12);
DELETE FROM sub WHERE adsh IN (SELECT adsh FROM sub ORDER BY adsh LIMIT 12);
