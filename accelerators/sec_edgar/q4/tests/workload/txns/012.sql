-- 700 groups with the same total: the cut at 500 falls inside the tie.
INSERT INTO sub (adsh, cik, name, sic, fy, prevrpt)
    SELECT * FROM (VALUES ('TIE-1', 7400001, 'Tie 1', 9999, 2024, 0), ('TIE-2', 7400002, 'Tie 2', 9999, 2024, 0)) v(adsh, cik, name, sic, fy, prevrpt)
        WHERE NOT EXISTS (SELECT 1 FROM sub x WHERE x.adsh = v.adsh);
INSERT INTO tag (tag, version, custom, abstract, tlabel)
    SELECT 'Tie' || i, 'vT', 1, 0, 'Tie label ' || i FROM range(700) t(i) WHERE NOT EXISTS (SELECT 1 FROM tag x WHERE x.tag = 'Tie' || i AND x.version = 'vT');
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
    SELECT a, 'Tie' || i, 'vT', 20240101, 4, 'TIE', 1.0e300 FROM range(700) t(i), (VALUES ('TIE-1'), ('TIE-2')) s(a)
        WHERE NOT EXISTS (SELECT 1 FROM num x WHERE x.adsh = a AND x.tag = 'Tie' || i);
INSERT INTO pre (adsh, report, line, stmt, tag, version)
    SELECT a, 1, i, 'BS', 'Tie' || i, 'vT' FROM range(700) t(i), (VALUES ('TIE-1'), ('TIE-2')) s(a)
        WHERE NOT EXISTS (SELECT 1 FROM pre x WHERE x.adsh = a AND x.tag = 'Tie' || i);
