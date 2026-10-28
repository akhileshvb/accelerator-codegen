-- NULLs in every NULL-able cell the kernel reads, and doubles at the edges of the format.
UPDATE num SET ddate = NULL WHERE rowid % 11 = 0;
UPDATE num SET qtrs = NULL WHERE rowid % 17 = 0;
UPDATE num SET value = NULL WHERE rowid % 13 = 0;
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) VALUES
    ('edge-1', 'Zero', 'v', 20200101, 4, 'USD', 0.0),
    ('edge-2', 'NegativeZero', 'v', 20200101, 4, 'USD', -(0.0::DOUBLE)),
    ('edge-3', 'Subnormal', 'v', 20200101, 4, 'USD', 4.9406564584124654e-324),
    ('edge-4', 'Largest', 'v', 20200101, 4, 'USD', 1.7976931348623157e308),
    ('edge-5', 'Tiny', 'v', 20200101, 4, 'USD', -2.2250738585072014e-308),
    ('edge-6', 'NoDate', 'v', NULL, 4, 'USD', 1.5),
    ('edge-7', 'NoValue', 'v', 20200101, 4, 'USD', NULL),
    ('edge-8', 'NoQuarter', 'v', 20200101, NULL, 'USD', 2.5),
    (NULL, NULL, NULL, NULL, NULL, NULL, NULL);
