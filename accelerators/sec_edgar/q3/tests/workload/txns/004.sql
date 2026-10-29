-- The same filing arrives again (key reused) with a different value.
INSERT INTO sub (adsh, cik, name, sic, fy, prevrpt) VALUES ('Q-F', 8000008, 'Fco', 1000, 2099, 0);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) VALUES ('Q-F', 'T', 'v', 20990101, 4, 'TQ', 15.0);
