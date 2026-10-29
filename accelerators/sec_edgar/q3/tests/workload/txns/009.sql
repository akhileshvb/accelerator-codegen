-- Values that do not add exactly, and a total that equals the average (to the last bit).
INSERT INTO sub (adsh, cik, name, sic, fy, prevrpt) VALUES ('Q-X1', 8100001, 'X1', 1000, 2097, 0), ('Q-X2', 8100002, 'X2', 1000, 2097, 0), ('Q-X3', 8100003, 'X3', 1000, 2097, 0);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) VALUES
    ('Q-X1', 'T', 'v', 20970101, 4, 'TQ', 0.1), ('Q-X2', 'T', 'v', 20970101, 4, 'TQ', 0.2), ('Q-X3', 'T', 'v', 20970101, 4, 'TQ', 0.3),
    ('Q-X1', 'T', 'v', 20970101, 4, 'TQ', 1.0e16), ('Q-X1', 'T', 'v', 20970101, 4, 'TQ', 1.0), ('Q-X1', 'T', 'v', 20970101, 4, 'TQ', -1.0e16),
    ('Q-X2', 'T', 'v', 20970101, 4, 'TQ', 4.9e-324), ('Q-X3', 'T', 'v', 20970101, 4, 'TQ', -(0.0::DOUBLE)),
    ('Q-X3', 'T', 'v', 20970101, 4, 'TQ', 1.0e300), ('Q-X3', 'T', 'v', 20970101, 4, 'TQ', -1.0e300);
