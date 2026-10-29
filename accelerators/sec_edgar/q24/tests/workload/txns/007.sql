-- rows without a filing and with NULL tag/version appear and vanish
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) VALUES (NULL, 'NullAdsh', 'v', 20300630, 4, 'EDGE', 4.0), (NULL, NULL, NULL, 20300630, 4, 'EDGE', 1.0);
DELETE FROM num WHERE adsh IS NULL AND value = 2.0 AND tag = 'NullAdsh';
DELETE FROM num WHERE adsh = 'x-1' AND tag IS NULL;
