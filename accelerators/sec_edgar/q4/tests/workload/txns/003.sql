-- A withdrawn filing: its pre and num rows, then its sub row, are deleted; a group falls below two companies.
DELETE FROM pre WHERE adsh = 'B1-3';
DELETE FROM num WHERE adsh = 'B1-3';
DELETE FROM sub WHERE adsh = 'B1-3';
