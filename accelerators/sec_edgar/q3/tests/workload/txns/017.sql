-- Totals at powers of two and sums that cancel: a group of three rows, single rows, a zero total of three rows.
INSERT INTO sub (adsh, cik, name, sic, fy, prevrpt) VALUES
    ('P-1', 8600001, 'Pow one', 1000, 2093, 0), ('P-2', 8600002, 'Pow two', 1000, 2093, 0), ('P-3', 8600003, 'Pow three', 1000, 2093, 0),
    ('P-4', 8600004, 'Neg four', 1000, 2093, 0), ('P-5', 8600005, 'Neg five', 1000, 2093, 0);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) VALUES
    ('P-1', 'T', 'v', 20930101, 4, 'TQ', 3.0), ('P-1', 'T', 'v', 20930101, 4, 'TQ', 1.0), ('P-1', 'T', 'v', 20930101, 4, 'TQ', 0.5),
    ('P-2', 'T', 'v', 20930101, 4, 'TQ', 1.0),
    ('P-3', 'T', 'v', 20930101, 4, 'TQ', 1024.0), ('P-3', 'T', 'v', 20930101, 4, 'TQ', -1024.0), ('P-3', 'T', 'v', 20930101, 4, 'TQ', 0.0),
    ('P-4', 'T', 'v', 20930101, 4, 'TQ', -8.0),
    ('P-5', 'T', 'v', 20930101, 4, 'TQ', -3.0), ('P-5', 'T', 'v', 20930101, 4, 'TQ', -5.0);
