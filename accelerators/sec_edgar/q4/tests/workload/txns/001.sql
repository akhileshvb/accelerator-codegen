-- A filing batch: one sub row with its num and pre rows, plus the filer's custom tag (each row once, so repeated applications add nothing).
INSERT INTO tag (tag, version, custom, abstract, tlabel)
    SELECT 'BatchTag', 'vB1', 1, 0, 'Batch label' WHERE NOT EXISTS (SELECT 1 FROM tag x WHERE x.tag = 'BatchTag' AND x.version = 'vB1');
INSERT INTO sub (adsh, cik, name, sic, fy, prevrpt)
    SELECT * FROM (VALUES ('B1-1', 7100001, 'Batch Co 1', 4011, 2024, 0), ('B1-2', 7100002, 'Batch Co 2', 4011, 2024, 0), ('B1-3', 7100003, 'Batch Co 3', 4011, 2024, 0))
        v(adsh, cik, name, sic, fy, prevrpt) WHERE NOT EXISTS (SELECT 1 FROM sub x WHERE x.adsh = v.adsh);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
    SELECT * FROM (VALUES ('B1-1', 'BatchTag', 'vB1', 20240101, 4, 'USD', 1000000.0), ('B1-2', 'BatchTag', 'vB1', 20240101, 4, 'USD', 2500000.5),
        ('B1-3', 'BatchTag', 'vB1', 20240101, 4, 'USD', 125.0), ('B1-1', 'BatchTag', 'vB1', 20240101, 0, 'USD', 7.0)) v(adsh, tag, version, ddate, qtrs, uom, value)
        WHERE NOT EXISTS (SELECT 1 FROM num x WHERE x.adsh = v.adsh AND x.tag = v.tag AND x.qtrs = v.qtrs);
INSERT INTO pre (adsh, report, line, stmt, tag, version)
    SELECT * FROM (VALUES ('B1-1', 1, 1, 'EQ', 'BatchTag', 'vB1'), ('B1-2', 1, 1, 'EQ', 'BatchTag', 'vB1'), ('B1-3', 1, 1, 'EQ', 'BatchTag', 'vB1'))
        v(adsh, report, line, stmt, tag, version) WHERE NOT EXISTS (SELECT 1 FROM pre x WHERE x.adsh = v.adsh AND x.tag = v.tag);
