-- A tag becomes concrete again and gains rows for a unit the answers do not ask about.
UPDATE tag SET abstract = 0 WHERE tag = 'Rounding' AND version = 'vF';
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) VALUES ('F-H1', 'Rounding', 'vF', 20230101, 4, 'EUR', 99.5), ('F-H2', 'Rounding', 'vF', 20230101, 4, 'EUR', 100.5);
UPDATE pre SET stmt = 'BS' WHERE adsh = 'F-H1';
