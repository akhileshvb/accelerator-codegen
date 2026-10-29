-- the filings of fiscal year 2021 are withdrawn but six: their classes keep fewer than 200 groups and refill from the frontier
DELETE FROM num WHERE adsh IN (SELECT adsh FROM sub WHERE fy = 2021 AND adsh NOT IN (SELECT adsh FROM sub WHERE fy = 2021 ORDER BY adsh LIMIT 6));
DELETE FROM pre WHERE adsh IN (SELECT adsh FROM sub WHERE fy = 2021 AND adsh NOT IN (SELECT adsh FROM sub WHERE fy = 2021 ORDER BY adsh LIMIT 6));
DELETE FROM sub WHERE fy = 2021 AND adsh NOT IN (SELECT adsh FROM sub WHERE fy = 2021 ORDER BY adsh LIMIT 6);
