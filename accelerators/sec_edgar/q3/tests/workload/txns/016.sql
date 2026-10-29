-- The averaged companies change: one joins below, one leaves.
INSERT INTO sub (adsh, cik, name, sic, fy, prevrpt) VALUES ('Y5-4', 8300004, 'Y5 four', 1000, 2095, 0);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) VALUES ('Y5-4', 'T', 'v', 20950101, 4, 'TQ', 5.0);
DELETE FROM num WHERE adsh = 'Y4-1';
DELETE FROM sub WHERE adsh = 'Y4-1';
