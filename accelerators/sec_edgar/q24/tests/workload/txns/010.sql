-- withdrawals: a filing disappears with its num, pre and sub rows; the copies go too
DELETE FROM num WHERE adsh LIKE 'bb-%' OR adsh = 'cp-0001';
DELETE FROM pre WHERE adsh LIKE 'bb-%' OR adsh = 'cp-0001';
DELETE FROM sub WHERE adsh LIKE 'bb-%' OR adsh = 'cp-0001';
DELETE FROM num WHERE adsh = 'x-2';
DELETE FROM pre WHERE adsh = 'x-2';
