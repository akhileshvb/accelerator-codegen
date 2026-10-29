-- Repeated rows: a num row and a pre row filed twice.
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
    SELECT adsh, tag, version, ddate, qtrs, uom, value FROM num WHERE adsh = 'F-A1' AND tag = 'NullLabel';
INSERT INTO pre (adsh, report, line, stmt, tag, version)
    SELECT adsh, report, line + 10, stmt, tag, version FROM pre WHERE adsh = 'F-A2' AND tag = 'NullLabel';
