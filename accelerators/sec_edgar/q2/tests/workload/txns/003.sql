-- Withdrawal: the biggest USD filing is deleted with its num rows.
DELETE FROM num WHERE adsh = (SELECT adsh FROM num WHERE uom = 'USD' AND adsh NOT LIKE '00099%' GROUP BY adsh ORDER BY count(*) DESC, adsh LIMIT 1);
DELETE FROM sub WHERE adsh NOT LIKE '00099%' AND adsh NOT IN (SELECT adsh FROM num WHERE adsh IS NOT NULL) AND adsh IN (SELECT adsh FROM sub ORDER BY adsh LIMIT 3);
