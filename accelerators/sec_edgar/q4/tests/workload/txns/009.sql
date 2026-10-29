-- NULLs: a tag without a label, a filing without a sic, a company without a number, values and statements missing.
INSERT INTO tag (tag, version, custom, abstract, tlabel)
    SELECT * FROM (VALUES ('NullTagA', 'vN', 1, 0, NULL::VARCHAR), ('NullTagB', 'vN', 1, 0, 'Has label')) v(tag, version, custom, abstract, tlabel)
        WHERE NOT EXISTS (SELECT 1 FROM tag x WHERE x.tag = v.tag AND x.version = v.version);
INSERT INTO sub (adsh, cik, name, sic, fy, prevrpt)
    SELECT * FROM (VALUES ('N-1', 7200001, 'Null Co 1', 4011, 2024, 0), ('N-2', 7200002, 'Null Co 2', 4011, 2024, 0), ('N-3', NULL, 'Null Co 3', 4011, 2024, 0))
        v(adsh, cik, name, sic, fy, prevrpt) WHERE NOT EXISTS (SELECT 1 FROM sub x WHERE x.adsh = v.adsh);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
    SELECT * FROM (VALUES
        ('N-1', 'NullTagA', 'vN', 20240101, 4, 'USD', 1.5), ('N-2', 'NullTagA', 'vN', 20240101, 4, 'USD', 2.5), ('N-3', 'NullTagA', 'vN', 20240101, 4, 'USD', 3.5),
        ('N-1', 'NullTagB', 'vN', 20240101, 4, 'USD', NULL::DOUBLE), ('N-2', 'NullTagB', 'vN', 20240101, 4, 'USD', 10.0), ('N-3', 'NullTagB', 'vN', 20240101, 4, 'USD', 20.0))
        v(adsh, tag, version, ddate, qtrs, uom, value) WHERE NOT EXISTS (SELECT 1 FROM num x WHERE x.adsh = v.adsh AND x.tag = v.tag);
INSERT INTO pre (adsh, report, line, stmt, tag, version)
    SELECT * FROM (VALUES
        ('N-1', 1, 1, 'IS', 'NullTagA', 'vN'), ('N-2', 1, 1, 'IS', 'NullTagA', 'vN'), ('N-3', 1, 1, 'IS', 'NullTagA', 'vN'),
        ('N-1', 1, 2, 'IS', 'NullTagB', 'vN'), ('N-2', 1, 2, NULL::VARCHAR, 'NullTagB', 'vN'), ('N-3', 1, 2, 'IS', 'NullTagB', 'vN'))
        v(adsh, report, line, stmt, tag, version) WHERE NOT EXISTS (SELECT 1 FROM pre x WHERE x.adsh = v.adsh AND x.tag = v.tag);
