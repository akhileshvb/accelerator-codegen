-- a group appears and vanishes inside one transaction; a filing arrives before its pre rows
INSERT INTO sub (adsh, cik, name, fy) VALUES ('t-1', 9, 'TRANSIENT', 2023);
INSERT INTO pre (adsh, report, line, stmt, tag, version, plabel) VALUES ('t-1', 1, 1, 'IS', 'Pow2', 'v', 'Transient');
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) VALUES ('t-1', 'Pow2', 'v', 20231231, 4, 'USD', 1.0);
DELETE FROM num WHERE adsh = 't-1';
INSERT INTO sub (adsh, cik, name, fy) VALUES ('t-2', 10, 'LATE PRE', 2023);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) VALUES ('t-2', 'Pow2', 'v', 20231231, 4, 'USD', 2.0);
