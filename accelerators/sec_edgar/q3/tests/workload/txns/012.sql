-- A real year: a dozen filings are removed and their companies' totals fall.
DELETE FROM num WHERE adsh IN (SELECT adsh FROM sub WHERE fy = 2023 ORDER BY adsh LIMIT 12);
DELETE FROM sub WHERE adsh IN (SELECT adsh FROM sub WHERE fy = 2023 ORDER BY adsh LIMIT 12);
