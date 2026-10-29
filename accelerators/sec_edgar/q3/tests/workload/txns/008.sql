-- Rows filed twice (a repeated num row and a repeated sub row under another key).
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
    SELECT adsh, tag, version, ddate, qtrs, uom, value FROM num WHERE adsh IN ('Q-A', 'Q-R') AND uom = 'TQ';
INSERT INTO sub (adsh, cik, name, sic, fy, prevrpt) VALUES ('Q-R2', 8000005, 'Rounding', 1000, 2099, 0);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) VALUES ('Q-R2', 'T', 'v', 20990101, 4, 'TQ', 0.7);
