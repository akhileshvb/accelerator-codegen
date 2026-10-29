-- A tie for the maximum appears (the top rows are inserted again) and one copy disappears again.
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
SELECT adsh, tag, version, ddate, qtrs, uom, value FROM num WHERE uom = 'USD' AND adsh = '0001114446-24-001605' AND value > 5e11;
DELETE FROM num WHERE rowid = (SELECT max(rowid) FROM num WHERE uom = 'USD' AND adsh = '0001114446-24-001605' AND value > 5e11);
