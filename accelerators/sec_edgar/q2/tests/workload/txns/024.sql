-- After a trim: withdraw the filings of the top stream values, then amend one of the remaining.
DELETE FROM num WHERE adsh LIKE '00099%-24-000001' AND adsh >= '0009901000-24-000001' AND adsh < '0009901400-24-000001';
DELETE FROM sub WHERE adsh LIKE '00099%-24-000001' AND adsh >= '0009901000-24-000001' AND adsh < '0009901400-24-000001';
UPDATE num SET value = value * 2 WHERE adsh = '0009900007-24-000001';
