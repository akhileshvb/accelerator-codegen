-- NULLs: a tag without a label, a filing without a sic, a company without a number, values and statements missing.
INSERT INTO tag (tag, version, custom, abstract, tlabel) VALUES ('NullTagA', 'vN', 1, 0, NULL), ('NullTagB', 'vN', 1, 0, 'Has label');
INSERT INTO sub (adsh, cik, name, sic, fy, prevrpt) VALUES
    ('N-1', 7200001, 'Null Co 1', 4011, 2024, 0), ('N-2', 7200002, 'Null Co 2', 4011, 2024, 0), ('N-3', NULL, 'Null Co 3', 4011, 2024, 0);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) VALUES
    ('N-1', 'NullTagA', 'vN', 20240101, 4, 'USD', 1.5), ('N-2', 'NullTagA', 'vN', 20240101, 4, 'USD', 2.5), ('N-3', 'NullTagA', 'vN', 20240101, 4, 'USD', 3.5),
    ('N-1', 'NullTagB', 'vN', 20240101, 4, 'USD', NULL), ('N-2', 'NullTagB', 'vN', 20240101, 4, 'USD', 10.0), ('N-3', 'NullTagB', 'vN', 20240101, 4, 'USD', 20.0);
INSERT INTO pre (adsh, report, line, stmt, tag, version) VALUES
    ('N-1', 1, 1, 'IS', 'NullTagA', 'vN'), ('N-2', 1, 1, 'IS', 'NullTagA', 'vN'), ('N-3', 1, 1, 'IS', 'NullTagA', 'vN'),
    ('N-1', 1, 2, 'IS', 'NullTagB', 'vN'), ('N-2', 1, 2, NULL, 'NullTagB', 'vN'), ('N-3', 1, 2, 'IS', 'NullTagB', 'vN');
