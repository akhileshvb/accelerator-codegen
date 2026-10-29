-- Duplicate rows (bag semantics), then one copy deleted.
INSERT INTO pre SELECT * FROM pre WHERE adsh = '0009990002-24-000001' AND report = 1 AND line = 1;
INSERT INTO pre SELECT * FROM pre WHERE adsh = '0009990002-24-000001' AND report = 1 AND line = 1 AND tag = 'NewTag0';
DELETE FROM pre WHERE rowid = (SELECT min(rowid) FROM pre WHERE adsh = '0009990002-24-000001' AND report = 1 AND line = 1 AND tag = 'NewTag0');
