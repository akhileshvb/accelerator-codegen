-- A row leaves and comes back in one transaction; a group loses its last row and another gains its first.
DELETE FROM num WHERE adsh = 'P-2';
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) VALUES ('P-2', 'T', 'v', 20930101, 4, 'TQ', 1.0);
DELETE FROM num WHERE adsh = 'P-4';
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) VALUES ('P-4', 'T', 'v', 20930101, 4, 'USD', 6.0), ('P-3', 'T', 'v', 20930101, 4, 'TQ', 0.25);
