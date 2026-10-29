-- A sandbox year (2099) and units (TQ, TR) that the real data never uses, so the fixtures decide the answers:
-- company totals 10, 20, 15 (two names), 50 and a NULL company (25); names missing; a total that is an exact average.
INSERT INTO sub (adsh, cik, name, sic, fy, prevrpt) VALUES
    ('Q-A', 8000001, 'Aco', 1000, 2099, 0), ('Q-B', 8000002, 'Bco', 1000, 2099, 0),
    ('Q-C1', 8000003, 'Cco', 1000, 2099, 0), ('Q-C2', 8000003, 'Cco renamed', 1000, 2099, 0),
    ('Q-D', 8000004, NULL, 1000, 2099, 0), ('Q-N', NULL, 'Nameless company', 1000, 2099, 0),
    ('Q-R', 8000005, 'Rounding', 1000, 2099, 0), ('Q-NF', 8000006, 'No year', 1000, NULL, 0);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) VALUES
    ('Q-A', 'T', 'v', 20990101, 4, 'TQ', 4.0), ('Q-A', 'T', 'v', 20990101, 4, 'TQ', 6.0),
    ('Q-B', 'T', 'v', 20990101, 4, 'TQ', 20.0),
    ('Q-C1', 'T', 'v', 20990101, 4, 'TQ', 7.5), ('Q-C2', 'T', 'v', 20990101, 4, 'TQ', 7.5),
    ('Q-D', 'T', 'v', 20990101, 4, 'TQ', 50.0), ('Q-N', 'T', 'v', 20990101, 4, 'TQ', 25.0),
    ('Q-R', 'T', 'v', 20990101, 4, 'TQ', 0.1), ('Q-R', 'T', 'v', 20990101, 4, 'TQ', 0.2), ('Q-R', 'T', 'v', 20990101, 4, 'TQ', 0.3),
    ('Q-NF', 'T', 'v', 20990101, 4, 'TQ', 5000.0), ('Q-A', 'T', 'v', 20990101, 4, 'TQ', NULL),
    ('Q-A', 'T', 'v', 20990101, 4, 'TR', 1.0), ('Q-B', 'T', 'v', 20990101, 4, 'TR', 2.0);
-- Years whose average is a total: 10, 20, 30 (average 20) and 1, 1, 2, 4/3 (average 4/3, inexact).
INSERT INTO sub (adsh, cik, name, sic, fy, prevrpt) VALUES
    ('Y5-1', 8300001, 'Y5 one', 1000, 2095, 0), ('Y5-2', 8300002, 'Y5 two', 1000, 2095, 0), ('Y5-3', 8300003, 'Y5 three', 1000, 2095, 0),
    ('Y4-1', 8400001, 'Y4 one', 1000, 2094, 0), ('Y4-2', 8400002, 'Y4 two', 1000, 2094, 0), ('Y4-3', 8400003, 'Y4 three', 1000, 2094, 0), ('Y4-4', 8400004, 'Y4 four', 1000, 2094, 0);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) VALUES
    ('Y5-1', 'T', 'v', 20950101, 4, 'TQ', 10.0), ('Y5-2', 'T', 'v', 20950101, 4, 'TQ', 20.0), ('Y5-3', 'T', 'v', 20950101, 4, 'TQ', 30.0),
    ('Y4-1', 'T', 'v', 20940101, 4, 'TQ', 1.0), ('Y4-2', 'T', 'v', 20940101, 4, 'TQ', 1.0), ('Y4-3', 'T', 'v', 20940101, 4, 'TQ', 2.0),
    ('Y4-4', 'T', 'v', 20940101, 4, 'TQ', 1.3333333333333333);
