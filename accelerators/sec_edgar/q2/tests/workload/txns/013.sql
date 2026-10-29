-- Mass withdrawal: all filings of one filer agent with their num rows.
DELETE FROM num WHERE adsh LIKE '0001104659-%';
DELETE FROM sub WHERE adsh LIKE '0001104659-%';
