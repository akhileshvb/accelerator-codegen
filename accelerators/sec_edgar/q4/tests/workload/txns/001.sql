-- A filing batch: one sub row with its num and pre rows, plus the filer's custom tag.
INSERT INTO tag (tag, version, custom, abstract, tlabel) VALUES ('BatchTag', 'vB1', 1, 0, 'Batch label');
INSERT INTO sub (adsh, cik, name, sic, fy, prevrpt) VALUES
    ('B1-1', 7100001, 'Batch Co 1', 4011, 2024, 0), ('B1-2', 7100002, 'Batch Co 2', 4011, 2024, 0), ('B1-3', 7100003, 'Batch Co 3', 4011, 2024, 0);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) VALUES
    ('B1-1', 'BatchTag', 'vB1', 20240101, 4, 'USD', 1000000.0), ('B1-2', 'BatchTag', 'vB1', 20240101, 4, 'USD', 2500000.5),
    ('B1-3', 'BatchTag', 'vB1', 20240101, 4, 'USD', 125.0), ('B1-1', 'BatchTag', 'vB1', 20240101, 0, 'USD', 7.0);
INSERT INTO pre (adsh, report, line, stmt, tag, version) VALUES
    ('B1-1', 1, 1, 'EQ', 'BatchTag', 'vB1'), ('B1-2', 1, 1, 'EQ', 'BatchTag', 'vB1'), ('B1-3', 1, 1, 'EQ', 'BatchTag', 'vB1');
