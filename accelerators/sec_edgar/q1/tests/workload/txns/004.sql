-- Withdrawal: a filing is deleted with its num and pre rows.
DELETE FROM pre WHERE adsh = (SELECT adsh FROM pre WHERE adsh NOT LIKE '00099%' GROUP BY adsh ORDER BY count(*) DESC, adsh LIMIT 1);
DELETE FROM num WHERE adsh = (SELECT adsh FROM sub WHERE adsh NOT LIKE '00099%' AND adsh NOT IN (SELECT adsh FROM pre) ORDER BY adsh LIMIT 1);
DELETE FROM sub WHERE adsh = (SELECT adsh FROM sub WHERE adsh NOT LIKE '00099%' AND adsh NOT IN (SELECT adsh FROM pre) ORDER BY adsh LIMIT 1);
