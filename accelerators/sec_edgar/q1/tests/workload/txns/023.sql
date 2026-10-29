-- Mass withdrawal: filings by one filer agent with their num and pre rows.
DELETE FROM pre WHERE adsh LIKE '0001104659-%';
DELETE FROM num WHERE adsh LIKE '0001104659-%';
DELETE FROM sub WHERE adsh LIKE '0001104659-%';
