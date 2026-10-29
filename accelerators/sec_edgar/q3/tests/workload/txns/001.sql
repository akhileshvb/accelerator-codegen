-- A filing batch (one sub row, its num rows): a new company above the average.
INSERT INTO sub (adsh, cik, name, sic, fy, prevrpt) VALUES ('Q-E', 8000007, 'Eco', 1000, 2099, 0), ('Q-F', 8000008, 'Fco', 1000, 2099, 0);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) VALUES
    ('Q-E', 'T', 'v', 20990101, 4, 'TQ', 30.0), ('Q-F', 'T', 'v', 20990101, 4, 'TQ', 14.0),
    ('Q-E', 'T', 'v', 20990101, 4, 'TR', 5.0);
