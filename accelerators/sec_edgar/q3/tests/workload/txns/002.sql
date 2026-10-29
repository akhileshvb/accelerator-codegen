-- An amendment (a new filing for the same company) replaces the figures of the one it amends.
INSERT INTO sub (adsh, cik, name, sic, fy, prevrpt) VALUES ('Q-EA', 8000007, 'Eco', 1000, 2099, 0);
UPDATE sub SET prevrpt = 1 WHERE adsh = 'Q-E';
DELETE FROM num WHERE adsh = 'Q-E' AND uom = 'TQ';
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) VALUES ('Q-EA', 'T', 'v', 20990101, 4, 'TQ', 31.0);
