-- a withdrawal: the filing is deleted with its num and pre rows
DELETE FROM num WHERE adsh = 'cp-0001';
DELETE FROM pre WHERE adsh = 'cp-0001';
DELETE FROM sub WHERE adsh = 'cp-0001';
