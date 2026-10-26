INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) VALUES
    ('t1', 'Dup', 'v', 20210101, 4, 'USD', 7.25), ('t1', 'Dup', 'v', 20210101, 4, 'USD', 7.25),
    ('t1', 'NullDate', 'v', NULL, 4, 'USD', -(0.0::DOUBLE)), ('t1', 'NullValue', 'v', 20210101, 4, 'USD', NULL),
    ('t1', 'NullBoth', 'v', NULL, 4, 'USD', NULL), ('t1', 'Other', 'v', 20210101, 3, 'USD', 1.0);
